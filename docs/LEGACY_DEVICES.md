# iPhones this build cannot reach, and why

The app's only implemented backend is `com.apple.coredevice.displayservice`,
which needs **iOS 17 or newer**. An iPhone 7 Plus stops at iOS 15, so it is
outside what this build can do. This file records what the alternative is, what
it costs, and what is still unverified — because the answer is not "impossible",
it is "possible with a trade you should choose deliberately".

## The two protocols are unrelated

| | CoreDevice DisplayService | QuickTime / Valeria |
|---|---|---|
| Minimum iOS | 17 | much older; this is what QuickTime Player and Xcode ≤14 used |
| Video | HEVC over RTP/UDP (RFC 7798) | H.264 in a proprietary framing |
| Audio | AAC-ELD 48 kHz stereo | 48 kHz PCM |
| Needs a DeveloperDiskImage | yes | **no** |
| Needs the trust prompt / pairing | yes | **no** |
| Driver on Windows | Apple's, from iTunes / Apple Devices | **must be replaced** |
| Used by | Xcode 15+ mirroring | QuickTime Player, 3uTools, 3uAirPlayer |

So the device the project was built against and the device an iPhone 7 Plus can
use share nothing but the cable.

## Why 3uAirPlayer worked and why that is not free

3uAirPlayer is an **AirPlay receiver**: the phone streams to it over the local
network, using the AirPlay protocol the phone already knows how to speak to any
Apple TV. That is why it reached an iPhone 7 Plus with no driver changes — no
USB screen protocol is involved at all. It is also why it is slow and blocky:
the transport is Wi-Fi, and the encoder is the phone's AirPlay encoder, not its
screen-mirroring encoder.

Getting that behaviour for free means reimplementing the AirPlay receiver side,
including the FairPlay handshake. That is the path this project deliberately
avoided, and it is recorded in [LEGAL.md](LEGAL.md).

## The USB path that does reach iOS 15

