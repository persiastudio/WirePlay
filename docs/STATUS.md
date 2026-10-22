# What has actually been verified

This exists because "it compiles" and "it works" are different claims, and a
mirroring app is the kind of thing where the difference only shows up once you
plug a phone in.

Environment used for everything below: Debian 13, gcc 14.2, cmake 4.4.4,
SDL2 2.32.4, FFmpeg 8 (libavcodec 61.19), Python 3.13, pymobiledevice3 11.36.0.

## Verified

**Engine unit tests — 56 passing.** `python -m pytest engine/tests -q`

The RTP/HEVC depacketiser is cross-checked against
`pymobiledevice3.remote.core_device.screen_stream.depacketize_hevc`, the
implementation Apple's own tooling path has been exercised against, on every
case where the two are expected to agree:

- RFC 7798 single NAL unit, Aggregation Packet, and Fragmentation Unit
  reassembly, byte-for-byte against the upstream function
- Apple's 14-byte proprietary NAL trailer stripped as an exact suffix only
- sequence gaps void the whole access unit; recovery on the next AU
- a gap inside a fragment clears the FU buffer rather than stitching
  non-contiguous payloads into one NAL
- 16-bit sequence wrap-around is not mistaken for loss
- RTCP multiplexed on the media port (PT 64–95) is rejected and excluded from
  statistics
- CSRC lists and header extensions shift the payload correctly
- RTCP Receiver Report + SDES compound is byte-identical to what Xcode's mirror
  sends

`test_app_engine_contract.py` reads the flags `buildEngineArgv` pushes in
`src/main.cpp` and asserts the engine's parser accepts every one of them, plus
that the `--backend` vocabulary is spelled identically on both sides. It exists
because of a real breakage: the app started sending `--backend` before the
engine had that argument, so argparse would have rejected the whole command line
and the engine would have exited before mirroring anything. The test was
verified by injecting a bogus flag and watching it fail.

The wire-protocol constants are read back out of `src/wp/protocol.hpp` with a
regex and asserted equal to the Python ones, and checked for duplicates within
each direction. `tests/cpp/test_protocol.cpp` pins the same values from the C++
side.

**C++ unit tests — 3 suites, 100% passing.** `ctest --test-dir build`

- `wireplay_tests` (5): frame round-trip, refusal of a zero-length frame and of
  a length above the 64 MiB ceiling, truncated-stream EOF, `readU32BE` bounds
  behaviour, and the protocol constants read back out of the header.
- `wireplay_framebus_tests` (8): the cross-process frame bus the virtual camera
  reads from. Publish/consume with a padded source stride, one-producer-only
  enforcement, consumer-before-producer failure, a flat-out producer against a
  60 Hz consumer with every consumed frame checked for tearing, oversized-frame
  rejection, staleness, a **real `fork()`ed consumer process**, and reclaiming
  the bus after the producer is `SIGKILL`ed mid-stream.
- `wireplay_backend_tests` (9): the CoreDevice-vs-QuickTime selector. Version
  parsing, iOS 18 on the Apple driver, the iPhone 7 Plus on iOS 15 both blocked
  and unblocked, iOS 16, a modern phone with the driver deliberately swapped,
  every forced `--backend`, no device attached, and an exhaustive sweep of 336
  combinations asserting that no path returns an empty reason and that an
  overrule never also selects a backend.
- `wireplay_camera_syntax`: compiles `src/dshow_filter.cpp` against stub Win32
  headers on Linux, so the filter cannot rot between Windows builds.
- `wireplay_camera_tests` (6): the camera's media-type logic — BITMAPINFOHEADER
  ABI layout and field offsets, frame-interval maths including 59.94, the 2%
  frame-rate tolerance, accept/insist/reject judgement for eleven proposed
  types, the advertised rate list, and the bottom-up DIB row flip.

Two of those tests exist because of bugs found while writing them, not because
they were planned:

- the bus name was `Local\WirePlayFrameBus` on every platform. `Local\` is
  Win32 namespace syntax; POSIX took it literally and created
  `/dev/shm/Local\WirePlayFrameBus`. The producer could reopen its own bus, so
  it looked healthy, and no consumer could ever find it.
- the bus capped height at 2160 ("4K"), so `publish` rejected **every** frame
  from an iPhone 14/15 Pro Max in portrait, which is 2796 px tall. The cap is
  now 4096.

**Virtual camera, end to end.** `tools/camera_probe.cpp` stands in for the
DirectShow filter: a separate process that opens the bus and pulls at ~60 Hz.
Run against the same 1290×2796 dump:

```
probe: 1290x2796, 168 frames read in 3 s, 112 distinct, 0 blank,
       56 repeats/waiting, producer published 240
[wireplay] done: 240 decoded, 0 decode errors, 240 presented in 3.9s
           (61.9 fps avg), software(slice-threads), 245 frames read,
           240 frames published to the camera
```

`112 distinct` is a running FNV checksum of each frame: the images are really
changing, so real content crossed the process boundary rather than a constant
buffer. `0 blank` rejects the case where the consumer reads zero-filled memory
and mistakes it for a picture. Repeats are expected — the probe polls at 60 Hz
while the producer ran at 61.9, so it sees some frames twice and misses others,
which is the intended latest-frame-wins behaviour.

**End-to-end decode and render.** A synthetic dump generated by
`tools/make_offline_dump.py` at **1290×2796 @ 60 fps** (iPhone 15 Pro Max
native resolution), HEVC Main with in-band parameter sets, fed through
`wireplay --offline`:

```
[wireplay] geometry 0x0 -> 1290x2796
[wireplay] done: 90 decoded, 0 decode errors, 90 presented in 1.5s
           (58.9 fps avg), software(slice-threads), 95 frames read
```

That run went through a real SDL2 renderer under Xvfb, with vsync on — which is
why the average lands at 58.9 rather than at the 145.9 fps the same dump gets
under SDL's dummy driver. Audio init was exercised: `48000 Hz 2 ch,
ASC=f8e64000`.
## MSVC round 2: three failures the Linux rig structurally cannot see

The user's second Windows build produced three distinct errors. All three are
genuine bugs, and each is invisible here for a stated reason.

**1. `main.cpp` -- 7 x `C2589 '('` at `std::min`.** `main.cpp` includes `<SDL.h>`
at line 11 and `wp/framebus.hpp` at line 30. SDL pulls in `windows.h` first,
with the `min`/`max` macros intact, so `NOMINMAX` defined inside `framebus.hpp`
arrives too late; the define has to be on the compile line. It was not there at
all: an earlier `CMakeLists.txt` rewrite that spliced out the camera section by
string index deleted the `wireplay` target's entire `if(MSVC)` block, silently
dropping `NOMINMAX`, `WIN32_LEAN_AND_MEAN` and a `_CRT_SECURE_NO_WARNINGS` that has
since been removed altogether (see below). That
splice was verified by "the script printed ok", not by grepping for the block.

**2. `framebus.cpp(132)` -- `C2065 'kSyncOffsetAligned'`.** The declared
constant is `kSlotOffsetAligned`. The wrong name appeared only in the
`#ifdef _WIN32` half of `FrameBus::create()`; g++ compiles the `#else` half,
which had it right. Both halves now say `kSlotOffsetAligned`, and the two
constants moved into `framebus.hpp` as `inline constexpr` so both halves and
every translation unit see exactly one definition.

