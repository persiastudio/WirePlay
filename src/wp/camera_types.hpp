// Media types the virtual camera offers, and the rules for accepting one.
//
// This file deliberately contains no Windows and no COM headers. DirectShow
// filters are the part of this project that cannot be built or run in CI, so
// everything that is pure logic lives here where a normal test can reach it.
// dshow_filter.cpp includes this file and nothing else decides types.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wp {

struct FrameFormat {
    int width = 0;
    int height = 0;
    int fps_num = 60;
    int fps_den = 1;
};

// What the camera offers before it knows anything about the phone. OBS and
// Streamlabs read the filter's media types at enumeration time, long before a
// device is attached, so there has to be a usable answer already; the filter
// then forces a reconnect to the real resolution once the first frame lands.
inline FrameFormat defaultCameraFormat() { return {1080, 1920, 60, 1}; }

// The frame rates worth advertising. 120 is here because the ProMotion models
// really do mirror at 120, and a camera that cannot express the source rate
// will silently halve it.
inline const int* advertisedFps(size_t* count) {
    static const int kRates[] = {30, 50, 60, 120};
    *count = sizeof(kRates) / sizeof(kRates[0]);
    return kRates;
}

// Frame rates that an exact device rate may round to, so that a phone doing
// 59.94 is offered 60 instead of being rejected outright.
inline bool fpsCloseEnough(int num, int den, int target) {
    if (den <= 0 || target <= 0) return false;
    const double a = double(num) / double(den);
    const double b = double(target);
    const double d = a > b ? a - b : b - a;
    return d <= b * 0.02;  // 2%: enough for 59.94 vs 60, not enough for 50 vs 60
}

// Bytes of pixel data for one frame at this format.
inline size_t frameBytes(const FrameFormat& f) {
    if (f.width <= 0 || f.height <= 0) return 0;
    return size_t(f.width) * size_t(f.height) * 4;
}

// 100-nanosecond units per frame, which is DirectShow's time base. Returns 0
// for a degenerate rate rather than dividing by zero.
inline int64_t frameInterval100ns(const FrameFormat& f) {
    if (f.fps_num <= 0 || f.fps_den <= 0) return 0;
    return (int64_t(10000000) * f.fps_den) / f.fps_num;
}

// BITMAPINFOHEADER, spelled out so this file needs no Windows headers. The
// field order and widths are fixed by the Win32 ABI.
struct BitmapInfoHeader {
    uint32_t biSize;
    int32_t biWidth;
    int32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t biXPelsPerMeter;
    int32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};

constexpr uint32_t kBiRgb = 0;

inline BitmapInfoHeader makeBitmapInfoHeader(const FrameFormat& f) {
    BitmapInfoHeader h {};
    h.biSize = uint32_t(sizeof(BitmapInfoHeader));
    h.biWidth = f.width;
    // Positive height = bottom-up, which is what RGB DIBs from a capture
    // source are expected to be. A negative value would mean top-down and
    // some consumers mirror the image vertically.
    h.biHeight = f.height;
    h.biPlanes = 1;
    h.biBitCount = 32;
    h.biCompression = kBiRgb;
    h.biSizeImage = uint32_t(frameBytes(f));
    return h;
}

// The verdict on a media type a consumer proposes.
enum TypeVerdict {
    kAccept = 0,        // use it as-is
    kAcceptWithOurSize,  // right shape, but the geometry has to come from us
    kReject,
};

// Decide what to do with a proposed media type. The consumer may propose
// anything: a size the phone cannot produce, a bit depth we do not emit, a
// frame rate we cannot hit. The camera's resolution is whatever the phone
// sends, so it is not negotiable; the frame rate is, within the advertised set.
inline TypeVerdict judgeMediaType(bool is_video, bool is_rgb32, const FrameFormat& proposed,
                                  const FrameFormat& source) {
    if (!is_video || !is_rgb32) return kReject;
    if (proposed.width <= 0 || proposed.height <= 0) return kReject;
    // We cannot know the phone's resolution before it is attached. Until then
    // the advertised default is the contract, and a consumer matching it is
    // right.
    const FrameFormat expected =
        (source.width > 0 && source.height > 0) ? source : defaultCameraFormat();
    if (proposed.width != expected.width || proposed.height != expected.height) {
        return kAcceptWithOurSize;
    }
    size_t n = 0;
    const int* rates = advertisedFps(&n);
    for (size_t i = 0; i < n; ++i) {
        if (fpsCloseEnough(proposed.fps_num, proposed.fps_den, rates[i])) return kAccept;
    }
    return kAcceptWithOurSize;
}

// A capture source hands out bottom-up DIBs, but the frame bus holds top-down
// rows, so every copy flips. Getting this wrong produces an upside-down phone
// in OBS, which is easy to ship by accident and annoying to spot in a preview,
// so the row mapping is a function rather than an inline loop.
//
// Returns the byte offset within a bottom-up image for a top-down row `y`.
inline size_t bottomUpRowOffset(int y, int height, size_t stride) {
    if (y < 0 || y >= height || height <= 0 || stride == 0) return 0;
    return size_t(height - 1 - y) * stride;
}

// Copy `height` rows from a top-down source into a bottom-up destination.
// `src` and `dst` must not overlap.
inline void copyTopDownToBottomUp(uint8_t* dst, const uint8_t* src, int width, int height,
                                  size_t src_stride) {
    if (!dst || !src || width <= 0 || height <= 0) return;
    const size_t row = size_t(width) * 4;
    for (int y = 0; y < height; ++y) {
        uint8_t* d = dst + bottomUpRowOffset(y, height, row);
        const uint8_t* s = src + size_t(y) * src_stride;
        for (size_t i = 0; i < row; ++i) d[i] = s[i];
    }
}

}  // namespace wp