The QuickTime protocol is fully documented and MIT-licensed:
[danielpaulus/quicktime_video_hack](https://github.com/danielpaulus/quicktime_video_hack)
ships `doc/technical_documentation.md` plus binary dumps of every message,
specifically so it can be reimplemented in another language test-first. The
transport is the **Valeria** USB configuration, selected by a control request
that switches the device's descriptor set.

On Windows it has a hard cost. From
[ios-screen-record's own Windows notes](https://github.com/YueChen-C/ios-screen-record/blob/master/windows.md),
verified against the file rather than remembered:

> 因为 iOS Screen协议无法通过 AppleMobileDeviceService.exe 跟iPhone通讯，所以需要用新的驱动通讯方案

*"Because the iOS Screen protocol cannot talk to the iPhone through
AppleMobileDeviceService.exe, a new driver is required."* Their documented
sequence is: install a libusb driver with zadig, **kill
AppleMobileDeviceService.exe**, run their `usbmuxd.exe`, then pair.

That is one way to get a driver, and it is the destructive one. There are two,
and the difference decides whether the app is usable day to day.

**The filter install (the normal one).** `libusb0.sys` goes in as an
**UpperFilter** on Apple's `usbccgp` device instance. Apple's driver stays the
function driver, so iTunes, the Apple Devices app and 3uTools all keep seeing
the phone, and libusb can reach it too. Both backends are available at once.
This is what the actively maintained Windows mirroring projects install, and
[iPhoneMirror](https://github.com/RayrenSX/iPhoneMirror) states it as a rule:
*"Do not use Zadig to replace the Apple parent driver with WinUSB/libusb."* Its
driver checks are "the Apple USB parent still uses usbccgp" plus "the device has
the libusb0 UpperFilter". They ship libusb-win32 1.2.6.0 in a separate driver
installer for exactly this.

**The replacement (rare, and what the quote above describes).** Zadig swaps
Apple's driver out for WinUSB or libusb. The QuickTime protocol works and
CoreDevice stops, because CoreDevice travels Apple's stack. iTunes and the Apple
Devices app stop seeing the phone until it is undone. Nothing in this project
should recommend it as the first move.

Two honest caveats on the filter path, because they are the ones that would
surprise someone:

- The libusb project itself ranks `libusb0.sys` last and says filter mode is
  unreliable, recommending device-driver mode instead. That is general guidance
  for arbitrary hardware; for iPhone capture on Windows the filter install is
  what the working tools actually ship. Worth knowing which side of that
  disagreement this project is on.
- libusb-win32 is **LGPL-3.0**. It is a separately installed kernel driver plus
  a dynamically linked user-mode DLL, which LGPL permits, and it is the same
  arrangement as FFmpeg here. It must never be statically linked into the
  MIT-licensed app.

And still true either way: qvh's author gave up on Windows (*"I have given up on
windows support :-)"*), so there is no working reference binary to lean on. A
port is real work.

## What was decided

Both backends ship in one app, selected automatically from the iOS version the
device reports, with `--backend` as an override. That is implemented:
`src/wp/backend.hpp` holds the decision, `tests/cpp/test_backend.cpp` pins every
branch, and `wireplay --backend` accepts `auto`, `coredevice` and `quicktime`.

**The QuickTime protocol itself is not implemented.** The selector can choose it
and will explain what is missing; no code here speaks Valeria yet.

Auto-selection has a limit worth stating, because it cannot be engineered away:
the version is read over lockdown, which needs *some* working driver first. So
the sequence is necessarily "find a driver, read the version, then choose". On
Windows there are three driver states to distinguish, not two, and
`src/wp/backend.hpp` models all three: Apple alone, Apple plus the libusb0
UpperFilter, and Apple's driver replaced. Collapsing the first two -- which an
earlier revision of this file did -- makes the selector refuse CoreDevice on a
machine where it would have worked, and tell the user to undo a driver swap they
never made.

If only Apple's driver is present and the phone is on iOS 15, the app cannot
quietly switch to the QuickTime protocol, because that protocol needs direct USB
access. It reports the situation and names the filter install, not zadig.

The options that were considered:

1. **Do nothing for iOS ≤16 and say so.** The app supports iOS 17+ over USB with
   the Apple driver intact. Honest, but it excludes an iPhone 7 Plus entirely.
2. **Add a QuickTime backend behind the libusb0 filter install.** Free, no
   royalties, works on every iOS version, and the protocol is documented and
   MIT. With the filter install the user does **not** give up iTunes -- that was
   the assumption when this option was first written down, and it was wrong.
   This is the option the project is pursuing, and it is the intended default
   backend.
3. **Talk to Apple's own driver the way the commercial tools do**, via the
   `AppleMobileDeviceSupport` DLLs. iTunes keeps working. Those DLLs are
   proprietary and their redistribution terms are not ones this project can
   rely on, so this option is off the table on licence grounds even though it is
   the nicest technically.

## Unverified, and what it would take to check

- **Whether an iPhone 7 Plus on iOS 15 actually speaks Valeria.** The protocol
  predates the device, so it almost certainly does, but no device has been
  attached to any of this project. One `qvh diagnostics` run on macOS or Linux
  would settle it in a minute.
- **Whether the libusb0 filter install works on a current Windows 11 build.**
  The working projects ship it, but nothing here has installed it. Only testable
  on a Windows machine with a phone attached -- and unlike the old plan, testing
  it should not cost the user their iTunes.
- **Frame rate and latency on a 2016 device.** The A10's H.264 encoder is not
  the same part that produces the HEVC stream on a 15 Pro Max, and nothing here
  measures it.

## What a port would look like

The protocol work is small compared to the driver work. A new engine backend
would: find the USB configuration whose interface has class `0xFF`, select the
AV configuration (subclass `0x2A`, four bulk endpoints), send the `CVRP` request
and parse the response (it already carries the SPS/PPS as NALUs), then read
`ASYN`/`FEED` packets for H.264 and PCM. Everything downstream — depacketising,
FFmpeg decode, SDL render, and the frame bus that feeds the OBS camera — already
exists and does not care whether the video arrived as HEVC-over-RTP or
H.264-over-Valeria.