**3. `dshow_filter.cpp` -- ~100 x `C2374`, then `C1003`.** `#define INITGUID`
sat before `<dshow.h>`/`<uuids.h>`, so `uuids.h` *defined* every system GUID in
that translation unit instead of declaring it. `<initguid.h>` now comes after
the SDK headers, which leaves the system GUIDs declared (resolved from
`strmiids.lib`) and only our own `DEFINE_GUID(CLSID_WirePlayCamera, ...)` a
definition.

### Guard: `engine/tests/test_windows_branches.py`

Four tests that read the sources instead of running them, because none of these
paths execute on Linux:

* every `kSomething` used inside an `#ifdef _WIN32` region must be declared
  somewhere in the tree -- exactly the shape of bug 2;
* the region walker is itself checked against `framebus.cpp` (it must see
  `CreateFileMapping` and must not see `shm_open`), so the test above cannot
  pass by collecting nothing;
* `wireplay`, `wireplay_camera`, `wireplay_framebus_tests` and
  `wireplay_camera_probe` must each carry `NOMINMAX` -- bug 1;
* `INITGUID` must not be hand-defined, and `<initguid.h>` must come after
  `<dshow.h>`, `<strmif.h>` and `<uuids.h>` -- bug 3.

Each was verified by reintroducing the bug and watching the matching test fail.

The stub harness gained `tests/win32_stub/initguid.h`, and its `uuids.h` now
records that the stub *cannot* reproduce the C2374 wall, because it declares
the system GUIDs unconditionally instead of honouring `INITGUID`.
## `_CRT_SECURE_NO_WARNINGS` is gone

It was defined on six targets. That is a mute button, not a fix: it silences
C4996 for every file in the target, including files nobody has read, so the
next genuinely unsafe call arrives without a complaint. The actual call sites
were few, and each now uses the replacement:

| was | now | where |
|---|---|---|
| `char title[160]` + `std::snprintf` | `std::string` + `to_string` | `main.cpp` window title |
| `fopen`/`fwrite`/`fclose` | `std::ofstream` in binary mode | `main.cpp --dump-hevc` |
| `getenv("TMPDIR"/"TEMP"/"TMP")` | `std::filesystem::temp_directory_path()` | `test_protocol.cpp` |
| `freopen(path, "rb", stdin)` | `_freopen_s` under `#ifdef _MSC_VER` | `main.cpp --offline` |
| `fopen` x6 | `fopen_s` under `#ifdef _MSC_VER` | `test_protocol.cpp` |

The last two are the honest cases. `FrameReader::hasPending()` polls the file
descriptor of the engine pipe and `wp::writeFrame()` takes a `FILE*`, so no
iostream can replace them; there the `_s` variant is the modern API and it is
genuinely better -- it returns the error instead of NULL plus a global errno.

`NOMINMAX` and `WIN32_LEAN_AND_MEAN` stay. Those are not suppressions: they
change what `windows.h` contains, and the first because whichever header
includes `windows.h` first decides whether `min`/`max` are macros.

### Guard: `engine/tests/test_no_crt_suppression.py`

Five tests. `_CRT_SECURE_NO_WARNINGS`/`_SCL_SECURE_NO_WARNINGS` must appear in
neither `CMakeLists.txt` nor `CMakePresets.json`; no deprecated CRT call may be
reachable by MSVC; a deprecated call is tolerated only inside the `#else` half
of `#ifdef _MSC_VER` or inside `#ifndef _MSC_VER`; every `#ifdef _MSC_VER`
branch must actually call an `_s` function, or it has no reason to exist.

The scanner strips comments first -- the first run flagged `getenv(` in the
very comment explaining that `getenv` had been replaced, which is a false
positive worth fixing in the test rather than by watering down the comment.
Comment stripping is hand-rolled because `//` inside a string literal is not a
comment, and `test_strip_comments_keeps_line_numbers_and_respects_strings`
pins both that and the line-count preservation.

Verified by mutation: re-adding the define to `CMakeLists.txt` fails two tests;
removing the `#ifdef _MSC_VER` from `test_protocol.cpp` fails two more.

**Executed, not just compiled:** the `--offline` path runs the `#else`
`std::freopen` branch, and `--dump-hevc` runs the new `std::ofstream`. The
latter produced a 306325-byte file with 248 start codes for 240 access units
(240 slices + 8 parameter sets), NAL types `{1, 32, 33, 34}` = TRAIL/VPS/SPS/PPS,
which decodes back through PyAV. The MSVC half of each `#ifdef` is not executed
here and remains unverified.
## MSVC round 3: what the second Windows build proved and what it found

The user's build confirmed three fixes from round 2: `framebus.cpp` compiled
(the `kSlotOffsetAligned` name), `main.cpp`'s `C2589` was gone (`NOMINMAX` on the
command line), and the `C2374` wall was gone (`initguid.h` after the SDK
headers). It then found four things the Linux rig still could not see.

**1. `dshow_filter.cpp(519)` -- `C3668` on `~PinEnum() override`.** `IEnumPins`
derives from `IUnknown`, which has no virtual destructor. This is the *third*
instance of that mistake: `~CWirePlayFilter` and `~CWirePlayPin` were fixed by
hand at their own sites in round 1, and the enumerator nested inside
`EnumPins()` was never searched for. `test_no_override_on_a_destructor_of_an_iunknown_derived_class`
now greps the whole file.

**2. `dshow_filter.cpp(634)` -- `C2039: 'NotifyEvent' is not a member of
`IMediaEventSink'`.** The interface has exactly one method,
`Notify(long, LONG_PTR, LONG_PTR)`. `NotifyEvent` is a helper on the SDK's
`CBaseFilter` that calls `pSink->Notify(...)` internally, and this filter does
not use the baseclasses.

This is the documented limit of the stub harness, realised. The stub declared
`NotifyEvent`, so filter and stub were wrong in the same direction and
`-Wall -Wextra` passed. The signature is now pinned on both sides by
`test_the_stub_agrees_with_the_sdk_on_imediaeventsink`, and `Release()` is
allowed alongside `Notify` because it is inherited from `IUnknown`.

