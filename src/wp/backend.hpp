// Choosing between the two screen-mirroring backends.
//
// There are two, and they are unrelated:
//
//   coredevice  com.apple.coredevice.displayservice, HEVC over RTP. Needs
//               iOS 17+, Developer Mode enabled, and a DeveloperDiskImage
//               mounted -- and that mount does not survive a reboot.
//   quicktime   the QuickTime/Valeria USB protocol, H.264 plus PCM. Reaches far
//               older devices and needs none of that: no Developer Mode, no DDI,
//               no pairing. This is what QuickTime Player on macOS, 3uTools and
//               the other mirroring tools use.
//
// The driver situation on Windows has three real states, not two, and getting
// this wrong used to make the selector give actively bad advice:
//
//   * Apple's usbccgp driver alone. CoreDevice works; the QuickTime protocol
//     cannot get at the USB interfaces it needs.
//   * Apple's usbccgp driver WITH libusb0.sys installed as an UpperFilter on
//     that device instance. Both backends work at once, and Apple's stack keeps
//     working -- iTunes, the Apple Devices app and 3uTools all still see the
//     phone. This is the normal setup for USB mirroring and is what the
//     actively maintained Windows mirroring projects install.
//   * Apple's driver replaced outright, e.g. WinUSB via zadig. The QuickTime
//     protocol works; CoreDevice stops, because it goes through Apple's stack.
//     This is the rare, destructive case and nothing here should recommend it.
//
// The selector reports both the choice and the reason, because "it picked the
// wrong one" is otherwise indistinguishable from a bug.
//
// No Windows or device I/O here. Everything that talks to a phone or to the
// registry lives elsewhere; this file only decides, so it can be tested.
#pragma once

#include <cstddef>
#include <string>

namespace wp {

enum class Backend {
    kNone = 0,
    kCoreDevice,  // iOS 17+
    kQuickTime,   // the Valeria protocol, any iOS version
};

// Which USB driver stack currently owns the phone.
enum class UsbDriver {
    kUnknown = 0,  // not probed, or not Windows (Linux/macOS need no driver)

    // Apple's usbccgp composite parent, nothing else attached.
    kApple,

    // Apple's usbccgp is still the function driver and libusb0.sys is an
    // UpperFilter on that instance. Both backends are reachable and iTunes keeps
    // working. The normal mirroring setup.
    kApplePlusLibusbFilter,

    // Apple's driver was replaced outright (zadig / WinUSB). The QuickTime
    // protocol works; CoreDevice does not, because it travels Apple's stack.
    kLibusbReplaced,

    kNonePresent,  // no driver at all: the phone is not visible to anything
};

struct DeviceInfo {
    std::string product_version;  // lockdown ProductVersion, e.g. "15.8.3"; "" if unknown
    std::string product_type;     // e.g. "iPhone12,5" (iPhone 11 Pro Max); "" if unknown
};

struct Environment {
    bool windows = false;
    UsbDriver driver = UsbDriver::kUnknown;
    // Whether the QuickTime protocol has a working transport here: on Windows
    // that means libusb can reach the device (either driver state above), and
    // on Linux/macOS that libusb has the device permissions.
    //
    // This must only be set once the QuickTime backend actually works end to
    // end. Auto-selection prefers it, so a true here is a promise.
    bool quicktime_available = false;
    // Whether this build contains a working QuickTime backend at all. Kept
    // apart from quicktime_available because the two need different advice: an
    // unimplemented backend is a gap in this program, while an unreachable
    // transport is a driver install on the user's machine. Telling someone to
    // install a driver to fix a missing feature is worse than saying either.
    bool quicktime_implemented = false;
    DeviceInfo device;
};

// An explicit user override, from --backend. kAuto means "decide".
enum class BackendPreference {
    kAuto = 0,
    kCoreDevice,
    kQuickTime,
};

struct BackendChoice {
    Backend backend = Backend::kNone;

    // Shown to the user. Never empty: when nothing can be selected, this says
    // what is missing and what to do about it.
    std::string reason;

