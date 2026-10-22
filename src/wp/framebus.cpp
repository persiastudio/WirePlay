#include "wp/framebus.hpp"

// The POSIX half of this file needs mmap, shm_open and friends; the Windows
// half is entirely inside `#ifdef _WIN32` further down. Including these
// unconditionally is what broke the MSVC build.
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>

namespace wp {

uint64_t monotonicNowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t currentPid() {
#ifdef _WIN32
    return uint32_t(GetCurrentProcessId());
#else
    return uint32_t(::getpid());
#endif
}

// Is a process with this id still alive? Used to decide whether an existing
// bus belongs to a running WirePlay or is wreckage from one that was killed.
static bool processAlive(uint32_t pid) {
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return false;  // no access, or it is gone: either way not ours to spare
    const DWORD code = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return code == WAIT_TIMEOUT;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

FrameBus::~FrameBus() { close(); }

uint8_t* FrameBus::slot() const { return base_ + kSlotOffsetAligned; }

void FrameBus::lock() {
#ifdef _WIN32
    if (mutex_) WaitForSingleObject(mutex_, INFINITE);
#else
    if (shm_mutex_) pthread_mutex_lock(shm_mutex_);
#endif
}

void FrameBus::unlock() {
#ifdef _WIN32
    if (mutex_) ReleaseMutex(mutex_);
#else
    if (shm_mutex_) pthread_mutex_unlock(shm_mutex_);
#endif
}

#ifdef _WIN32

bool FrameBus::create(const std::string& name) {
    close();
    name_ = name;
    mutex_ = CreateMutexA(nullptr, FALSE, (name + ".lock").c_str());
    if (!mutex_) {
        error_ = "CreateMutex failed";
        return false;
    }
    // Wake-up hint for the consumer; best effort. A null handle only means the
    // consumer falls back to short polling.
    frame_event_ = CreateEventA(nullptr, FALSE, FALSE, (name + ".frame").c_str());
    mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  DWORD(kFrameBusSectionSize >> 32),
                                  DWORD(kFrameBusSectionSize & 0xFFFFFFFF), name.c_str());
    if (!mapping_) {
        error_ = "CreateFileMapping failed";
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // A mapping exists. It belongs either to a live WirePlay or to one
        // that died without closing it; the kernel keeps the section alive
        // while any handle is open, but a crash closes every handle, so a
        // stale section here means its owner is really gone.
        uint32_t owner = 0;
        HANDLE existing = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
        if (existing) {
            if (const void* view = MapViewOfFile(existing, FILE_MAP_READ, 0, 0, sizeof(BusHeader))) {
                const BusHeader* h = static_cast<const BusHeader*>(view);
                if (h->magic == kFrameBusMagic && h->version == kFrameBusVersion) {
                    owner = h->producer_pid;
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(existing);
        }
        if (processAlive(owner)) {
            error_ = "another WirePlay instance already owns the bus";
            return false;
        }
        // Dead owner. Close our handle and retry; the section goes away when
        // the last handle to it closes, so this second attempt usually wins.
        CloseHandle(mapping_);
        mapping_ = nullptr;
        mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      DWORD(kFrameBusSectionSize >> 32),
                                      DWORD(kFrameBusSectionSize & 0xFFFFFFFF), name.c_str());
        if (!mapping_) {
            error_ = "CreateFileMapping failed while reclaiming a stale bus";
            return false;
        }
        SetLastError(ERROR_SUCCESS);
    }
    base_ = static_cast<uint8_t*>(
        MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kFrameBusSectionSize));
    if (!base_) {
        error_ = "MapViewOfFile failed";
        return false;
    }
    owns_ = true;
    std::memset(base_, 0, kSlotOffsetAligned);
    header_ = reinterpret_cast<BusHeader*>(base_);
    header_->magic = kFrameBusMagic;
    header_->version = kFrameBusVersion;
    header_->slot_offset = kSlotOffsetAligned;
    header_->producer_pid = currentPid();
    return true;
}

bool FrameBus::open(const std::string& name) {
    close();
    mutex_ = CreateMutexA(nullptr, FALSE, (name + ".lock").c_str());
    frame_event_ = CreateEventA(nullptr, FALSE, FALSE, (name + ".frame").c_str());
    mapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!mapping_) {
        error_ = "the bus does not exist yet — is WirePlay running?";
        return false;
    }
    base_ = static_cast<uint8_t*>(
        MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kFrameBusSectionSize));
    if (!base_) {
        error_ = "MapViewOfFile failed";
        return false;
    }
    header_ = reinterpret_cast<BusHeader*>(base_);
    if (header_->magic != kFrameBusMagic || header_->version != kFrameBusVersion) {
        error_ = "bus layout mismatch — the app and the camera filter are different builds";
        close();
        return false;
    }
    return true;
}