**3. `main.cpp(503)` -- `C3861: '_freopen_s' not found`.** My name was wrong:
the MSVC secure CRT uses `freopen_s` with no leading underscore, same family as
`fopen_s` -- which I had used correctly in `test_protocol.cpp` two edits
earlier. `C2737` on the next line was the cascade.

**4. `framebus.cpp(321)` -- `C4267`, `size_t` to `uint32_t`.** GCC's
`-Wall -Wextra` does not cover implicit narrowing in an assignment (that is
`-Wconversion`), so this was invisible here. `row_bytes` is `width * 4` while
the bounds check limits `stride`, so the bound is real but transitive; it is now
two `static_assert`s plus an explicit `uint32_t(...)`, because `BusHeader` is a
layout shared across processes and a silent truncation there would corrupt
another process's reads.

Two more `size_t(y) * stride` mixes in the same file's memcpy loops were fixed
for the same reason: `stride` is a signed `int` in pointer arithmetic. MSVC does
not report these -- C4365 stays off even at `/W4` -- but they are worse code.

A `-Wconversion -Wsign-conversion` pass over `src/` and `tools/` now returns
only the `ULONG(InterlockedDecrement(&ref_))` idiom in the four COM `Release()`
methods, which is the canonical pattern and an explicit cast, so MSVC does not
flag it.
## First contact with a real device

An iPhone 11 Pro Max on iOS 27.2, Windows, userspace tunnel. What worked: the
device was found on usbmux, the no-admin PyTCP tunnel came up
(`fd55:7fd:772b::2/64`, mtu 16000), and RemoteXPC service discovery connected.
What did not: `InvalidServiceError: No such service:
com.apple.coredevice.displayservice`.

The service name is correct -- it is what pymobiledevice3's own
`remote/core_device/display_service.py` uses. The device was not advertising it,
because CoreDevice's developer services only appear once the Developer Disk
Image is mounted, and that mount does not survive a reboot. For iOS 27 that is
the Cryptex1 DDI installed over `cryptexd`. So this is device setup, not a
protocol failure, and it is the first step of `docs/BUILDING.md`'s device
section rather than a bug.

Two real defects did surface:

* **The retry loop never stopped.** It caught every exception, slept a second,
  and reconnected -- so a missing service produced the same traceback forever
  with no explanation. `InvalidServiceError` is now classified fatal: the engine
  logs the service table the device *does* advertise, prints the Developer
  Mode + `mounter auto-mount` remedy, sends it to the app over the pipe as a
  fatal frame, and exits 3. `FATAL_SESSION_ERRORS` is deliberately narrow;
  connection resets and timeouts still retry.
* **`break` inside a `finally`** -- a `SyntaxWarning` on Python 3.14, and it
  discarded the return value, so the fatal path's exit code would have been
  swallowed. Replaced with a flag tested after the `finally`.

`engine/tests/test_fatal_errors.py` covers both: the classification, the
service-listing helper (including a broken handshake, which must not raise
while reporting another failure), the wording of the remedy, and an AST walk
that fails if a `break` or `return` ever lands in a `finally` again. Verified by
mutation.
## The decoder is no longer HEVC-only

The QuickTime USB backend streams H.264 while CoreDevice streams HEVC, and
everything downstream of the decoder is identical for both, so the split is one
field on the wire:

* `kVideoInit` grew a fourth big-endian u32 -- the codec. `src/wp/protocol.hpp`
  has `enum VideoCodec { kCodecHevc = 0, kCodecH264 = 1 }`, the engine has
  matching `CODEC_HEVC`/`CODEC_H264`, and both are pinned from either side by
  `engine/tests/test_wire_protocol.py` and `tests/cpp/test_protocol.cpp`.
* `HevcDecoder` became `VideoDecoder::open(VideoCodec, int threads)`. The
  hardware path is unchanged: D3D11VA on Windows and VAAPI on Linux serve both
  codecs, so only the `avcodec_find_decoder` argument is codec-dependent.
* The app now opens the decoder on the first `kVideoInit` instead of before the
  loop, because that message is where the codec is declared. An access unit
  arriving before it is dropped rather than decoded by a guess.
* `--dump-hevc` is now `--dump-video`: the flag writes a raw Annex-B bitstream
  and the codec is no longer implied by its name.
* `tools/make_offline_dump.py --codec {hevc,h264}` encodes either. It also had
  a latent bug found while wiring this: it emitted `kVideoInit` with an **empty
  payload**, which the app previously ignored and now would have read as a
  zero-length header, never opened a decoder, and silently decoded nothing.

**Executed, both codecs, end to end:** `--offline` over an 180-frame synthetic
dump at 1290x2796. HEVC: `180 decoded, 0 decode errors, 180 presented in 3.0s
(60.9 fps)`, `decoder: HEVC (software(slice-threads))`. H.264: identical numbers
with `decoder: H.264`. Both published every frame to the camera bus.

Renaming the enum value in the header was verified to fail
`tests/cpp/test_protocol.cpp` (5/6).

### A pre-existing test fragility this exposed

`engine/tests/test_wire_protocol.py` imported `wireplay_engine` without putting
the engine directory on `sys.path`, and passed only because three other test
modules each did their own `sys.path.insert` and pytest happened to import one
first. Run on its own it raised `ModuleNotFoundError`. Collection order is not a
dependency a suite should have; `engine/tests/conftest.py` now sets the path
once and the three ad-hoc inserts are gone. Every test file passes both
individually and together.
## The Windows driver model was wrong, and the selector gave bad advice

This project assumed two driver states: Apple's driver owns the phone, or libusb
replaced it. There are three. `libusb0.sys` installed as an **UpperFilter** on
Apple's `usbccgp` instance leaves Apple's driver as the function driver, so
iTunes, the Apple Devices app and 3uTools keep working and libusb reaches the
phone too -- both backends at once. That is what the actively maintained Windows
mirroring projects install, and iPhoneMirror states the rule explicitly: *"Do
not use Zadig to replace the Apple parent driver with WinUSB/libusb."*

The consequence was not cosmetic. `selectBackend()` refused CoreDevice on a
machine where the filter was installed and it would have worked, told users to
"restore it with zadig" when they had never swapped anything, and claimed the
two backends could not run simultaneously. `UsbDriver` now has
`kApplePlusLibusbFilter` alongside `kApple` and `kLibusbReplaced`, with
`coredeviceReachable()` and `appleStackIntact()` naming what each state actually
permits.

Auto-selection also changed direction: it now prefers the QuickTime protocol
whenever it is reachable, because it needs neither Developer Mode nor a
DeveloperDiskImage remount after every reboot. `Environment::quicktime_available`
is documented as a promise -- it must only be set once that backend works end to
end, since auto now acts on it.

