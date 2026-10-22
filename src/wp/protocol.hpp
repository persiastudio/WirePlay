// WirePlay — frame protocol shared with the GPL engine process.
//
// Must stay in sync with docs/WIRE_PROTOCOL.md and
// engine/wireplay_engine.py. A change to one without the other is a
// silent-corruption bug, so engine/tests/test_wire_protocol.py asserts the
// numeric values match.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wp {

// Engine -> app
enum EngineMsg : uint8_t {
    kStatus    = 0x01,
    kFatal     = 0x02,
    kStats     = 0x04,
    kDevice    = 0x08,
    kVideoInit = 0x10,
    kVideoAU   = 0x11,
    kAudioInit = 0x20,
    kAudioAU   = 0x21,
    // Raw PCM has no AudioSpecificConfig to send, so it cannot ride kAudioInit
    // (whose payload is defined as an ASC). QuickTime audio uses this instead;
    // the AUs themselves still travel as kAudioAU.
    kAudioPcmInit = 0x22,
    kPing      = 0xFE,
    kEnd       = 0xFF,
};

// App -> engine
enum AppMsg : uint8_t {
    kPong         = 0x81,
    kRequestKey   = 0x82,
    kRotate       = 0x83,
    kRestartStream = 0x84,
    kTouch        = 0x85,
    kButton       = 0x86,
    kSetBitrate   = 0x87,
    kAppPing      = 0xFE,
    kAppEnd       = 0xFF,
};

constexpr size_t kMaxFrame = 64u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// Which codec the video access units carry.
// ---------------------------------------------------------------------------
// Declared by the engine in the kVideoInit header rather than guessed from the
// bitstream, because CoreDevice streams HEVC and the QuickTime USB protocol
// streams H.264, and the decoder has to be constructed before the first
// access unit arrives. Values are wire constants: they are pinned by
// engine/tests/test_wire_protocol.py on the Python side.
enum VideoCodec : uint32_t {
    kCodecHevc = 0,
    kCodecH264 = 1,
};

// ---------------------------------------------------------------------------
// One decoded frame off the pipe.
// ---------------------------------------------------------------------------
struct Frame {
    uint8_t type = 0;
    // payload points into the FrameReader's reusable buffer; valid until the
    // next read() call. Never hold on to it across reads.
    const uint8_t* payload = nullptr;
    size_t size = 0;

    // Parsed views, filled by the accessors below.
    bool isJson() const {
        return type == kStatus || type == kFatal || type == kStats || type == kDevice;
    }
    std::string asString() const { return {reinterpret_cast<const char*>(payload), size}; }
};

// ---------------------------------------------------------------------------
// Blocking reader with an optional "is anything pending?" poll.
// ---------------------------------------------------------------------------
class FrameReader {
public:
    explicit FrameReader(FILE* f) : f_(f) {}

    // False on EOF or a framing error (the engine is gone or wedged).
    bool read(Frame& out);

    // True when at least one byte is buffered/available, so the caller can
    // drain the pipe without blocking. Platform-specific.
    bool hasPending() const;

    uint64_t frames_read = 0;

private:
    bool readFully(void* dst, size_t n);
    FILE* f_ = nullptr;
    std::vector<uint8_t> buf_;
};

// Writes one frame to a FILE* opened in binary mode.
bool writeFrame(FILE* f, uint8_t type, const void* payload, size_t size);
inline bool writeFrame(FILE* f, uint8_t type) { return writeFrame(f, type, nullptr, 0); }

// Parses "[u32be a][u32be b]..." little helpers used by the video/audio init
// messages. Returns false when the payload is too short.
bool readU32BE(const uint8_t*& p, size_t& remaining, uint32_t& out);

// Forces a FILE* to byte-exact mode. On Windows the C runtime otherwise
// translates 0x0A into 0x0D 0x0A on write, which silently corrupts a binary
// frame stream. Must be called on stdin/stdout before any frame I/O.
void setBinaryMode(FILE* f);

}  // namespace wp