    // Set when the user asked for a backend that cannot run here. The selector
    // still returns kNone rather than silently substituting the other one,
    // because quietly switching would leave the user staring at a failure they
    // did not choose.
    bool overruled = false;
};

// Parses "15.8.3" into its major component. Returns 0 for anything unparseable,
// which callers must treat as "unknown", not as "very old".
inline int iosMajorVersion(const std::string& version) {
    size_t i = 0;
    int major = 0;
    while (i < version.size() && version[i] >= '0' && version[i] <= '9') {
        major = major * 10 + (version[i] - '0');
        if (major > 999) return 0;  // not a version string
        ++i;
    }
    if (i == 0) return 0;  // no leading digits at all
    // After the digits we accept either end-of-string or a separator, so "15",
    // "15." and "15.8.3" all parse, while "15x" is rejected outright rather
    // than quietly read as 15.
    if (i != version.size() && version[i] != '.') return 0;
    return major;
}

// The CoreDevice display service arrived with iOS 17. Below that the service
// simply does not exist on the device, so there is nothing to negotiate.
constexpr int kCoreDeviceMinIos = 17;

// CoreDevice travels Apple's USB stack, so it survives every driver state
// except the one that removed it.
inline bool coredeviceReachable(UsbDriver d) {
    return d != UsbDriver::kLibusbReplaced && d != UsbDriver::kNonePresent;
}

// True when Apple's own stack is still the function driver, i.e. iTunes and the
// Apple Devices app still work. Both of the non-destructive states qualify.
inline bool appleStackIntact(UsbDriver d) {
    return d == UsbDriver::kApple || d == UsbDriver::kApplePlusLibusbFilter;
}

inline const char* backendName(Backend b) {
    switch (b) {
        case Backend::kCoreDevice: return "coredevice";
        case Backend::kQuickTime:  return "quicktime";
        default:                   return "none";
    }
}

inline bool parseBackendName(const std::string& s, BackendPreference* out) {
    if (s == "auto") { *out = BackendPreference::kAuto; return true; }
    if (s == "coredevice" || s == "core") { *out = BackendPreference::kCoreDevice; return true; }
    if (s == "quicktime" || s == "valeria" || s == "legacy") {
        *out = BackendPreference::kQuickTime;
        return true;
    }
    return false;
}

// The advice for making the QuickTime protocol reachable on Windows. Deliberate
// about which install to recommend: the filter leaves Apple's driver working,
// the replacement breaks it, and the second is the one people reach for first.
inline std::string quicktimeWindowsAdvice() {
    return "the QuickTime protocol needs direct access to the phone's USB "
           "interfaces, which AppleMobileDeviceService does not provide. Install "
           "libusb0.sys as an UpperFilter on the Apple USB device: Apple's driver "
           "stays in place, so iTunes, the Apple Devices app and 3uTools keep "
           "working alongside it. Replacing the Apple driver with WinUSB also "
           "works, but it breaks all of those until it is undone";
}

// The whole decision, in one place, so it can be tested exhaustively.
inline BackendChoice selectBackend(const Environment& env, BackendPreference pref) {
    BackendChoice choice;
    const int major = iosMajorVersion(env.device.product_version);

    // A forced choice is honoured when it can run, and reported when it cannot.
    if (pref == BackendPreference::kCoreDevice) {
        if (major > 0 && major < kCoreDeviceMinIos) {
            choice.reason = "iOS " + env.device.product_version +
                            " does not have the CoreDevice display service; it needs iOS " +
                            std::to_string(kCoreDeviceMinIos) + " or newer";
            choice.overruled = true;
            return choice;
        }
        if (!coredeviceReachable(env.driver)) {
            choice.reason = env.driver == UsbDriver::kNonePresent
                                ? "no USB driver claims the phone; install iTunes or the "
                                  "Apple Devices app, then reconnect the cable"
                                : "Apple's USB driver has been replaced, and CoreDevice "
                                  "travels Apple's stack. Reinstall Apple USB support -- the "
                                  "Apple Devices app or iTunes does it. Note that the libusb0 "
                                  "filter driver is the install that does not need this: it "
                                  "leaves Apple's driver in place";
            choice.overruled = true;
            return choice;
        }
        choice.backend = Backend::kCoreDevice;
        choice.reason = (major > 0
                             ? "forced with --backend; iOS " + env.device.product_version +
                                   " supports the CoreDevice display service"
                             : "forced with --backend; device version not read yet") +
                        ". This path needs Developer Mode enabled and the DeveloperDiskImage "
                        "remounted after every reboot. It is kept as a development harness for "
                        "the decode and camera pipeline, not as a supported way to use the app";
        return choice;
    }
    if (pref == BackendPreference::kQuickTime) {
        // A transport with no code behind it is still nothing the user can run.
        if (!env.quicktime_implemented) {
            choice.reason = "--backend quicktime was requested, but this build does not "
                            "implement the QuickTime USB protocol yet";
            choice.overruled = true;
            return choice;
        }
        if (!env.quicktime_available) {
            choice.reason = env.windows ? quicktimeWindowsAdvice()
                                        : "no usable libusb transport for the QuickTime protocol";
            choice.overruled = true;
            return choice;
        }
        choice.backend = Backend::kQuickTime;
        choice.reason = appleStackIntact(env.driver)
                            ? "forced with --backend; Apple's driver is intact, so iTunes keeps "
                              "working"
                            : "forced with --backend; note that Apple's driver has been "
                              "replaced, so iTunes will not see the phone";
        return choice;
    }

    // Auto.
    if (env.driver == UsbDriver::kNonePresent) {
        choice.reason =
            "no USB driver claims the phone; install iTunes or the Apple Devices app, "
            "then reconnect the cable";
        return choice;
    }

    // QuickTime first whenever it is reachable. It needs no Developer Mode and
    // no DeveloperDiskImage remount after every reboot, which CoreDevice does,
    // and it works on every iOS version rather than 17 and up.
    // Both flags, not one: quicktime_available says a transport can reach the
    // device, quicktime_implemented says this build has the code to speak to it.
    // They are set from different places, so neither implies the other.
    if (env.quicktime_available && env.quicktime_implemented) {
        choice.backend = Backend::kQuickTime;
        choice.reason = major >= kCoreDeviceMinIos
                            ? "iOS " + env.device.product_version + " could use CoreDevice, but "
                              "the QuickTime protocol is reachable and needs no Developer Mode "
                              "and no DeveloperDiskImage remount after each reboot"
                            : "using the QuickTime USB protocol, H.264 with PCM audio";
        return choice;
    }

    // Nothing below this point can offer a backend, and every branch says why.
    //
    // Auto deliberately never selects CoreDevice. That is policy rather than
    // preference. The CoreDevice display service only appears once the
    // DeveloperDiskImage is mounted, and pymobiledevice3's mounter raises
    // DeveloperModeIsNotEnabledError unless Developer Mode is already on
    // (services/mobile_image_mounter.py:77). So CoreDevice cannot be reached
    // without asking the user to find a toggle iOS hides until the device has
    // met Xcode, then reboot and confirm it. Every app this one is measured
    // against installs, opens and runs; this one has to as well. The honest
    // answer here is "not yet", never "go enable Developer Mode".
    const std::string what_now =
        env.quicktime_implemented
            ? (env.windows
                   ? quicktimeWindowsAdvice()
                   : "no libusb transport is available for the QuickTime protocol")
            : "this build does not implement the QuickTime USB protocol yet";

    if (major >= kCoreDeviceMinIos) {
        choice.reason = "iOS " + env.device.product_version + " has a CoreDevice display "
                        "service, but reaching it needs Developer Mode and a "
                        "DeveloperDiskImage remount after every reboot, and this app asks for "
                        "neither. The QuickTime USB path asks for neither either, and it is the "
                        "one this project ships. " + what_now;
        return choice;
    }

    if (major > 0) {
        // Known-old device. CoreDevice does not exist for it at all, so there is
        // no policy to decline on: the QuickTime protocol is simply the only USB
        // screen path such a device has ever had.
        choice.reason = "iOS " + env.device.product_version + " predates CoreDevice (iOS 17), so "
                        "the QuickTime USB protocol is the only path. " + what_now;
        return choice;
    }

    // Version unknown, and it would not have changed the answer: the QuickTime
    // protocol is version-independent, so the version only ever mattered for a
    // CoreDevice path this selector no longer offers. Report what is actually
    // missing rather than blaming an unread version.
    choice.reason = "no backend is reachable. The QuickTime USB protocol is the only one this "
                    "app uses and it does not depend on the iOS version. " + what_now;
    return choice;
}

}  // namespace wp