`tests/cpp/test_backend.cpp` covers 420 combinations (was 336; the extra driver
state accounts for the difference), 198 selecting. Beyond the old invariants it
asserts that no reason string contains the retracted "restore it with zadig"
advice, that every Windows dead end for the QuickTime protocol names the filter
install, and that a forced CoreDevice is never overruled while Apple's stack is
intact and the phone is new enough. Reverting `coredeviceReachable()` to the
two-state version fails the suite.

Two caveats recorded rather than smoothed over: the libusb project ranks
`libusb0.sys` last and calls filter mode unreliable, which is general guidance
this project is knowingly departing from because it is what the working iPhone
tools ship; and libusb-win32 is LGPL-3.0, so it stays a separately installed
driver plus a dynamically linked DLL and is never statically linked into the
MIT app.
## Developer Mode is refused, which retires the CoreDevice path

The user's rule is absolute: this app must never require Developer Mode, because
every alternative they compare it against installs, opens and runs. That is not
a preference to weigh, so it is now encoded rather than remembered.

It is also not circumventable from our side. `com.apple.coredevice.displayservice`
appears only once the DeveloperDiskImage is mounted, and pymobiledevice3's
mounter raises `DeveloperModeIsNotEnabledError` when Developer Mode is off
(`services/mobile_image_mounter.py:77`). The CLI's own developer tooling banner
reads "requires Developer Mode + mounted DeveloperDiskImage". So CoreDevice
mirroring cannot exist without asking the user to find a toggle iOS hides until
the device has met Xcode, then reboot and confirm it.

Consequences, all now pinned by tests:

- `selectBackend()` with `kAuto` **never** returns `kCoreDevice`, on any OS,
  driver state or iOS version. The 840-combination sweep asserts it, and asserts
  that no auto reason contains "enable Developer Mode" or "amfi".
- The version-unreadable branch used to guess CoreDevice. It no longer guesses.
  That branch's old wording was also wrong: it blamed an unread iOS version,
  when the QuickTime protocol is version-independent and the version was never
  the obstacle.
- `Environment` gained `quicktime_implemented`, separate from
  `quicktime_available`. A reachable transport with no code behind it is still
  nothing the user can run, and the two need different advice -- sending someone
  to install a driver to fix a missing feature is worse than saying either.
- CoreDevice stays reachable only through an explicit `--backend coredevice`,
  whose reason now states the cost and calls itself a development harness. It is
  what validated the decode and camera pipeline end to end; deleting it would
  leave that pipeline unverifiable.

**The honest state this leaves the project in: there is no path from an iPhone to
a frame on any machine today.** The offline dump exercises decode, SDL and the
camera bus; the QuickTime USB backend that would make a real device work is not
written. Step 6 of `docs/COMMANDS.md` now lists only what genuinely runs --
`usbmux list` and `lockdown info`, neither of which needs Developer Mode -- and
carries an explicit instruction not to run `amfi enable-developer-mode`.
## The QuickTime USB backend exists

`engine/quicktime_backend.py` implements the Valeria protocol: the hidden USB
configuration, the sync/asyn handshake, and CMSampleBuffer parsing into Annex-B
H.264. `--backend quicktime` runs it. No Developer Mode, no DeveloperDiskImage.

The wire format was taken from the MIT-licensed reference implementations
(YueChen-C/ios-screen-record, danielpaulus/quicktime_video_hack) rather than
reconstructed from prose, because the record lengths and field offsets are exact
or the handshake stalls.

34 byte-fixture tests cover the parts that can be tested without a phone:
framing across chunk boundaries, record lengths pinned against the protocol,
dictionary encoding, CMSampleBuffer parsing, avcC parameter-set extraction, and
AVCC to Annex-B conversion. Two real bugs came out of writing them: `_reply()`
and `_asyn()` declared a length four bytes short of the record they built, and a
test read the async subtype at offset 12 instead of 16.

Running `--backend quicktime` here reaches the transport and fails cleanly with
exit 3 and a `fatal` frame -- no traceback -- because the sandbox has no libusb
backend. That path is tested; the message tells the user to install libusb0.sys
as an UpperFilter and explicitly warns against replacing Apple's driver.

Two things are known gaps rather than unknowns:

- **Audio is forwarded as raw PCM (fixed).** The device sends 48 kHz stereo
  PCM; it now travels under `kAudioPcmInit` (0x22) plus ordinary `kAudioAU`
  frames and the app queues it to SDL untouched. Before this, the engine never
  wired `on_audio` at all: the phone routed its sound to the "dock", went
  silent, and the samples were discarded on arrival.
- **The libusb0 UpperFilter install is scripted** (`tools/install_usb_filter.ps1`)
  and proven on the target machine: pyusb enumerates the phone and the capture
  configuration (with the `ff/2a` interface) comes up.
## The app was silent on Windows, and --offline was broken by the same cause

`wireplay` was built with `WIN32_EXECUTABLE ON` so no console window appeared next
to the mirror. A `/SUBSYSTEM:WINDOWS` process has no console, so every
`fprintf(stderr, ...)` was discarded. The comment beside that setting claimed
diagnostics "still go to stderr, which the installer redirects to a log file" --
there is no installer and nothing redirected anything. Every run produced no
output at all, which made a failure indistinguishable from a success.

The same subsystem broke `--offline`. It opened the dump by `freopen()`-ing the
file onto `stdin`, and in a console-less process `stdin` is not a usable stream,
so the redirect failed: the app printed "cannot open" to nowhere and returned 1.
Because the SDL window opens before that, the visible result was a window
flashing uninitialised texture contents and closing -- reported as "a screen
like a VHS tape with no signal, then it closed". Nothing was wrong with the
dump, the decoder, or the phone.

Two fixes. `--offline` now opens the file directly with `fopen_s`; `FrameReader`
only ever needed a `FILE*`, so routing a file through `stdin` bought nothing and
cost the Windows path. And `attachToParentConsole()` adopts the parent terminal's
console at startup when there is none, so `-v` reaches the user; double-clicking
still finds no parent console and still shows no window, which was the point of
the subsystem in the first place.

It reopens **stderr only**. `stdout` is a data channel in `--stdin` mode, where
the engine is piped in and commands travel back out through it, so pointing
stdout at the console would break that workflow -- verified still working. It
also calls `SetStdHandle(STD_ERROR_HANDLE, ...)` explicitly, because the engine
is spawned with `GetStdHandle(STD_ERROR_HANDLE)` and it is not documented that
the CRT pushes a `freopen_s` handle down to the OS standard-handle table; without
that the engine's log would vanish the same way.

## Not verified

**The DirectShow filter has never been compiled on Windows.** It is now written
against raw COM (no SDK baseclasses, which current SDKs do not ship) and it
compiles clean under `-Wall -Wextra` against the stub Win32 headers in
`tests/win32_stub/`, as target `wireplay_camera_syntax`. That harness has already
paid for itself: it caught `FrameBus` used without its `wp::` namespace,
`override` on destructors of classes deriving from `IUnknown`, a missing
`JoinFilterGraph`, `wcsncpy_s` called with three arguments instead of four, a
`const` pointer passed to `IMediaSample::SetTime`, and type-punning between
`wp::BitmapInfoHeader` and `BITMAPINFOHEADER`.