void FrameBus::close() {
    if (base_) {
        UnmapViewOfFile(base_);
        base_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (mutex_) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    if (frame_event_) {
        CloseHandle(frame_event_);
        frame_event_ = nullptr;
    }
    header_ = nullptr;
    owns_ = false;
}

#else  // POSIX

bool FrameBus::create(const std::string& name) {
    close();
    const std::string shm_name = "/" + name;
    name_ = name;
    // O_EXCL is the "one producer" guarantee: a second WirePlay instance fails
    // here instead of quietly publishing over the first one's frames.
    fd_ = ::shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd_ < 0 && errno == EEXIST) {
        // Either a live instance, or a section left behind by one that was
        // killed. Find out which before giving up, or a single crash would
        // make the virtual camera unusable until the machine is rebooted.
        FrameBus probe;
        if (probe.open(name)) {
            const uint32_t owner = probe.producerPid();
            probe.close();
            if (processAlive(owner)) {
                error_ = "another WirePlay instance already owns the bus";
                return false;
            }
        }
        ::shm_unlink(shm_name.c_str());
        fd_ = ::shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd_ < 0) {
        error_ = std::string("shm_open failed: ") + std::strerror(errno);
        return false;
    }
    if (ftruncate(fd_, off_t(kFrameBusSectionSize)) != 0) {
        error_ = std::string("ftruncate failed: ") + std::strerror(errno);
        close();
        return false;
    }
    base_ = static_cast<uint8_t*>(
        mmap(nullptr, kFrameBusSectionSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (base_ == MAP_FAILED) {
        base_ = nullptr;
        error_ = std::string("mmap failed: ") + std::strerror(errno);
        close();
        return false;
    }
    std::memset(base_, 0, kSlotOffsetAligned);

    pthread_mutexattr_init(&attr_);
    attr_init_ = true;
    pthread_mutexattr_setpshared(&attr_, PTHREAD_PROCESS_SHARED);
    shm_mutex_ = reinterpret_cast<pthread_mutex_t*>(base_ + kSyncOffset);
    if (pthread_mutex_init(shm_mutex_, &attr_) != 0) {
        error_ = "pthread_mutex_init failed";
        close();
        return false;
    }

    header_ = reinterpret_cast<BusHeader*>(base_);
    header_->magic = kFrameBusMagic;
    header_->version = kFrameBusVersion;
    header_->slot_offset = kSlotOffsetAligned;
    header_->producer_pid = currentPid();
    owns_ = true;
    // Deliberately NOT shm_unlink'd: the consumer opens the bus by name, so
    // the name has to outlive create(). The cost is that a crashed producer
    // leaves the segment behind and the next create() fails on O_EXCL -- which
    // is the right failure, since it tells the user to close the old instance.
    return true;
}

bool FrameBus::open(const std::string& name) {
    close();
    const std::string shm_name = "/" + name;
    fd_ = ::shm_open(shm_name.c_str(), O_RDWR, 0600);
    if (fd_ < 0) {
        error_ = "the bus does not exist yet — is WirePlay running?";
        return false;
    }
    base_ = static_cast<uint8_t*>(
        mmap(nullptr, kFrameBusSectionSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (base_ == MAP_FAILED) {
        base_ = nullptr;
        error_ = std::string("mmap failed: ") + std::strerror(errno);
        close();
        return false;
    }
    header_ = reinterpret_cast<BusHeader*>(base_);
    if (header_->magic != kFrameBusMagic || header_->version != kFrameBusVersion) {
        error_ = "bus layout mismatch — the app and the camera filter are different builds";
        close();
        return false;
    }
    shm_mutex_ = reinterpret_cast<pthread_mutex_t*>(base_ + kSyncOffset);
    return true;
}

void FrameBus::close() {
    if (shm_mutex_ && owns_) {
        pthread_mutex_destroy(shm_mutex_);
        shm_mutex_ = nullptr;
    }
    shm_mutex_ = nullptr;
    if (base_) {
        munmap(base_, kFrameBusSectionSize);
        base_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (attr_init_) {
        pthread_mutexattr_destroy(&attr_);
        attr_init_ = false;
    }
    header_ = nullptr;
    owns_ = false;
}

#endif

uint32_t FrameBus::producerPid() const { return header_ ? header_->producer_pid : 0; }

void FrameBus::abandon() {
#ifndef _WIN32
    if (owns_ && !name_.empty()) {
        ::shm_unlink(("/" + name_).c_str());
    }
#endif
    // On Windows the section is named by the kernel object and disappears when
    // the last handle to it closes, which close() already does.
    close();
}

bool FrameBus::publish(const uint8_t* bgra, int width, int height, int stride, uint32_t fps_num,
                       uint32_t fps_den) {
    if (!header_ || !bgra || width <= 0 || height <= 0 || stride < width * 4) return false;
    if (uint32_t(stride) > kBusMaxStride || uint32_t(height) > kBusMaxHeight) {
        error_ = "frame larger than the bus supports";
        return false;
    }
    const size_t row_bytes = size_t(width) * 4;
    lock();
    header_->width = uint32_t(width);
    header_->height = uint32_t(height);
    header_->format = kBusFormatBGRA32;
    header_->fps_num = fps_num;
    header_->fps_den = fps_den ? fps_den : 1;
    header_->slot_stride = uint32_t(row_bytes);
    // row_bytes is width*4, and the checks above bound it by kBusMaxStride, so
    // this product cannot exceed kBusSlotBytes. MSVC's C4267 is asking whether
    // the narrowing to the uint32_t header field is safe; prove it rather than
    // assume it, because the header layout is shared across processes.
    static_assert(size_t(kBusMaxStride) * size_t(kBusMaxHeight) == size_t(kBusSlotBytes),
                  "kBusSlotBytes must be exactly the worst case for slot_size");
    static_assert(size_t(kBusSlotBytes) <= 0xFFFFFFFFull,
                  "slot_size is uint32_t in BusHeader, which is a frozen layout");
    header_->slot_size = uint32_t(row_bytes * size_t(height));
    uint8_t* dst = slot();
    const uint8_t* src = bgra;
    for (int y = 0; y < height; ++y) {
        // size_t(stride): stride is a signed int and this is pointer arithmetic.
        std::memcpy(dst + size_t(y) * row_bytes, src + size_t(y) * size_t(stride),
                    row_bytes);
    }
    header_->published_count++;
    header_->last_publish_ns = uint64_t(monotonicNowMs()) * 1000000ull;
    unlock();
#ifdef _WIN32
    // After the unlock, so the woken consumer does not immediately block on
    // the lock this thread still holds.
    if (frame_event_) SetEvent(frame_event_);
#endif
    return true;
}

bool FrameBus::consumeLatest(uint8_t* dst, int* out_width, int* out_height, int* out_stride,
                             uint64_t* out_seq) {
    if (!header_ || !dst) return false;
    lock();
    const uint64_t before = header_->published_count;
    if (before == 0) {
        unlock();
        return false;
    }
    const uint32_t w = header_->width, h = header_->height;
    const uint32_t stride = header_->slot_stride;
    if (w == 0 || h == 0 || stride < w * 4 || uint32_t(stride) > kBusMaxStride ||
        uint32_t(h) > kBusMaxHeight) {
        unlock();
        return false;
    }
    const size_t row_bytes = size_t(w) * 4;
    const uint8_t* src = slot();
    for (uint32_t y = 0; y < h; ++y) {
        // size_t(stride): stride is a signed int and this is pointer arithmetic.
        std::memcpy(dst + size_t(y) * row_bytes, src + size_t(y) * size_t(stride),
                    row_bytes);
    }
    header_->consumed_count++;
    // If the producer overwrote the slot while we were copying, the frame is
    // torn. Report it and let the caller skip; the next one is a frame away.
    const bool torn = header_->published_count != before;
    unlock();
    if (torn) return false;
    *out_width = int(w);
    *out_height = int(h);
    *out_stride = int(row_bytes);
    if (out_seq) *out_seq = before;
    return true;
}

uint64_t FrameBus::waitForFrame(uint64_t seen, uint32_t timeout_ms) {
    if (!header_) return seen;
    const uint64_t deadline = monotonicNowMs() + timeout_ms;
    for (;;) {
        // An aligned 8-byte read; the producer writes it under the lock, and a
        // stale value here only means one more trip round this loop.
        const uint64_t count = header_->published_count;
        if (count != seen) return count;
        const uint64_t now = monotonicNowMs();
        if (now >= deadline) return seen;
#ifdef _WIN32
        const DWORD left = DWORD(deadline - now);
        if (frame_event_) {
            WaitForSingleObject(frame_event_, left);
        } else {
            Sleep(left > 1 ? 1 : left);
        }
#else
        // POSIX is the test platform only; there is no event, so poll finely.
        struct timespec ts {0, 200 * 1000};
        nanosleep(&ts, nullptr);
#endif
    }
}

bool FrameBus::stale(uint64_t timeout_ms) const {
    if (!header_ || header_->published_count == 0) return true;
    const uint64_t last_ms = header_->last_publish_ns / 1000000ull;
    return monotonicNowMs() - last_ms > timeout_ms;
}

}  // namespace wp
