#include "wp/camera_types.hpp"

#include <cstddef>
#include <cstdio>
#include <vector>
#include <cstdlib>

// Media-type negotiation is the one part of the DirectShow filter whose
// behaviour is knowable without Windows: a wrong answer here is why OBS shows a
// black screen or refuses to list the camera at all, and it is far easier to
// catch in a unit test than by staring at a broken preview.

namespace {

[[noreturn]] void fail(const char* what, int line) {
    std::fprintf(stderr, "  FAIL line %d: %s\n", line, what);
    std::exit(1);
}

#define REQUIRE(cond)                     \
    do {                                  \
        if (!(cond)) fail(#cond, __LINE__); \
    } while (0)

void testBitmapHeaderLayout() {
    std::printf("[ RUN ] camera: BITMAPINFOHEADER layout\n");
    // 40 bytes is the ABI contract; if this ever changes, every consumer
    // reading our format block reads garbage.
    REQUIRE(sizeof(wp::BitmapInfoHeader) == 40);
    REQUIRE(offsetof(wp::BitmapInfoHeader, biWidth) == 4);
    REQUIRE(offsetof(wp::BitmapInfoHeader, biBitCount) == 14);
    REQUIRE(offsetof(wp::BitmapInfoHeader, biCompression) == 16);
    REQUIRE(offsetof(wp::BitmapInfoHeader, biSizeImage) == 20);

    const wp::FrameFormat f {1290, 2796, 60, 1};
    const wp::BitmapInfoHeader h = wp::makeBitmapInfoHeader(f);
    REQUIRE(h.biSize == 40);
    REQUIRE(h.biWidth == 1290);
    REQUIRE(h.biHeight == 2796);  // bottom-up, not top-down
    REQUIRE(h.biPlanes == 1);
    REQUIRE(h.biBitCount == 32);
    REQUIRE(h.biCompression == 0);  // BI_RGB
    REQUIRE(h.biSizeImage == size_t(1290) * 2796 * 4);
    REQUIRE(wp::frameBytes(f) == size_t(1290) * 2796 * 4);
    std::printf("[  OK ] camera: BITMAPINFOHEADER layout (40 bytes, %u image bytes)\n",
                h.biSizeImage);
}

void testFrameInterval() {
    std::printf("[ RUN ] camera: frame interval\n");
    REQUIRE(wp::frameInterval100ns({0, 0, 60, 1}) == 166666);
    REQUIRE(wp::frameInterval100ns({0, 0, 30, 1}) == 333333);
    REQUIRE(wp::frameInterval100ns({0, 0, 120, 1}) == 83333);
    REQUIRE(wp::frameInterval100ns({0, 0, 60000, 1001}) == 166833);  // 59.94
    REQUIRE(wp::frameInterval100ns({0, 0, 0, 1}) == 0);   // no divide by zero
    REQUIRE(wp::frameInterval100ns({0, 0, 60, 0}) == 0);
    REQUIRE(wp::frameBytes({-1, 100, 60, 1}) == 0);
    std::printf("[  OK ] camera: frame interval\n");
}

void testFpsTolerance() {
    std::printf("[ RUN ] camera: frame-rate tolerance\n");
    REQUIRE(wp::fpsCloseEnough(60, 1, 60));
    REQUIRE(wp::fpsCloseEnough(60000, 1001, 60));  // NTSC 59.94 must pass as 60
    REQUIRE(wp::fpsCloseEnough(30000, 1001, 30));
    REQUIRE(!wp::fpsCloseEnough(50, 1, 60));       // PAL is not NTSC
    REQUIRE(!wp::fpsCloseEnough(120, 1, 60));      // and ProMotion is not halved
    REQUIRE(!wp::fpsCloseEnough(0, 1, 60));
    REQUIRE(!wp::fpsCloseEnough(60, 1, 0));
    REQUIRE(!wp::fpsCloseEnough(60, 0, 60));
    std::printf("[  OK ] camera: frame-rate tolerance\n");
}

void testJudgement() {
    std::printf("[ RUN ] camera: media-type judgement\n");
    using wp::kAccept;
    using wp::kAcceptWithOurSize;
    using wp::kReject;

    const wp::FrameFormat none {0, 0, 0, 0};  // no device attached yet

    // Before a device is attached the advertised default is the contract.
    REQUIRE(wp::judgeMediaType(true, true, {1080, 1920, 60, 1}, none) == kAccept);
    // Wrong shape entirely: we do not do anything but 32-bit RGB video.
    REQUIRE(wp::judgeMediaType(false, true, {1080, 1920, 60, 1}, none) == kReject);
    REQUIRE(wp::judgeMediaType(true, false, {1080, 1920, 60, 1}, none) == kReject);
    REQUIRE(wp::judgeMediaType(true, true, {0, 1920, 60, 1}, none) == kReject);
    REQUIRE(wp::judgeMediaType(true, true, {1080, -5, 60, 1}, none) == kReject);

    // Right geometry, unsupported rate: accept the geometry but insist on ours.
    REQUIRE(wp::judgeMediaType(true, true, {1080, 1920, 25, 1}, none) == kAcceptWithOurSize);
    // Right geometry, 59.94: close enough to 60 to take as-is.
    REQUIRE(wp::judgeMediaType(true, true, {1080, 1920, 60000, 1001}, none) == kAccept);

    // Once the phone is attached, its geometry is the only correct answer.
    const wp::FrameFormat phone {1290, 2796, 60, 1};
    REQUIRE(wp::judgeMediaType(true, true, {1290, 2796, 60, 1}, phone) == kAccept);
    REQUIRE(wp::judgeMediaType(true, true, {1080, 1920, 60, 1}, phone) == kAcceptWithOurSize);
    REQUIRE(wp::judgeMediaType(true, true, {2796, 1290, 60, 1}, phone) == kAcceptWithOurSize);
    std::printf("[  OK ] camera: media-type judgement\n");
}

void testAdvertisedRatesCoverTheDevices() {
    std::printf("[ RUN ] camera: advertised rates cover real devices\n");
    size_t n = 0;
    const int* rates = wp::advertisedFps(&n);
    REQUIRE(n >= 4);
    bool has30 = false, has60 = false, has120 = false;
    for (size_t i = 0; i < n; ++i) {
        if (rates[i] == 30) has30 = true;
        if (rates[i] == 60) has60 = true;
        if (rates[i] == 120) has120 = true;
        REQUIRE(rates[i] > 0);
    }
    REQUIRE(has30 && has60 && has120);
    std::printf("[  OK ] camera: advertised rates cover real devices (%zu rates)\n", n);
}

// The classic upside-down-phone bug. Verified explicitly because a preview
// that is merely flipped still looks like it "works".
void testRowFlip() {
    std::printf("[ RUN ] camera: bottom-up row flip\n");
    const int w = 2, h = 3;
    const size_t row = size_t(w) * 4;
    std::vector<uint8_t> src(row * h), dst(row * h, 0);
    for (int y = 0; y < h; ++y)
        for (size_t x = 0; x < row; ++x) src[size_t(y) * row + x] = uint8_t(y + 1);

    wp::copyTopDownToBottomUp(dst.data(), src.data(), w, h, row);
    // Top-down row 0 (value 1) must land last in memory.
    for (size_t x = 0; x < row; ++x) {
        REQUIRE(dst[wp::bottomUpRowOffset(0, h, row) + x] == 1);
        REQUIRE(dst[wp::bottomUpRowOffset(1, h, row) + x] == 2);
        REQUIRE(dst[wp::bottomUpRowOffset(2, h, row) + x] == 3);
    }
    REQUIRE(dst[0] == 3);
    REQUIRE(dst[row * (h - 1)] == 1);

    // A padded source stride must not smear rows into each other.
    const size_t padded = row + 8;
    std::vector<uint8_t> src2(padded * h, 0xEE), dst2(row * h, 0);
    for (int y = 0; y < h; ++y)
        for (size_t x = 0; x < row; ++x) src2[size_t(y) * padded + x] = uint8_t(y + 1);
    wp::copyTopDownToBottomUp(dst2.data(), src2.data(), w, h, padded);
    for (size_t i = 0; i < dst2.size(); ++i) REQUIRE(dst2[i] != 0xEE);
    REQUIRE(dst2[0] == 3);

    // Degenerate inputs must not write.
    wp::copyTopDownToBottomUp(nullptr, src.data(), w, h, row);
    wp::copyTopDownToBottomUp(dst.data(), nullptr, w, h, row);
    wp::copyTopDownToBottomUp(dst.data(), src.data(), 0, h, row);
    wp::copyTopDownToBottomUp(dst.data(), src.data(), w, 0, row);
    REQUIRE(wp::bottomUpRowOffset(-1, h, row) == 0);
    REQUIRE(wp::bottomUpRowOffset(h, h, row) == 0);
    std::printf("[  OK ] camera: bottom-up row flip\n");
}

}  // namespace

int main() {
    testBitmapHeaderLayout();
    testFrameInterval();
    testFpsTolerance();
    testJudgement();
    testAdvertisedRatesCoverTheDevices();
    testRowFlip();
    std::printf("\ncamera types: all checks passed\n");
    return 0;
}
