// Stands in for the DirectShow filter: opens the bus, pulls frames at roughly
// 60 Hz, and reports what it saw. This is the part of the virtual camera that
// can be exercised without Windows, so it doubles as the integration test.
#include "wp/framebus.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 3;
    wp::FrameBus bus;
    for (int i = 0; i < 100 && !bus.open(wp::defaultBusName()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!bus.valid()) {
        std::fprintf(stderr, "probe: cannot open the bus: %s\n", bus.error().c_str());
        return 1;
    }
    std::vector<uint8_t> buf(size_t(wp::kBusMaxStride) * wp::kBusMaxHeight);
    int w = 0, h = 0, stride = 0;
    uint64_t got = 0, waiting = 0, last_seq = 0, unique = 0, last_sum = 0, blank = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        if (bus.consumeLatest(buf.data(), &w, &h, &stride)) {
            ++got;
            if (bus.published() == last_seq) ++waiting;  // same frame twice
            last_seq = bus.published();
            // A frame counter proves something arrived; a checksum proves it is
            // a picture. The dump is a moving gradient, so consecutive frames
            // must differ and none may be uniformly filled.
            uint64_t sum = 1469598103934665603ull;
            const size_t n = size_t(w) * 4 * size_t(h);
            for (size_t i = 0; i < n; i += 97) {
                sum ^= buf[i];
                sum *= 1099511628211ull;
            }
            if (sum != last_sum) ++unique;
            last_sum = sum;
            bool uniform = true;
            for (size_t i = 4; i < n && i < 4096; i += 4) {
                if (buf[i] != buf[0]) { uniform = false; break; }
            }
            if (uniform) ++blank;
        } else {
            ++waiting;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60 Hz
    }
    std::printf("probe: %dx%d, %llu frames read in %d s, %llu distinct, %llu blank, "
                "%llu repeats/waiting, producer published %llu\n",
                w, h, (unsigned long long)got, seconds, (unsigned long long)unique,
                (unsigned long long)blank, (unsigned long long)waiting,
                (unsigned long long)bus.published());
    return got > 0 && w > 0 && h > 0 && unique > 1 && blank == 0 ? 0 : 2;
}
