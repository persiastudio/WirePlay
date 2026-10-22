// Cross-process frame bus: how decoded frames get from wireplay.exe to the
// DirectShow virtual camera.
//
// One producer, one consumer, "latest frame wins". The camera pulls at the
// graph's frame rate and the app pushes as fast as it decodes, so a queue
// would only grow; when the consumer is behind it skips to the newest frame
// rather than playing catch-up. That is the behaviour OBS wants, because a
// virtual camera that lags behind the phone is worse than one that drops.
//
// The layout is identical on both sides of the process boundary and contains
// no pointers, so it can live in a memory-mapped section.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
// windows.h defines min/max as macros, which turns every `std::min(a, b)` into
// a syntax error (C2589) in any file that includes this header. It also pulls
// in most of the Win32 API, none of which is needed here.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace wp {

constexpr uint32_t kFrameBusMagic = 0x57504642;  // "WPFB"
constexpr uint32_t kFrameBusVersion = 1;

enum BusPixelFormat : uint32_t {
    kBusFormatNone = 0,
    kBusFormatBGRA32 = 1,  // what the decoder emits and what the camera wants
};

struct BusHeader {
    uint32_t magic;
    uint32_t version;

    // Written by the producer under the lock.
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t fps_num;
    uint32_t fps_den;
    uint64_t published_count;
    uint64_t last_publish_ns;  // producer clock, for staleness checks only

    // Written by the consumer under the lock.
    uint64_t consumed_count;

    uint32_t slot_stride;  // bytes per row of the slot buffer
    uint32_t slot_size;    // bytes of pixel data in one slot
    uint32_t slot_offset;  // byte offset of the slot buffer from the section base

    // Producer identity, so a successor can tell "another instance is running"
    // from "the previous one crashed and left the section behind".
    uint32_t producer_pid;
    uint32_t reserved_[5];
    uint64_t reserved[6];
};

// Both processes must agree on this; it bounds the resolution the bus can
// carry, and it has to be big enough for a phone held the long way round.
// An iPhone 14/15 Pro Max mirrors at 1290x2796, so 2796 is a real height, not
// a theoretical one; 4096 also covers the 13" iPad at 2752x2064. The worst case
// is a ~63 MB section, allocated on demand rather than touched.
constexpr uint32_t kBusMaxStride = 3840 * 4;
constexpr uint32_t kBusMaxHeight = 4096;
constexpr uint32_t kBusSlotBytes = kBusMaxStride * kBusMaxHeight;

// The mutex lives in the mapping itself, right after the header, so it works
// across the process boundary. Slots start on the next page after that. These
// live here rather than in the .cpp so that both halves of the platform ifdef
// and every translation unit see exactly one definition.
inline constexpr size_t kSyncOffset = sizeof(BusHeader);
inline constexpr size_t kSlotOffsetAligned = (kSyncOffset + 256 + 0xFFF) & ~size_t(0xFFF);

// Total section size, header included. Exposed so the camera and the app size
// their mapping identically.
constexpr size_t kFrameBusSectionSize =
    ((sizeof(BusHeader) + 0xFFF) & ~size_t(0xFFF)) + kBusSlotBytes;

class FrameBus {
public:
    FrameBus() = default;
    ~FrameBus();
    FrameBus(const FrameBus&) = delete;
    FrameBus& operator=(const FrameBus&) = delete;

    // Producer side. Fails if another producer already owns the name.
    bool create(const std::string& name);

    // Consumer side. Fails until the producer has created it.
    bool open(const std::string& name);

    void close();

    // Producer only: forget the name as well as the mapping. The real bus is
    // meant to outlive a restart so the camera can reconnect immediately, but
    // tests would otherwise leave one section per run behind in /dev/shm.
    void abandon();

    bool valid() const { return header_ != nullptr; }

    // Producer: publish one tightly packed BGRA frame. `stride` may differ
    // from width*4 (FFmpeg aligns rows); rows are copied individually.
    // Returns false if the frame does not fit the bus or the bus is not open.
    bool publish(const uint8_t* bgra, int width, int height, int stride, uint32_t fps_num,
                 uint32_t fps_den);

    // Consumer: copy the newest published frame into `dst`, which must hold at
    // least (*out_stride) * (*out_height) bytes. Returns false if nothing has
    // been published yet or the frame changed shape mid-copy.
    // `out_seq`, when given, receives the published-frame counter of the copy,
    // so a caller can tell a new frame from the one it already delivered.
    bool consumeLatest(uint8_t* dst, int* out_width, int* out_height, int* out_stride,
                       uint64_t* out_seq = nullptr);

    // Consumer: block until the producer has published a frame newer than
    // `seen`, or `timeout_ms` has passed. Returns the current published-frame
    // counter (equal to `seen` on a timeout). On Windows the producer signals
    // a named event on every publish, so the wake-up is immediate and does not
    // depend on the system timer resolution; the counter, not the event, is
    // what decides, so a missed or shared wake-up only costs a short re-check.
    uint64_t waitForFrame(uint64_t seen, uint32_t timeout_ms);

    // True when the producer has not published for `timeout_ms`. Lets the
    // camera show "waiting" instead of a frozen last frame.
    bool stale(uint64_t timeout_ms) const;

    uint32_t producerPid() const;
    uint64_t published() const { return header_ ? header_->published_count : 0; }
    uint32_t width() const { return header_ ? header_->width : 0; }
    uint32_t fpsNum() const { return header_ ? header_->fps_num : 0; }
    uint32_t fpsDen() const { return header_ ? header_->fps_den : 0; }
    uint32_t height() const { return header_ ? header_->height : 0; }

    const std::string& error() const { return error_; }

private:
    void lock();
    void unlock();
    uint8_t* slot() const;

    BusHeader* header_ = nullptr;
    uint8_t* base_ = nullptr;
    std::string error_;
    bool owns_ = false;
    std::string name_;
#ifdef _WIN32
    HANDLE mapping_ = nullptr;
    HANDLE mutex_ = nullptr;
    HANDLE frame_event_ = nullptr;  // auto-reset, "<name>.frame": a publish happened
#else
    int fd_ = -1;
    pthread_mutex_t* shm_mutex_ = nullptr;
    pthread_mutexattr_t attr_ {};
    bool attr_init_ = false;
#endif
};

// The section name is fixed: there is one phone mirror and one camera.
//
// The "Local\\" prefix is Win32 namespace syntax for a per-session object. It
// is not a path separator and POSIX does not understand it -- passing it to
// shm_open creates a file literally named "Local\\WirePlayFrameBus" in /dev/shm,
// which the producer can reopen and the consumer can never find. So the name
// differs per platform, and there is a test pinning that down.
#ifdef _WIN32
inline const char* defaultBusName() { return "Local\\WirePlayFrameBus"; }
#else
inline const char* defaultBusName() { return "WirePlayFrameBus"; }
#endif

uint64_t monotonicNowMs();

// Our own process id, used as the producer identity so a successor can tell a
// live instance from a crashed one.
uint32_t currentPid();

}  // namespace wp