The first real MSVC compile then found three more problems the harness could
not see (see "MSVC round 2" below). "Compiles against stubs" was never a claim
about MSVC, and this is the third time that gap has cost a round trip.

Two bugs found this way were not compile errors at all, which is the argument
for having the harness: the delivery loop ran synchronously inside
`IMediaFilter::Run`, which would have hung the whole graph and made OBS look
frozen rather than broken; and `NotifyGeometryChanged` queried the filter's own
interfaces for `IMediaEventSink`, which is implemented by the *graph*, so the
rotation notification would silently never be sent.

The harness is Linux-only by construction. Building it on Windows would put a
fake `windows.h` on the include path and strip the build of every Win32 API,
which is what happened on the first VS 2026 attempt; the target is now gated on
`NOT WIN32` and the stub header `#error`s if it is ever included on Windows.

The stubs are a transcription, not the real headers, so an API misremembered in
both places would still pass. Concretely unproven until someone builds it on
Windows and opens OBS: that the filter appears in the device list, that
`EC_VIDEO_SIZE_CHANGED` makes OBS reconnect on rotation, that two allocator
buffers survive OBS's encoder load, and that the delivery thread's pacing holds
60 fps under load.

**No physical iPhone has been attached to any of this.** Specifically untested:

- CoreDevice `DisplayService` negotiation against a real device
- the in-process userspace tunnel on Windows (it is what pymobiledevice3 itself
  defaults to, but this project has not run it)
- DeveloperDiskImage / cryptex mounting, and the trust prompt flow
- real behaviour on USB disconnect, phone lock, or app backgrounding

**Nothing has been built with Visual Studio 2026.** The generator name is
`Visual Studio 18 2026` and it was added in CMake 4.2, both confirmed against
the CMake documentation. But no VS 2026 toolchain has run over this source: the
GitHub-hosted `windows-latest` image is `windows-2025`, whose manifest lists
**Visual Studio Enterprise 2022 (17.14)** and CMake 3.30.5 / 3.31.5 / 4.1.2 —
none of which can generate for VS 2026 — so CI configures with
`"Visual Studio 17 2022"`. The `vs2026` preset is provided and parses, but the
first VS 2026 build is still a first build.

**The Windows code has never been compiled by hand.** `src/wp/process.cpp`, the
D3D11VA path in `src/wp/decoder.cpp`, and the `PeekNamedPipe` branch of
`FrameReader::hasPending` are written against documented APIs and are built in
CI, but their author ran only the POSIX paths locally. Treat the first Windows
build as a test.

**Hardware decode is untested.** D3D11VA on Windows and VAAPI on Linux both
have a fallback to software, which is what every run above used.

**Audio has not been heard.** The AAC-ELD decoder is configured and the SDL
queue path is written, but no sound has come out of a speaker. The 480-sample
frame size is set explicitly because leaving the generic AAC default of 1024 is
a known way to get clipped garbage from this codec.

**The keyframe-request packet is a guess.** `Session.request_key` builds an
RTCP Picture Loss Indication (PT 206, FMT 1) from the RFC 4585 shape. Apple's
encoder may or may not honour it — pymobiledevice3's web viewer has its own
recovery path that this did not copy. The app's **R** key and its
post-decode-error recovery both ride on this, so treat forced keyframes as
unproven until a device says otherwise.

**No real packet loss has been exercised.** The gap and reorder handling is
unit-tested, but USB is not a lossy transport, so the code that matters when it
misbehaves has never run for real.

## Known device-side behaviour to expect

From pymobiledevice3's own notes on this service, which are the closest thing
to documentation it has:

- The encoder caps around 6 Mbps. Under sustained motion it can drop capture
  resolution into the top-left corner of the frame and recover on the next
  keyframe. `--ltrp` lowers QP under motion; it does not remove the collapse.
- Without RTCP receiver reports the encoder stalls after roughly 25 seconds.
  The engine sends one per second per stream.
- Starting a mirror while another app holds the camera or microphone fails with
  CoreDevice error 9022. Close the other app.
- Two identical-model devices on one host cannot be told apart by the tunnel
  layer; pass `--udid`.

## The legacy backend is a decision, not an implementation

`src/wp/backend.hpp` picks between the two backends and is fully tested. The
QuickTime/Valeria protocol it can select **is not implemented** — there is no
code in this repository that speaks it. Selecting it today gets you an
explanation, not a picture. See [LEGACY_DEVICES.md](LEGACY_DEVICES.md).

## Devices this cannot reach

**iOS 16 and older, including the iPhone 7 Plus, are not supported by the
current backend.** `com.apple.coredevice.displayservice` requires iOS 17. The
only USB path that reaches older devices is the QuickTime/Valeria protocol,
which is a different transport, H.264 rather than HEVC, PCM rather than AAC-ELD,
and on Windows needs the Apple USB driver replaced. See
[the legacy-device notes](LEGACY_DEVICES.md).

## Deliberately removed during development

A hand-rolled HEVC SPS parser was written to size the window before the first
frame arrived. It disagreed with FFmpeg on real x265 SPS data and was deleted
rather than debugged: the decoded first frame is the authority on geometry, it
arrives within one frame interval, and a second independent opinion that can be
wrong is worse than waiting 16 ms.

## 2026-10-22 (later) — all renderers field-confirmed smooth; D3D11 letterbox wedge fixed

- D3D11 present shader used the 0..2 fullscreen-triangle trick scaled by the
  letterbox rect: on any window aspect != stream aspect a clamped-sample
  triangle spilled into the letterbox bar (the diagonal wedge on
  maximise/resize). Fixed: 4-vertex TRIANGLESTRIP, uv exactly 0..1, in
  presentGpu + presentCpu. No measurable perf role in spikes (encoder GOP).
- Vulkan/OpenGL/CUDA jitter+delay root-caused to the same disease cured in
  D3D11: forced decoder CPU copy (setCpuCopy(true) in app for GL/Vulkan,
  forced in decoder for CUDA) ran av_hwframe_transfer_data + sws_scale
  (~14 ms/frame at 2688x1242) ON THE STREAM THREAD. Fix: hardware frames
  are submitted as-is; new View::hwToBgra converts on the PREVIEW thread
  under decoder.hwMutex(); camera thread converts any hw format
  (hw_frames_ctx) on its own thread; SDL fallback keeps the old copy.
