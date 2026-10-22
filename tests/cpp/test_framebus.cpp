#include "wp/framebus.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// The bus is the seam between the MIT app and the DirectShow camera filter, so
// it is the one part of the virtual-camera path that can be proven without a
// Windows box: the layout is a POD struct with no pointers, so both sides of
// the ifdef compile to the same bytes.

namespace {

// Each test names a fixed section, and a producer must not leave 60 MB behind
// in /dev/shm: on a CI runner that accumulates one per run until the tmpfs
// fills. abandon() is called explicitly rather than through a RAII wrapper
// because a wrapper silently changes every call site.
struct Failure {
    const char* what;
    int line;
};

[[noreturn]] void fail(const char* what, int line) {
    std::fprintf(stderr, "  FAIL line %d: %s\n", line, what);
    std::exit(1);
}

#define REQUIRE(cond) \
    do {              \
        if (!(cond)) fail(#cond, __LINE__); \
    } while (0)

void testPublishThenConsume() {
    std::printf("[ RUN ] framebus: publish then consume\n");
    wp::FrameBus producer, consumer;
    const std::string name = "wpfb_test_single";
    REQUIRE(producer.create(name));
    REQUIRE(consumer.open(name));

    const int w = 8, h = 4, stride = w * 4 + 16;  // stride deliberately padded
    std::vector<uint8_t> src(size_t(stride) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &src[size_t(y) * stride + x * 4];
            p[0] = uint8_t(x);        // B
            p[1] = uint8_t(y);        // G
            p[2] = uint8_t(x + y);    // R
            p[3] = 0xFF;              // A
        }
    REQUIRE(producer.publish(src.data(), w, h, stride, 60, 1));

    // Before the first publish there is nothing to hand out.
    std::vector<uint8_t> dst(size_t(w) * 4 * h, 0);
    int ow = 0, oh = 0, os = 0;
    REQUIRE(consumer.consumeLatest(dst.data(), &ow, &oh, &os));
    REQUIRE(ow == w && oh == h && os == w * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &dst[(size_t(y) * w + x) * 4];
            REQUIRE(p[0] == uint8_t(x) && p[1] == uint8_t(y) && p[2] == uint8_t(x + y) &&
                    p[3] == 0xFF);
        }
    REQUIRE(producer.published() == 1);
    producer.abandon();
    std::printf("[  OK ] framebus: publish then consume\n");
}

void testSingleProducerOnly() {
    std::printf("[ RUN ] framebus: only one producer\n");
    wp::FrameBus a, b;
    const std::string name = "wpfb_test_owner";
    REQUIRE(a.create(name));
    REQUIRE(!b.create(name));  // a second mirror must not silently win
    REQUIRE(!b.error().empty());
    a.abandon();
    std::printf("[  OK ] framebus: only one producer (%s)\n", b.error().c_str());
}

void testOpenBeforeCreateFails() {
    std::printf("[ RUN ] framebus: consumer before producer\n");
    wp::FrameBus consumer;
    REQUIRE(!consumer.open("wpfb_test_absent"));
    REQUIRE(!consumer.error().empty());
    std::printf("[  OK ] framebus: consumer before producer (%s)\n", consumer.error().c_str());
}

void testLatestFrameWins() {
    std::printf("[ RUN ] framebus: consumer gets the newest frame\n");
    wp::FrameBus producer, consumer;
    REQUIRE(producer.create("wpfb_test_race"));
    REQUIRE(consumer.open("wpfb_test_race"));

    const int w = 64, h = 64;
    std::vector<uint8_t> src(size_t(w) * 4 * h);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> published{0};
    std::thread writer([&] {
        uint8_t seq = 0;
        while (!stop.load()) {
            std::memset(src.data(), seq, src.size());
            producer.publish(src.data(), w, h, w * 4, 60, 1);
            published.fetch_add(1);
            ++seq;
            std::this_thread::yield();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    stop = true;
    writer.join();

    std::vector<uint8_t> dst(size_t(w) * 4 * h);
    int ow = 0, oh = 0, os = 0;
    int good = 0;
    for (int i = 0; i < 64; ++i) {
        if (!consumer.consumeLatest(dst.data(), &ow, &oh, &os)) continue;
        ++good;
        // A uniform frame is the check: any tear shows up as mixed values.
        const uint8_t first = dst[0];
        for (size_t k = 1; k < dst.size(); ++k) REQUIRE(dst[k] == first);
        REQUIRE(ow == w && oh == h && os == w * 4);
    }
    REQUIRE(good > 0);
    REQUIRE(published.load() > 1000);  // the writer really did run flat out
    producer.abandon();
    std::printf("[  OK ] framebus: consumer gets the newest frame (%d clean reads of %llu "
                "publishes)\n",
                good, (unsigned long long)published.load());
}

void testOversizedFrameRejected() {
    std::printf("[ RUN ] framebus: oversized frame rejected\n");
    wp::FrameBus producer;
    REQUIRE(producer.create("wpfb_test_big"));
    std::vector<uint8_t> src(16);
    REQUIRE(!producer.publish(src.data(), 1, int(wp::kBusMaxHeight) + 1, 4, 60, 1));
    REQUIRE(!producer.publish(nullptr, 8, 8, 32, 60, 1));
    REQUIRE(producer.publish(src.data(), 2, 2, 8, 0, 0));  // fps_den 0 must not divide
    producer.abandon();
    std::printf("[  OK ] framebus: oversized frame rejected\n");
}

void testStaleness() {
    std::printf("[ RUN ] framebus: staleness\n");
    wp::FrameBus producer, consumer;
    REQUIRE(producer.create("wpfb_test_stale"));
    REQUIRE(consumer.open("wpfb_test_stale"));
    REQUIRE(consumer.stale(1000));  // nothing published yet
    std::vector<uint8_t> src(16, 0x7F);
    REQUIRE(producer.publish(src.data(), 2, 2, 8, 60, 1));
    REQUIRE(!consumer.stale(1000));
    REQUIRE(!consumer.stale(0));  // just published: even a zero budget is not stale
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(consumer.stale(1));
    REQUIRE(!consumer.stale(1000));
    producer.abandon();
    std::printf("[  OK ] framebus: staleness\n");
}

// ---------------------------------------------------------------------------
// Two of these tests need a second, genuinely separate process. That is the
// point: the camera filter lives in OBS's process, not ours, so a mutex that is
// not PROCESS_SHARED or a section opened by the wrong name only shows up across
// a real boundary. POSIX forks; Windows re-executes this same binary with a
// marker argument, which additionally exercises the named-section path rather
// than an inherited handle -- and Windows is the platform that matters here.

constexpr int kCrossW = 32, kCrossH = 32;

int childConsume(const std::string& name) {
    wp::FrameBus consumer;
    if (!consumer.open(name)) return 10;
    std::vector<uint8_t> dst(size_t(kCrossW) * 4 * kCrossH, 0);
    int w = 0, h = 0, stride = 0;
    if (!consumer.consumeLatest(dst.data(), &w, &h, &stride)) return 11;
    if (w != kCrossW || h != kCrossH || stride != kCrossW * 4) return 12;
    for (size_t i = 0; i < dst.size(); ++i) {
        if (dst[i] != uint8_t(i)) return 13;
    }
    // Write back through the same mapping: the parent must see it, which is
    // what proves the two processes share one section and not two copies.
    std::vector<uint8_t> back(16, 0x5A);
    if (!consumer.publish(back.data(), 4, 4, 16, 60, 1)) return 14;
    return 0;
}

int childProduceAndDie(const std::string& name) {
    wp::FrameBus producer;
    if (!producer.create(name)) return 20;
    std::vector<uint8_t> src(16, 0x33);
    if (!producer.publish(src.data(), 2, 2, 8, 60, 1)) return 21;
    return 9;  // caller exits hard: no cleanup, as a crash would
}

// Runs this executable again as `mode name`. Returns false if the process could
// not be started or did not finish.
bool runSelf(const char* mode, const std::string& name, uint32_t* exit_code, uint32_t* child_pid) {
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) return false;
    const std::string cmdline = std::string("\"") + exe + "\" " + mode + " " + name;
    std::vector<char> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back('\0');
    STARTUPINFOA si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};
    if (!CreateProcessA(exe, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
        return false;
    }
    *child_pid = uint32_t(pi.dwProcessId);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    *exit_code = uint32_t(code);
    return ok != FALSE;
#else
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        const int rc =
            std::strcmp(mode, "consume") == 0 ? childConsume(name) : childProduceAndDie(name);
        if (rc == 9) ::raise(SIGKILL);  // leave the segment behind, as a crash does
        std::_Exit(rc);
    }
    *child_pid = uint32_t(pid);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    if (std::strcmp(mode, "crash") == 0) {
        if (!WIFSIGNALED(status)) return false;
        *exit_code = 9;
        return true;
    }
    if (!WIFEXITED(status)) return false;
    *exit_code = uint32_t(WEXITSTATUS(status));
    return true;
#endif
}

void testWaitForFrame() {
    std::printf("[ RUN ] framebus: waitForFrame wakes on publish, times out otherwise\n");
    wp::FrameBus producer, consumer;
    const std::string name = "wpfb_test_wait";
    REQUIRE(producer.create(name));
    REQUIRE(consumer.open(name));

    const int w = 8, h = 4;
    std::vector<uint8_t> src(size_t(w) * h * 4, 0x11);

    // Nothing published: must time out, returning the value it was given, and
    // must not return much earlier than asked.
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(consumer.waitForFrame(0, 40) == 0);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
    REQUIRE(waited >= 35);

    // A publish from another thread wakes it well before the timeout.
    std::thread pub([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        producer.publish(src.data(), w, h, w * 4, 60, 1);
    });
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t got = consumer.waitForFrame(0, 2000);
    const auto woke = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t1).count();
    pub.join();
    REQUIRE(got == 1);
    REQUIRE(woke < 500);

    // consumeLatest reports the sequence it copied, and waiting on that
    // sequence blocks until the NEXT publish.
    std::vector<uint8_t> dst(size_t(w) * h * 4);
    int ow = 0, oh = 0, os = 0;
    uint64_t seq = 0;
    REQUIRE(consumer.consumeLatest(dst.data(), &ow, &oh, &os, &seq));
    REQUIRE(seq == 1);
    REQUIRE(consumer.waitForFrame(seq, 30) == seq);  // no new frame: timeout
    REQUIRE(producer.publish(src.data(), w, h, w * 4, 60, 1));
    REQUIRE(consumer.waitForFrame(seq, 30) == 2);

    producer.abandon();
    std::printf("[  OK ] framebus: waitForFrame (woke after %lld ms)\n", (long long)woke);
}

void testAcrossRealProcesses() {
    std::printf("[ RUN ] framebus: across real processes\n");
    const std::string name = "wpfb_test_xproc";
    wp::FrameBus producer;
    REQUIRE(producer.create(name));

    std::vector<uint8_t> src(size_t(kCrossW) * 4 * kCrossH);
    for (size_t i = 0; i < src.size(); ++i) src[i] = uint8_t(i);
    REQUIRE(producer.publish(src.data(), kCrossW, kCrossH, kCrossW * 4, 60, 1));

    uint32_t code = 123, child = 0;
    REQUIRE(runSelf("consume", name, &code, &child));
    REQUIRE(code == 0);
    REQUIRE(producer.published() == 2);  // the child's publish landed here
    REQUIRE(producer.width() == 4);
    producer.abandon();
    std::printf("[  OK ] framebus: across real processes (child pid %u)\n", child);
}

// The user-visible requirement: closing or crashing WirePlay must not leave the
// virtual camera permanently dead. On POSIX the section survives its owner, so
// this also proves the stale-owner reclaim path; on Windows the section goes
// away with the last handle, so it proves the successor can simply create it.
void testSuccessorAfterProducerDies() {
    std::printf("[ RUN ] framebus: successor after the producer dies\n");
    const std::string name = "wpfb_test_died";
    uint32_t code = 0, child = 0;
    REQUIRE(runSelf("crash", name, &code, &child));
    REQUIRE(code == 9);

#ifndef _WIN32
    // The segment is still there and names a process that no longer exists.
    wp::FrameBus probe;
    REQUIRE(probe.open(name));
    REQUIRE(probe.producerPid() == child);
    probe.close();
#endif

    wp::FrameBus successor;
    REQUIRE(successor.create(name));
    REQUIRE(successor.producerPid() == uint32_t(wp::currentPid()));
    std::vector<uint8_t> src(16, 0x44);
    REQUIRE(successor.publish(src.data(), 2, 2, 8, 60, 1));
    REQUIRE(successor.published() == 1);  // a fresh bus, not the dead one's counter
    successor.abandon();
    std::printf("[  OK ] framebus: successor after the producer dies (dead pid %u)\n", child);
}

// Two real bugs, both of which produced a camera that silently showed nothing.
void testBusNameIsUsableOnThisPlatform() {
    std::printf("[ RUN ] framebus: bus name is valid for this platform\n");
    const std::string name = wp::defaultBusName();
    REQUIRE(!name.empty());
#ifdef _WIN32
    REQUIRE(name.rfind("Local\\", 0) == 0);  // per-session namespace
#else
    // A backslash here means Win32 syntax leaked into a POSIX shm name, and
    // then the producer and the consumer disagree about which file to open.
    REQUIRE(name.find('\\') == std::string::npos);
    REQUIRE(name.find('/') == std::string::npos);
#endif
    wp::FrameBus producer, consumer;
    REQUIRE(producer.create(name));
    REQUIRE(consumer.open(name));
    producer.abandon();
    std::printf("[  OK ] framebus: bus name is valid for this platform (%s)\n", name.c_str());
}

// A phone held the long way round is taller than 2160, so a bus capped at
// "4K height" rejects every frame from a Pro Max in portrait.
void testFullPhoneResolutionFits() {
    std::printf("[ RUN ] framebus: full phone resolution fits\n");
    REQUIRE(wp::kBusMaxHeight >= 2796);       // iPhone 14/15 Pro Max, portrait
    REQUIRE(wp::kBusMaxStride >= 2064u * 4);  // 13" iPad, landscape
    wp::FrameBus producer, consumer;
    REQUIRE(producer.create("wpfb_test_pro_max"));
    REQUIRE(consumer.open("wpfb_test_pro_max"));
    const int w = 1290, h = 2796;
    std::vector<uint8_t> src(size_t(w) * 4 * h, 0x77);
    REQUIRE(producer.publish(src.data(), w, h, w * 4, 60, 1));
    std::vector<uint8_t> dst(size_t(w) * 4 * h, 0);
    int ow = 0, oh = 0, os = 0;
    REQUIRE(consumer.consumeLatest(dst.data(), &ow, &oh, &os));
    REQUIRE(ow == w && oh == h);
    REQUIRE(dst.front() == 0x77 && dst.back() == 0x77);
    producer.abandon();
    std::printf("[  OK ] framebus: full phone resolution fits (%dx%d, section %zu bytes)\n", ow,
                oh, wp::kFrameBusSectionSize);
}

// Proves the layout really is pointer-free and that both sides see the same
// offsets -- the property the Windows camera filter depends on.
void testLayoutIsPortable() {
    std::printf("[ RUN ] framebus: layout is portable\n");
    REQUIRE(sizeof(wp::BusHeader) <= 4096);
    REQUIRE(wp::kFrameBusSectionSize > wp::kBusSlotBytes);
    wp::FrameBus producer, consumer;
    REQUIRE(producer.create("wpfb_test_layout"));
    REQUIRE(consumer.open("wpfb_test_layout"));
    std::vector<uint8_t> src(8 * 4 * 8, 0x11);
    REQUIRE(producer.publish(src.data(), 8, 8, 8 * 4, 30, 1));
    REQUIRE(consumer.width() == 8 && consumer.height() == 8);
    REQUIRE(consumer.published() == 1);
    producer.abandon();
    std::printf("[  OK ] framebus: layout is portable (header %zu bytes, section %zu bytes)\n",
                sizeof(wp::BusHeader), wp::kFrameBusSectionSize);
}

}  // namespace

// Wrappers so the Windows re-exec path in main() can reach the child bodies,
// which live in the anonymous namespace above.
int wp_childConsume(const std::string& name) { return childConsume(name); }
int wp_childProduceAndDie(const std::string& name) { return childProduceAndDie(name); }

int main(int argc, char** argv) {
    // Windows cannot fork, so the cross-process tests re-exec this binary and
    // the child lands here instead of in the test list.
    if (argc >= 3) {
        const std::string mode = argv[1], name = argv[2];
        if (mode == "consume") return wp_childConsume(name);
        if (mode == "crash") return wp_childProduceAndDie(name);
    }

    testPublishThenConsume();
    testSingleProducerOnly();
    testOpenBeforeCreateFails();
    testLatestFrameWins();
    testOversizedFrameRejected();
    testStaleness();
    testWaitForFrame();
    testAcrossRealProcesses();
    testSuccessorAfterProducerDies();
    testBusNameIsUsableOnThisPlatform();
    testFullPhoneResolutionFits();
    testLayoutIsPortable();
    std::printf("\nframebus: all checks passed\n");
    return 0;
}
