#include "wp/protocol.hpp"

#include <cstring>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace wp {

bool readU32BE(const uint8_t*& p, size_t& remaining, uint32_t& out) {
    if (remaining < 4) return false;
    out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    p += 4;
    remaining -= 4;
    return true;
}

bool FrameReader::readFully(void* dst, size_t n) {
    auto* out = static_cast<uint8_t*>(dst);
    while (n > 0) {
        const size_t got = std::fread(out, 1, n, f_);
        if (got == 0) {
            // EOF (engine exited) or a read error; either way the stream is over.
            return false;
        }
        out += got;
        n -= got;
    }
    return true;
}

bool FrameReader::read(Frame& out) {
    uint8_t header[4];
    if (!readFully(header, 4)) return false;

    const size_t length = (size_t(header[0]) << 24) | (size_t(header[1]) << 16) |
                          (size_t(header[2]) << 8) | size_t(header[3]);
    if (length < 1 || length > kMaxFrame) {
        std::fprintf(stderr, "[wireplay] bad frame length %zu\n", length);
        return false;
    }
    if (buf_.size() < length) buf_.resize(length);
    if (!readFully(buf_.data(), length)) return false;

    out.type = buf_[0];
    out.payload = buf_.data() + 1;
    out.size = length - 1;
    ++frames_read;
    return true;
}

bool FrameReader::hasPending() const {
#ifdef _WIN32
    // A pipe-backed FILE* cannot be polled with select(); PeekNamedPipe on the
    // underlying handle reports bytes the OS has already buffered for us.
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f_)));
    if (h == INVALID_HANDLE_VALUE) return true;  // cannot poll: let read() decide
    DWORD avail = 0;
    // PeekNamedPipe fails on disk files (the --offline source) and on a
    // pipe whose writer has closed. Both mean "do not wait here": the
    // blocking read returns data or EOF immediately in either case.
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return true;
    return avail > 0;
#else
    struct pollfd pfd {};
    pfd.fd = fileno(f_);
    pfd.events = POLLIN;
    // POLLHUP matters: a dead engine leaves a readable-at-EOF pipe, and
    // waiting for POLLIN alone would spin here forever.
    return ::poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR));
#endif
}

bool writeFrame(FILE* f, uint8_t type, const void* payload, size_t size) {
    const uint32_t length = uint32_t(size) + 1;
    const uint8_t header[5] = {
        uint8_t(length >> 24), uint8_t(length >> 16), uint8_t(length >> 8), uint8_t(length), type,
    };
    if (std::fwrite(header, 1, 5, f) != 5) return false;
    if (size > 0 && payload && std::fwrite(payload, 1, size, f) != size) return false;
    std::fflush(f);
    return true;
}

// Forces a FILE* to byte-exact mode. On Windows the C runtime otherwise
// translates 0x0A into 0x0D 0x0A on write and strips 0x1A on read, which
// silently corrupts a binary frame stream. POSIX is already byte-exact.
void setBinaryMode(FILE* f) {
#ifdef _WIN32
    _setmode(_fileno(f), _O_BINARY);
#else
    (void)f;
#endif
}

}  // namespace wp