- Field (2026-10-22): Vulkan 11780/11780 in 214 s, stream decode
  0.22-0.33 ms; OpenGL 2032/2032, audio_cleared=0; CUDA 96110/96110 in
  1645 s (27 min stability run), stream decode 2.6-2.9 ms (was 13.4-14.1),
  audio_cleared=1 total. User: "Todos lisinhos!" D3D11 remains default.
  Minor residual: Vulkan accumulated ~173 audio queue trims (inaudible).
- AirPlay-over-USB spike CLOSED (see docs/AIRPLAY_USB.md conclusion):
  no pure-USB discovery channel exists on this PC (mDNS does not cross
  the CoreDevice tunnel; BLE vetoed; hotspot vetoed). 3uTools "USB
  AirPlay" is host-initiated libusb capture, same family as QuickTime.
  QuickTime remains the shipping engine; no legacy Apple driver needed.

## Output effects — MSAA + anisotropic (2026-10-22, sandbox-verified, awaiting field)

Opt-in, off by default: `--msaa 2|4|8|16`, `--aniso 1..16`, `--no-camera-effects`.
- D3D11 preview: anisotropic sampler + full mip chain on the NV12/BGRA staging
  textures (GenerateMips per frame; NV12 refusal falls back to single-level with
  a log line); MSAA renders into an offscreen multisample target and resolves
  into the FLIP_DISCARD backbuffer (sample count clamped to GPU max, logged).
- OpenGL preview: default-framebuffer MSAA (context attributes) + LINEAR_MIPMAP_LINEAR
  with GL_TEXTURE_MAX_ANISOTROPY_EXT and runtime-loaded glGenerateMipmap.
