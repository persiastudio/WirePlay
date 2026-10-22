#include "wp/backend.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

// The selector decides whether a given phone can be mirrored at all, so every
// branch is worth pinning.
//
// The driver model here has three Windows states, not two, and that matters:
// libusb0.sys installed as an UpperFilter leaves Apple's usbccgp driver in
// place, so iTunes and 3uTools keep working and BOTH backends are reachable.
// Replacing Apple's driver (zadig/WinUSB) is the rare, destructive case. An
// earlier version of this file only modelled "Apple" or "replaced", which made
// the selector refuse CoreDevice on a machine where it would have worked and
// tell the user to undo a driver swap they had not made.

namespace {

[[noreturn]] void fail(const char* what, int line) {
    std::fprintf(stderr, "  FAIL line %d: %s\n", line, what);
    std::exit(1);
}

#define REQUIRE(cond)                       \
    do {                                    \
        if (!(cond)) fail(#cond, __LINE__); \
    } while (0)

using wp::Backend;
using wp::BackendPreference;
using wp::Environment;
using wp::UsbDriver;

Environment env(bool windows, UsbDriver driver, bool qt, const std::string& version,
                const std::string& model = "", bool implemented = true) {
    Environment e;
    e.windows = windows;
    e.driver = driver;
    e.quicktime_available = qt;
    e.quicktime_implemented = implemented;
    e.device.product_version = version;
    e.device.product_type = model;
    return e;
}

// Apple's driver alone: CoreDevice works, the QuickTime protocol cannot get at
// the interfaces it needs.
Environment winApple(const std::string& version, const std::string& model = "") {
    return env(true, UsbDriver::kApple, false, version, model);
}

// The normal mirroring setup: Apple's usbccgp is still the function driver and
// libusb0.sys is an UpperFilter. Both backends work; iTunes keeps working.
Environment winFilter(const std::string& version, bool qt = true) {
    return env(true, UsbDriver::kApplePlusLibusbFilter, qt, version);
}

// The destructive setup: Apple's driver replaced outright.
Environment winReplaced(const std::string& version, bool qt = true) {
    return env(true, UsbDriver::kLibusbReplaced, qt, version);
}

void testVersionParsing() {
    std::printf("[ RUN ] backend: iOS version parsing\n");
    REQUIRE(wp::iosMajorVersion("15.8.3") == 15);
    REQUIRE(wp::iosMajorVersion("17.0") == 17);
    REQUIRE(wp::iosMajorVersion("18") == 18);
    REQUIRE(wp::iosMajorVersion("26.1.2") == 26);
    REQUIRE(wp::iosMajorVersion("27.2") == 27);
    REQUIRE(wp::iosMajorVersion("9.3.6") == 9);
    // Unknown must come back as 0, never as "very old". A blank answer from
    // lockdown has to mean "we do not know", not "fall back to the legacy path".
    REQUIRE(wp::iosMajorVersion("") == 0);
    REQUIRE(wp::iosMajorVersion("unknown") == 0);
    // A leading number followed by junk with no separator is not a version.
    REQUIRE(wp::iosMajorVersion("15x") == 0);
    REQUIRE(wp::iosMajorVersion("junk15") == 0);
    REQUIRE(wp::iosMajorVersion(".15") == 0);
    REQUIRE(wp::iosMajorVersion(" 15") == 0);
    // "15." is a major version followed by a separator, which is a legitimate
    // prefix of "15.8.3"; accepting it is deliberate.
    REQUIRE(wp::iosMajorVersion("15.") == 15);
    REQUIRE(wp::iosMajorVersion("1.0") == 1);
    REQUIRE(wp::iosMajorVersion("99999999999") == 0);
    std::printf("[  OK ] backend: iOS version parsing\n");
}

void testDriverStateHelpers() {
    std::printf("[ RUN ] backend: driver-state predicates\n");
    // CoreDevice travels Apple's stack, so only the states that removed it block.
    REQUIRE(wp::coredeviceReachable(UsbDriver::kApple));
    REQUIRE(wp::coredeviceReachable(UsbDriver::kApplePlusLibusbFilter));
    REQUIRE(wp::coredeviceReachable(UsbDriver::kUnknown));
    REQUIRE(!wp::coredeviceReachable(UsbDriver::kLibusbReplaced));
    REQUIRE(!wp::coredeviceReachable(UsbDriver::kNonePresent));

    // "Intact" means iTunes and the Apple Devices app still work.
    REQUIRE(wp::appleStackIntact(UsbDriver::kApple));
    REQUIRE(wp::appleStackIntact(UsbDriver::kApplePlusLibusbFilter));
    REQUIRE(!wp::appleStackIntact(UsbDriver::kLibusbReplaced));
    REQUIRE(!wp::appleStackIntact(UsbDriver::kNonePresent));
    std::printf("[  OK ] backend: driver-state predicates\n");
}

// The user's actual hardware.
void testIphone11ProMaxOnIos27() {
    std::printf("[ RUN ] backend: iPhone 11 Pro Max on iOS 27.2\n");

    // Apple's driver alone, no QuickTime transport. CoreDevice could run here,
    // and auto still refuses it: reaching it needs Developer Mode, and this app
    // does not ask the user for that.
    const auto apple = wp::selectBackend(winApple("27.2", "iPhone12,5"), BackendPreference::kAuto);
    REQUIRE(apple.backend == Backend::kNone);
    REQUIRE(!apple.overruled);
    REQUIRE(apple.reason.find("27.2") != std::string::npos);
    REQUIRE(apple.reason.find("Developer Mode") != std::string::npos);
    REQUIRE(apple.reason.find("asks for neither") != std::string::npos);

    // With the filter installed, QuickTime is preferred: no Developer Mode, no
    // DDI remount after every reboot.
    const auto filter = wp::selectBackend(winFilter("27.2"), BackendPreference::kAuto);
    REQUIRE(filter.backend == Backend::kQuickTime);
    REQUIRE(!filter.overruled);
    REQUIRE(filter.reason.find("CoreDevice") != std::string::npos);
    REQUIRE(filter.reason.find("Developer Mode") != std::string::npos);

    // CoreDevice still runs there when explicitly forced, because Apple's driver
    // is intact -- it is the harness that validated the decode and camera
    // pipeline -- but the reason states the cost instead of selling it as free.
    const auto forced = wp::selectBackend(winFilter("27.2"), BackendPreference::kCoreDevice);
    REQUIRE(forced.backend == Backend::kCoreDevice);
    REQUIRE(!forced.overruled);
    REQUIRE(forced.reason.find("development harness") != std::string::npos);

    // Filter installed but the QuickTime transport not up. Auto no longer falls
    // back to CoreDevice: it reports the dead end and names the filter install.
    const auto fallback = wp::selectBackend(winFilter("27.2", false), BackendPreference::kAuto);
    REQUIRE(fallback.backend == Backend::kNone);
    REQUIRE(fallback.reason.find("UpperFilter") != std::string::npos);

    // And when the backend is simply not written yet, the reason says that
    // rather than sending the user off to install a driver for nothing.
    const auto not_written =
        wp::selectBackend(env(true, UsbDriver::kApplePlusLibusbFilter, true, "27.2", "", false),
                          BackendPreference::kAuto);
    REQUIRE(not_written.backend == Backend::kNone);
    REQUIRE(not_written.reason.find("does not implement") != std::string::npos);
    REQUIRE(not_written.reason.find("UpperFilter") == std::string::npos);
    std::printf("[  OK ] backend: iPhone 11 Pro Max on iOS 27.2\n");
}

void testLegacyDevice() {
    std::printf("[ RUN ] backend: iPhone 7 Plus on iOS 15\n");
    // Driver still Apple: the legacy path exists but is not reachable, and the
    // reason has to tell the user the concrete next step rather than just no.
    const auto blocked = wp::selectBackend(winApple("15.8.3", "iPhone9,2"), BackendPreference::kAuto);
    REQUIRE(blocked.backend == Backend::kNone);
    REQUIRE(blocked.reason.find("15.8.3") != std::string::npos);
    REQUIRE(blocked.reason.find("UpperFilter") != std::string::npos);
    // The advice must lead with the install that keeps Apple's driver working.
    REQUIRE(blocked.reason.find("keep working") != std::string::npos);

    // After the filter install it works, and says what it is using.
    const auto ok = wp::selectBackend(winFilter("15.8.3"), BackendPreference::kAuto);
    REQUIRE(ok.backend == Backend::kQuickTime);
    REQUIRE(!ok.overruled);
    REQUIRE(ok.reason.find("QuickTime") != std::string::npos);

    // CoreDevice must refuse on an old phone, not quietly substitute -- and the
    // reason is the iOS version, not the driver.
    const auto forced = wp::selectBackend(winFilter("15.8.3"), BackendPreference::kCoreDevice);
    REQUIRE(forced.backend == Backend::kNone);
    REQUIRE(forced.overruled);
    REQUIRE(forced.reason.find("15.8.3") != std::string::npos);

    // iOS 16 is legacy too.
    REQUIRE(wp::selectBackend(winApple("16.7.10"), BackendPreference::kAuto).backend ==
            Backend::kNone);
    REQUIRE(wp::selectBackend(winFilter("16.7.10"), BackendPreference::kAuto).backend ==
            Backend::kQuickTime);
    std::printf("[  OK ] backend: iPhone 7 Plus on iOS 15\n");
}

void testReplacedDriver() {
    std::printf("[ RUN ] backend: Apple's driver replaced outright\n");
    // QuickTime works, and the reason warns that iTunes no longer sees the phone.
    const auto qt = wp::selectBackend(winReplaced("27.2"), BackendPreference::kAuto);
    REQUIRE(qt.backend == Backend::kQuickTime);
    REQUIRE(qt.reason.find("CoreDevice") != std::string::npos);

    // Forced CoreDevice here is genuinely impossible, and the reason must not
    // tell the user to reach for zadig -- it must point at Apple's own installer.
    const auto forced = wp::selectBackend(winReplaced("27.2"), BackendPreference::kCoreDevice);
    REQUIRE(forced.backend == Backend::kNone);
    REQUIRE(forced.overruled);
    REQUIRE(forced.reason.find("Apple Devices") != std::string::npos);

    // Replaced driver and no working transport is a dead end, not a guess.
    const auto dead = wp::selectBackend(winReplaced("27.2", false), BackendPreference::kAuto);
    REQUIRE(dead.backend == Backend::kNone);
    REQUIRE(dead.reason.find("UpperFilter") != std::string::npos);
    std::printf("[  OK ] backend: Apple's driver replaced outright\n");
}

void testForcedPreferences() {
    std::printf("[ RUN ] backend: explicit --backend\n");
    // Forced quicktime on a modern phone works if the transport is there.
    const auto forced_qt = wp::selectBackend(winFilter("27.2"), BackendPreference::kQuickTime);
    REQUIRE(forced_qt.backend == Backend::kQuickTime);
    REQUIRE(forced_qt.reason.find("forced") != std::string::npos);
    // Apple's driver is intact, so the reason says iTunes keeps working.
    REQUIRE(forced_qt.reason.find("iTunes") != std::string::npos);

    // Same, with Apple's driver gone: the reason flips to a warning.
    const auto forced_qt_replaced =
        wp::selectBackend(winReplaced("27.2"), BackendPreference::kQuickTime);
    REQUIRE(forced_qt_replaced.backend == Backend::kQuickTime);
    REQUIRE(forced_qt_replaced.reason.find("will not see the phone") != std::string::npos);

    // Forced quicktime with no transport is overruled and explained.
    const auto blocked = wp::selectBackend(winApple("27.2"), BackendPreference::kQuickTime);
    REQUIRE(blocked.backend == Backend::kNone);
    REQUIRE(blocked.overruled);
    REQUIRE(blocked.reason.find("UpperFilter") != std::string::npos);

    // Forced coredevice on an old phone is overruled with the version named.
    const auto old = wp::selectBackend(winApple("12.5.7"), BackendPreference::kCoreDevice);
    REQUIRE(old.backend == Backend::kNone);
    REQUIRE(old.overruled);
    REQUIRE(old.reason.find("12.5.7") != std::string::npos);
    std::printf("[  OK ] backend: explicit --backend\n");
}

void testNoDeviceOrNoDriver() {
    std::printf("[ RUN ] backend: nothing attached\n");
    const auto c = wp::selectBackend(env(true, UsbDriver::kNonePresent, false, ""),
                                     BackendPreference::kAuto);
    REQUIRE(c.backend == Backend::kNone);
    REQUIRE(c.reason.find("driver") != std::string::npos);
    REQUIRE(c.reason.find("iTunes") != std::string::npos);

    // kNonePresent must block a forced CoreDevice too, and with its own reason.
    const auto forced = wp::selectBackend(env(true, UsbDriver::kNonePresent, false, "27.2"),
                                          BackendPreference::kCoreDevice);
    REQUIRE(forced.backend == Backend::kNone);
    REQUIRE(forced.overruled);
    REQUIRE(forced.reason.find("Apple Devices") != std::string::npos);

    // Version unreadable, Apple driver present. Auto does not guess any more:
    // the QuickTime protocol is version-independent, so an unread version was
    // never the thing blocking a backend, and the old code used this case to
    // sneak in a CoreDevice selection that needs Developer Mode.
    const auto guess = wp::selectBackend(winApple(""), BackendPreference::kAuto);
    REQUIRE(guess.backend == Backend::kNone);
    REQUIRE(guess.reason.find("UpperFilter") != std::string::npos);
    std::printf("[  OK ] backend: nothing attached\n");
}

void testEveryBranchHasAReason() {
    std::printf("[ RUN ] backend: no path returns an empty reason\n");
    const char* versions[] = {"", "9.3.6", "15.8.3", "16.7.10", "17.0", "27.2", "junk"};
    const UsbDriver drivers[] = {UsbDriver::kUnknown, UsbDriver::kApple,
                                 UsbDriver::kApplePlusLibusbFilter, UsbDriver::kLibusbReplaced,
                                 UsbDriver::kNonePresent};
    const BackendPreference prefs[] = {BackendPreference::kAuto, BackendPreference::kCoreDevice,
                                       BackendPreference::kQuickTime};
    int combos = 0, selected = 0, advised_filter = 0;
    for (bool win : {false, true}) {
        for (const char* v : versions) {
            for (UsbDriver d : drivers) {
                for (bool qt : {false, true}) {
                  for (bool impl : {false, true}) {
                    for (BackendPreference p : prefs) {
                        Environment e = env(win, d, qt, v, "", impl);
                        const auto c = wp::selectBackend(e, p);
                        ++combos;
                        REQUIRE(!c.reason.empty());  // the user always gets told something
                        if (c.backend != Backend::kNone) ++selected;
                        // An overrule must never also select a backend.
                        if (c.overruled) REQUIRE(c.backend == Backend::kNone);
                        // A selection must never claim to be overruled.
                        if (c.backend != Backend::kNone) REQUIRE(!c.overruled);

                        // The advice that got retracted: telling someone to undo
                        // a driver swap with zadig, when the filter install
                        // never needed one.
                        REQUIRE(c.reason.find("restore it with zadig") == std::string::npos);

                        // The standing rule: auto never picks the backend that
                        // needs Developer Mode, and never tells the user to turn
                        // it on. Forced CoreDevice is the only way to reach it.
                        if (p == BackendPreference::kAuto) {
                            REQUIRE(c.backend != Backend::kCoreDevice);
                            REQUIRE(c.reason.find("enable Developer Mode") == std::string::npos);
                            REQUIRE(c.reason.find("amfi") == std::string::npos);
                        }

                        // Whenever we explain that the QuickTime protocol is out
                        // of reach on Windows, the filter install must be named.
                        const bool unreachable =
                            c.backend == Backend::kNone && win && qt == false && impl &&
                            d != UsbDriver::kNonePresent && p != BackendPreference::kCoreDevice &&
                            wp::iosMajorVersion(v) > 0;
                        if (unreachable) {
                            REQUIRE(c.reason.find("UpperFilter") != std::string::npos);
                            ++advised_filter;
                        }

                        // If Apple's stack is intact and the phone is new enough,
                        // a forced CoreDevice is never overruled.
                        if (p == BackendPreference::kCoreDevice && wp::appleStackIntact(d) &&
                            wp::iosMajorVersion(v) >= wp::kCoreDeviceMinIos) {
                            REQUIRE(!c.overruled);
                            REQUIRE(c.backend == Backend::kCoreDevice);
                        }
                    }
                  }
                }
            }
        }
    }
    REQUIRE(combos == 2 * 7 * 5 * 2 * 2 * 3);
    REQUIRE(advised_filter > 0);  // the check above is not vacuous
    std::printf("[  OK ] backend: no path returns an empty reason (%d combinations, %d selected)\n",
                combos, selected);
}

void testNameRoundTrip() {
    std::printf("[ RUN ] backend: --backend name parsing\n");
    wp::BackendPreference p = wp::BackendPreference::kAuto;
    REQUIRE(wp::parseBackendName("auto", &p) && p == wp::BackendPreference::kAuto);
    REQUIRE(wp::parseBackendName("coredevice", &p) && p == wp::BackendPreference::kCoreDevice);
    REQUIRE(wp::parseBackendName("core", &p) && p == wp::BackendPreference::kCoreDevice);
    REQUIRE(wp::parseBackendName("quicktime", &p) && p == wp::BackendPreference::kQuickTime);
    REQUIRE(wp::parseBackendName("valeria", &p) && p == wp::BackendPreference::kQuickTime);
    REQUIRE(wp::parseBackendName("legacy", &p) && p == wp::BackendPreference::kQuickTime);
    REQUIRE(!wp::parseBackendName("airplay", &p));
    REQUIRE(!wp::parseBackendName("", &p));
    REQUIRE(std::string(wp::backendName(Backend::kCoreDevice)) == "coredevice");
    REQUIRE(std::string(wp::backendName(Backend::kQuickTime)) == "quicktime");
    REQUIRE(std::string(wp::backendName(Backend::kNone)) == "none");
    std::printf("[  OK ] backend: --backend name parsing\n");
}

}  // namespace

int main() {
    testVersionParsing();
    testDriverStateHelpers();
    testIphone11ProMaxOnIos27();
    testLegacyDevice();
    testReplacedDriver();
    testForcedPreferences();
    testNoDeviceOrNoDriver();
    testEveryBranchHasAReason();
    testNameRoundTrip();
    std::printf("\nbackend selection: all checks passed\n");
    return 0;
}