- SDL fallback: scale quality hint "2" when aniso is on (no MSAA there).
- Vulkan: unchanged blit (VK_FILTER_LINEAR); logs that the flags do not apply.
- Camera: effects ON by default when the flags are passed (user's opt-out model):
  each BGRA frame runs the D3D11 output stage at native resolution on the camera
  thread before publishing; `--no-camera-effects` skips it. Non-D3D11 renderers
  publish the untouched frame. At 1:1 the camera delta is expectedly subtle —
  the meaningful filtering is OBS's downscale — the pass exists so the flag
  really covers both outputs; its cost is the reason the opt-out exists.
- All D3D11 render sequences (preview presents + camera pass + teardown) are
  serialised on fx_mutex_: the immediate context's state is not sequence-atomic.
- Sandbox: Linux build OK, ctest 4/4, pytest 157/157. The D3D11 code paths are
  _WIN32-only and NOT compile-verified in the sandbox — first Windows build is
  the real compile check.

## Preview scaling: --scaling lanczos|aniso|linear (2026-10-22, awaiting field)

Lanczos-3 (the OBS/Streamlabs kernel), GPU-side, on by default — user: AA/aniso
showed little visible difference, Streamlabs' Lanczos looked "maravilhosa".
- `--scaling lanczos` (DEFAULT, quality): separable Lanczos-3 (H pass into an
  intermediate, V pass to screen, kernel stretched by the downscale factor,
  point-sampled taps) in D3D11 (HLSL), OpenGL (runtime-loaded GL2 shader+FBO)
  and Vulkan (embedded SPIR-V graphics pipeline replacing the blit; blit stays
  as fallback when pipeline creation fails). `--scaling aniso`: sampler path
  with anisotropic 16x default (or --aniso N). `--scaling linear`: old sampled
  path (performance mode). This is the future UI's quality/performance/custom
  mapping; camera is NOT resampled by it (native publish, as before).
- --msaa and the camera effects pass are unchanged.
- Sandbox: Linux build OK — GL+Vulkan paths COMPILE here now, GLSL validated
  with glslangValidator, SPIR-V compiled from the same kernel; ctest 4/4,
  pytest 157/157. HLSL is NOT compile-checked in the sandbox and nothing was
  runtime-verified (no GPU/display) — first Windows run is the real test.

## D3D12 presenter, default (2026-10-22, sandbox build only — field will tell)

- `--renderer d3d12` is now the default (plain run, no flag). Decode stays
  D3D11VA (FFmpeg has no D3D12 decode): the D3D11 device is created for the
  decoder as before; the NV12 slice is GPU-copied on the preview thread into
  a D3D11_RESOURCE_MISC_SHARED texture, opened as ID3D12Resource, and sampled
  by the D3D12 presenter. A shared fence orders both queues (11 signals the
  copy, 12 waits/renders/signals back, next copy waits). Zero CPU copy.
- D3D12 carries the same output stage: Lanczos-3 (default), aniso sampler,
  linear, MSAA offscreen+resolve, CPU-BGRA path, letterbox — same HLSL.
- Any D3D12 init/present-path failure at startup falls back to the proven
  D3D11 presenter automatically (`--renderer d3d11` pins it). The camera
  effects pass (`fxBgra`) is D3D11-presenter only; under D3D12 the camera
  publishes native and the log says so when --msaa/--aniso are passed.
- Parser now accepts single-dash long options (-msaa == --msaa).
- NOT verified: MSVC compile of the _WIN32 D3D12 code, 11↔12 shared-handle
  interop on the user's GTX 1080 Ti driver. Expect a possible fix-up round.

## D3D12 FIELD-CONFIRMED (2026-10-22) + undock speed

D3D12 presenter locked in: user reports best image quality of the project so
far (D3D12 + Lanczos-3 + MSAA 8x), 1246/1246 presented, 53.4 fps avg over the
23 s excerpt, 0 errors. Interop breakage was root-caused by instrumentation:
CreateSharedHandle needs D3D11_RESOURCE_MISC_SHARED_NTHANDLE (E_INVALIDARG
without it) — fixed and field-verified in the same round.

Undocking felt slow (~10 s perceived). Teardown audit: the release drain
always burned its full 1.2 s cap even when the phone answered immediately,
and the release poll slept 250 ms between enumerations. Drain now exits on
silence-after-answer (silence before any answer still waits the cap; message
sequence unchanged), poll is 50 ms, and close_session logs
`teardown timing: total %.2f s`. New regression test pins the fast drain
(158 pytest). Awaiting field timing.

Undocking, field follow-up: stubborn-phone runs took 4.79 s (three disables
~2 s apart). Cadence changed to up to six asks with 0.75 s polls (same
worst-case total): stubborn case now closes in 2.14 s, good case ~2.0 s,
and the shell prompt lands immediately after the app's done line -- the
perceived post-done lag was engine teardown variance all along. The
temporary "[wireplay] exiting" marker was removed again.

## MILESTONE 2 (2026-10-22)

Everything above this line is Milestone 2: D3D12 default presenter with the
best field-confirmed image quality, Lanczos-3 scaling, MSAA/aniso, ~2 s
undock, camera live. Snapshot + rollback plan: `docs/MILESTONE_2.md`.
User decisions for what follows: USB stays in Python; everything else
ports to C++ step by step; no engine/app merge. Plan: `docs/DEPYTHONIZATION.md`.

## Despythonização, passo 1 — motor sem pymobiledevice3 (2026-10-22)

O caminho CoreDevice saiu do `engine/wireplay_engine.py` (895 linhas mortas:
depacotador HEVC, Sessão/túnel RTP, `amain`, flags `--ltrp`/`--tunneld`/
`--display-id`/`--tiles`/`--no-fec`). O motor caiu de 1531 para ~640 linhas e
agora importa só **pyusb + stdlib** — provado no sandbox bloqueando o import
de pymobiledevice3: o motor sobe e chega até a descoberta USB. `--backend
auto` e `quicktime` são a mesma coisa; `coredevice` responde erro claro
(exit 2) em vez de quebrar. O app parou de enviar `--ltrp`/`--tunneld`
(contrato app↔engine, que pegou o resíduo sozinho). Testes do caminho morto
(`test_depacketizer.py`, `test_fatal_errors.py`) removidos; `annexb` genérico
mantido como helper de fio. **pytest 103/103, ctest 4/4.** Instalações novas
não precisam mais de `pip install pymobiledevice3` nem PyAV/numpy para o
motor — falta só o pyusb.

Falta validar em campo (1 run): build + Docking normal, conferindo que o
console mostra QuickTime sem nenhuma menção a túnel/CoreDevice.

## Bug do registro da câmera — RegisterFilter com prf2 nulo (2026-10-22)

`regsvr32` quebrava com `0xC0000005` e o target `register_camera` falhava
(MSB8066) mesmo em prompt elevado. Instrumentação com `FilterDbg` isolou:
o crash era DENTRO de `IFilterMapper2::RegisterFilter`, chamado com
`prf2=nullptr` + categoria — o mapper desreferencia a `REGFILTER2` para
montar a entrada da categoria. Correção: `REGFILTER2` v1 completa
(MERIT_DEFAULT, 1 pino de saída de vídeo) + `szInstance`. Campo confirmou:
`regsvr32 exit 0`, `register_camera` limpo. As escritas manuais no registro
(InprocServer32 + Instance) sempre funcionaram — a câmera "existia" mesmo
com o registro quebrando no meio. Instrumentação temporária removida.

## Despythonização passo 1 VALIDADO EM CAMPO + registro da câmera (2026-10-22)

Run completo com o motor enxuto: Docking, 6874/6874 apresentados, 0 erros,
D3D12, teardown 2.04 s. `register_camera` limpo (bug do `prf2` nulo fechado
na seção anterior). Passo 1 encerrado: o motor roda só com pyusb + stdlib.

### Mini-spikes de frametime — análise dos dois logs do usuário

O usuário notou mini-travadas na build atual e levantou regressão da
despythonização. Comparação janela a janela (estado estável, sem rotação):
AMBOS os logs mostram o mesmo rabo estrutural — fps 58.8–59.3, gap p50
16.7–16.8, p95 18–21, p99 ~29–30, `pres_gap>25ms` ~5–7 por janela de ~300,
`pts_holes` ~5–7. O caminho de mídia NÃO mudou no passo 1
(`quicktime_backend.py` byte a byte igual; no motor só saiu o caminho
CoreDevice; no app só saíram as flags `--ltrp`/`--tunneld`, que nem eram
enviadas) — não há mecanismo para regressão. As janelas ruins do run atual
(40–43 fps) coincidem com as trocas de geometria (rotação 2688x1242 ↔
1242x2688) e a janela com `late=7` tem `rx_gap` de 118 ms + decode de
15–18 ms: chegada, não apresentação. Veredito: variância de sessão sobre o
rabo conhecido (IDR/GOP + rotação), não regressão. Se o usuário quiser
certeza experimental: A/B no mesmo jogo/cena com a pasta do backup do
Milestone 2 (externa) vs atual, com `WIREPLAY_TRACE=1`.

### Mini-spikes: causa localizada pelo tracer (2026-10-22)

Run `WIREPLAY_TRACE=1` no Brawl Stars. Estado estável (janelas 2–4):
- IDR a cada ~510 ms, 30 AUs, **~355 KB** (P-frames: ~31 KB).
- TODO gap USB >28 ms do estado estável é o IDR: **~29,5 ms, relógio**
  (506–516 ms após o anterior — o período do GOP). 20 IDRs/janela =
  20 gaps/janela; `p99=29,9–30,1` É esse gap; `pres_gap` máx 34–35 ms.
- Pipe NÃO bloqueia em estado estável (só no startup e no `kEnd`):
  app e pipe inocentes. Decode 0,19–0,20 ms.

Ou seja: o frame IDR chega ~13 ms atrasado (intervalo de 29,5 em vez de
16,7 ms) — o encoder do telefone leva um beat a mais no keyframe gigante,
a cada meio segundo. É a mini-travadinha periódica. Apresentação não pode
vencer chegada: só buffer absorve. O scheduler já tem a alavanca
(`--cushion`, default 17; deadline = instante PTS + offset EMA + cushion).
Próximo passo: A/B `--cushion 32` na mesma cena — sucesso = `usb gap`
idêntico (lado do telefone não muda) e `pres_gap>25ms` → ~0. Custo:
~15 ms de latência a mais. Frames reais, apresentados uma vez — só o
agendamento anda, nada de duplicação.

### A/B `--cushion 32`: hipótese FALSIFICADA (2026-10-22)

Mesma cena, cushion 17 → 32: `pres_gap>25ms` continuou 5–7/janela, máx
33–35 ms. **Nada mudou.** O motivo está no próprio log: `pts_holes`
(PTS de frames apresentados com salto >25 ms) ≈ `pres_gap` em TODAS as
janelas, nos dois runs. O PTS vem direto do `presentation_time` do
dispositivo (`quicktime_backend.py:1077`) — **a timeline do stream já
nasce com o beat faltando no IDR**: o encoder do telefone pula um
instante de captura a cada GOP (~0,51 s) ao emitir o keyframe de
~355 KB. O scheduler segue o PTS; cushion maior só desloca o relógio
inteiro, o buraco vai junto. `--present arrival` sofre do mesmo buraco
(a chegada também é 29,5 ms). Apresentar "suave" exigiria exibir algo no
beat vazio = frame duplicado/interpolado = **FRAME FAKE JAMAIS**.
Veredito: teto estrutural do encoder QT, agora medido com precisão
(1 beat de ~33 ms a cada ~0,51 s; ~2×/s). Manter `--cushion 17`
(default): 32 só adiciona latência. Caça ao p99 ENCERRADA como
estrutural — não é regressão, não é o nosso pipeline (pipe sem bloqueios
em jogo, decode 0,2 ms, `late` ~0).

### `--present arrival` testado e DESCARTADO; cushion default = 0 (2026-10-22)

Decisão do usuário após o A/B: cushion só desloca o relógio (não absorve
o buraco de PTS do IDR), então o default caiu para **0** — latência mínima
pura; `--cushion` continua existindo como opt-in. O run de controle com
`--present arrival` confirmou a previsão e piorou tudo: `pres_gap>25ms`
11–13/janela (contra 5–7 do PTS), máx 38–39 ms — apresentar na chegada
expõe o jitter cru da chegada em vez da cadência do PTS. `arrival` fica
como modo diagnóstico; o default `pts` é o caminho. Caça ao p99
encerrada de verdade: buraco é da timeline do encoder do telefone.

### Correções do usuário + próxima hipótese (2026-10-22)

Campo: (1) `--cushion 0` TROUXE o jitter de volta; **18 foi o valor
bom** → default volta para 18 (0 e 32 rejeitados em campo; 17≈32 nas
métricas, mas a fase do deadline muda a percepção). (2) O frametime fica
limpo por **4–5 s** e então spike de novo — periodicidade longa demais
para o GOP puro de 0,51 s; algo nosso entra em fase. Suspeita principal:
a thread da câmera (readback+publish todo frame, total máx 16–24 ms)
disputando o `fx_mutex_` com o present do preview. A/B decisivo e
grátis: `--no-camera` (prioridade do usuário: frametime >> câmera).
(3) NEED: já sai ANTES do parse do FEED (linha 1047), 1/FEED, 9 créditos
primados; resíduo possível = pump monothread despachando a mensagem
anterior (poucos ms no IDR). Se o A/B da câmera não explicar, o próximo
passo é instrumentar a latência real FEED→NEED.

### NEED no microscópio (2026-10-22)

A/B `--no-camera`: spikes continuam (pres_gap 4–6, idêntico) → thread
da câmera INOCENTE. Por insistência (fundada) do usuário, o backend ganhou
instrumentação de latência do NEED sob `WIREPLAY_TRACE=1`: por janela de
600 FEEDs, `[trace] need window` traz dispatch máx (read→despacho), write
máx (a escrita USB em si), feed-busy máx (parse+forward segurando o pump)
e NEED→FEED máx (resposta do telefone), além de linha imediata
`[trace] slow NEED` quando o NEED sai >8 ms após o retorno do read.
pytest 103/103. Aguardando run de campo.

### NEED medido e INOCENTADO (2026-10-22)

Run com a instrumentação nova, 1200 FEEDs: **dispatch máx 0,54 ms,
write máx 0,17 ms, feed-busy máx 1,36 ms, slow NEEDs 0**. O vale sai
menos de 1 ms depois que o read retorna — fisicamente incapaz de perder
um slot de 16,7 ms. `NEED->FEED` máx 30–35 ms é a resposta no IDR (o
buraco de sempre). A suspeita do usuário foi checada com número e
morreu com número: o beat de ~33 ms a cada ~0,51 s nasce no encoder do
telefone. Câmera também inocente; arrival descartado; cushion 18 é o
default por sintonia de campo.

Anomalia separada no mesmo run (olho aberto): às 16:28:24 um
`pipe write blocked 115,3 ms` (áudio, fila 12) com `rx_gap` de 131 ms e
`pres_gap` máx 142 ms no app — o app ficou ~115 ms sem ler o pipe. É
outra classe de spike (rara, grande, LADO DO APP), distinta do beat do
IDR. Se recorrer, a próxima caça é essa: onde a thread de leitura do
app estaciona por >100 ms.

## Despythonização CONCLUÍDA (2026-10-22)

Passo 2 implementado: preflight usbmux agora é do app C++
(`ensureUsbmuxPreflight` antes de lançar o motor — Winsock + SCM,
start/restart do Apple Mobile Device Service, poll de 15 s, falha alta
no lugar). O motor mantém o dele como defesa (fatal 5 standalone).
Passo 3: auditoria — constantes duplicadas só na fronteira
processo/linguagem, pinadas por testes de contrato; nota em
`docs/WIRE_PROTOCOL.md`. Passo 4: fechado sem mexer — pela própria
regra do plano, a cola (fila/Sink/stats) só moveria com prova A/B de
latência, e as medições do NEED/pipe não atribuem latência nenhuma a
ela (feed-busy máx 1,36 ms). Sandbox: build, ctest 4/4, pytest 103.
Resta UMA validação de campo do passo 2 (run normal). Detalhes em
`docs/DEPYTHONIZATION.md` (seção Completion). O que sobrou de Python é
só a ilha USB sancionada: interpretador + pyusb + backend + launcher.

## Passo 2 VALIDADO em campo; cushion fecha em 17 (2026-10-22)

Dois runs normais com o preflight no app: nenhuma linha de preflight
(usbmux de pé = silêncio, como projetado), Docking normal, 1119/1119 e
5524/5524 apresentados, 0 erros. **Despythonização encerrada de fato.**
Cushion: depois de testar 0 (jitter), 32 (nada) e 18, o usuário fixou
**17** como default definitivo — é o valor que estava lá desde o início
da sintonia. Nem 1 ms a mais de discussão sobre isso.

### Os spikes são do tamanho do IDR (observação do usuário, 2026-10-22)

O usuário notou: **mexendo a tela, os spikes somem ~99% do tempo.**
Cruzando com os logs dele, a correlação é direta com o TAMANHO do
keyframe, não com o GOP em abstrato:

| Janela | IDR médio | gaps >28 ms | gap p99 | pres_gap |
|---|---|---|---|---|
| 16:40 (cena pesada) | 528–660 KB | 19–21 | 43,8–51 ms | 14–20 |
| 16:41 (mista) | 313–353 KB | 8–19 | 29,5–45 ms | 3–10 |
| 16:41 (movendo) | 201–213 KB | 0–1 | 22,0 ms | 1–2 |
| 16:28 (Brawl lobby) | 325–378 KB | 11–21 | 29,6–33 ms | 5–8 |

Mecanismo: é o rate control do VideoToolbox no telefone. Em cena
quase estática os P-frames custam 20–35 KB (só o delta), e o encoder
concentra o orçamento no keyframe periódico gigante (500–660 KB);
codificar+entregar esse monstro leva 1–2 beats extras, e o VBV pode
pular um instante depois dele. Com a tela mexendo, TODO frame custa
parecido, nenhum pico existe, e a cadência sai uniforme em 16,7 ms —
fps 59,6–59,7, p99 de 22 ms. Não é um "detector de atualização"
por cima: é o próprio H.264 (P = delta barato, I = frame inteiro).
O protocolo QT não expõe bitrate/GOP pro host (qvh-verificado;
`T_APP_SET_BITRATE` não tem implementação no caminho QT) — não há
alavanca nossa. Caracterização final do beat: **estrutural,
dependente de conteúdo, some em movimento.** Frametime encerrado.
