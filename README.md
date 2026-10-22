# WirePlay

iPhone screen and audio on your Windows PC, over the USB cable. No Wi-Fi video
path, no account, no watermark, no trial timer. Free, and the source is here.

> **Read [STATUS.md](docs/STATUS.md) first.** It states plainly which parts of
> this have been verified and which have not. Nothing here has been run against
> a physical iPhone yet, and the Windows code paths have never been compiled by
> their author's hands — the build is wired up and exercised in CI, but you
> should treat the first run as a test, not a product.
>
> **Requires iOS 17 or newer.** The backend is Apple's CoreDevice display
> service, which does not exist before iOS 17. An iPhone 7 Plus cannot use this
> build; what that would take is in
> [docs/LEGACY_DEVICES.md](docs/LEGACY_DEVICES.md).

## Why not AirPlay

Every free Windows "iPhone mirror" app is an AirPlay receiver, and AirPlay's
video travels over Wi-Fi. That is where the lag, the macroblocking and the
dropouts come from: the phone is re-encoding for a lossy radio link and your
router is the bottleneck.

WirePlay does not use AirPlay. It uses the path Apple's own tooling uses —
`com.apple.coredevice.displayservice`, the service behind Xcode's device
mirroring and QuickTime's iPhone recording. The phone encodes HEVC, hands it to
the USB transport as RTP, and it arrives on your machine without ever touching
the network.

That has three consequences worth knowing up front:

- **iOS 17 or later**, on a device paired to the PC (the "Trust This Computer"
  prompt). Older iOS has no CoreDevice, and there is no USB path at all there.
- **Apple Devices or iTunes must be installed**, because they provide the USB
  drivers Windows needs to see the phone. They can be closed afterwards.
- **The encoder is the phone's, not ours.** Quality is capped by what iOS is
  willing to emit — around 6 Mbps, with a documented behaviour where it drops
  capture resolution during heavy motion and recovers on the next keyframe.
  `--ltrp` measurably improves that (see below); it does not remove it.

## Resolution and framerate

There is no device database to get this wrong. The phone encodes its actual
screen at its actual refresh rate, and the window sizes itself from the first
decoded frame — so portrait, landscape, and the flip between them are all
handled by observation rather than by a lookup table keyed on a model string.

The one thing that caps the framerate is the RCTL rate-control loop the device
runs. The engine sends RTCP receiver reports every second (without them the
device's encoder concludes nobody is watching and stalls after ~25 seconds) and
the app can request a keyframe on demand.

## Install

1. Install **Apple Devices** (or iTunes) from the Microsoft Store and run it
   once, so the USB drivers land.
2. Install **Python 3.11+** from python.org. Tick "Add to PATH".
3. `pip install pymobiledevice3`
4. Unpack the WirePlay build, or build it yourself (see
   [docs/BUILDING.md](docs/BUILDING.md)).
5. Connect the iPhone, unlock it, tap **Trust This Computer**.
6. Run `wireplay.exe`.

No administrator rights are needed: the tunnel is built in-process on a
pure-Python network stack. If that path misbehaves on your machine,
`wireplay.exe --tunneld` uses the kernel TAP device instead, which does need
elevation.

## Usage

```
wireplay.exe                              mirror the first device on USB
wireplay.exe --udid 00008120-001A2C3D...   pick a specific device
wireplay.exe --ltrp                        sharper under motion (see below)
wireplay.exe --no-audio                    video only
wireplay.exe --fullscreen                  F11 also toggles
wireplay.exe --dump-hevc clip.h265         save the raw bitstream
wireplay.exe --offline clip.bin            replay a recorded session
wireplay.exe --no-camera                   do not publish to the OBS camera
```

Keys: **F11** fullscreen · **K** force keyframe · **R** restart the stream ·
**Esc** quit.

### `--ltrp`

Long-term reference pictures give the encoder more to predict from, which
lowers QP under motion — measurably, roughly 19 versus 23–31 on a hard swipe.
Browser-based receivers keep it off because WebCodecs and VideoToolbox mishandle
the long-term references and smear the picture; FFmpeg decodes them correctly,
so WirePlay can take the quality win. Try it and compare.

## Using it as a camera in OBS and Streamlabs

WirePlay installs a DirectShow capture filter called **WirePlay iPhone**, so it
shows up in the video-capture device list of OBS, Streamlabs, Discord, Teams and
Chrome — no window capture, no cropping, no "is the window behind something?"
problems.

```
OBS → Sources → Video Capture Device → WirePlay iPhone
```

Registering it writes to `HKLM`, so it is one elevated step:

```
regsvr32 WirePlayCamera.dll        # in an admin shell
regsvr32 /u WirePlayCamera.dll     # to remove
```

Run `wireplay.exe` first, then pick the device in OBS. Before a phone is
attached the camera shows a dark grey placeholder rather than black, because a
black frame in OBS is indistinguishable from a broken connection and sends
people debugging the wrong thing. The picture switches to the phone's real
resolution as soon as the first frame lands, and rotation triggers a reconnect
at the new size.

DirectShow rather than the Media Foundation virtual camera because MF needs
Windows 11 22H2 and OBS builds older than 28 do not see it; DirectShow is what
every one of those apps has always enumerated.

**This part is not verified.** The frame bus the camera reads from is tested
here, including across a real process boundary at 1290×2796, but the COM filter
around it has never been compiled — see [STATUS.md](docs/STATUS.md).

## Older iPhones

The implemented backend needs **iOS 17 or newer**. Below that the only USB path
is the QuickTime/Valeria protocol, and on Windows it cannot go through
`AppleMobileDeviceService` — it needs the Apple USB driver replaced, which stops
iTunes from seeing the phone until you swap it back.

WirePlay picks the backend from the iOS version it reads off the device, and
`--backend` overrides it:

```
wireplay.exe --backend auto         default: CoreDevice on iOS 17+, QuickTime below
wireplay.exe --backend coredevice
wireplay.exe --backend quicktime
```

When neither backend can run, it says what is missing and what to do rather than
failing quietly. **The QuickTime backend is selected but not implemented** — the
selector is written and tested, the protocol is not. Trade-offs and plan:
[docs/LEGACY_DEVICES.md](docs/LEGACY_DEVICES.md).

## How it fits together

```
 iPhone ── USB ──> pymobiledevice3 tunnel ──> CoreDevice DisplayService
                        (GPL-3.0)                 HEVC + AAC-ELD over RTP
                                                     │
                                    engine/wireplay_engine.py   ← separate process
                                                     │  framed protocol on a pipe
                                                     ▼
                                            wireplay.exe  (MIT)
                                     FFmpeg decode → SDL2 present + audio
```

The engine is a separate process on purpose. `pymobiledevice3` is GPL-3.0;
running it as a child over a pipe keeps the licence boundary at a process edge,
so the application stays MIT while still shipping the engine in the same
folder. Details and the rest of the dependency licences are in
[docs/LEGAL.md](docs/LEGAL.md).

The pipe format is documented in [docs/WIRE_PROTOCOL.md](docs/WIRE_PROTOCOL.md)
and asserted from both sides by tests, because a mismatch between the Python
and C++ constants would be silent corruption rather than an error.

## What is missing

Being direct about the gaps, because a mirroring app with holes in it is worse
than no app:

- **No touch input back to the phone.** The engine accepts `kTouch` and
  `kButton` frames and the device-side HID service exists, but the app does not
  send them and neither path has been exercised.
- **No installer.** You get a folder with an exe and some DLLs.
- **No multi-device UI.** `--udid` selects; there is no picker.
- **DRM content will not appear correctly.** Netflix, Apple TV+ and friends
  blank their video for any non-Apple receiver. That is not fixable here.

## Development

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure          # C++ unit tests
python -m pytest engine/tests -q                    # engine unit tests

# Record a session on a real device, then develop against the recording:
python engine/wireplay_engine.py --dump session.bin
./build/wireplay --offline session.bin

# Or generate a synthetic one, no device needed:
python tools/make_offline_dump.py --out /tmp/dump.bin --frames 300
./build/wireplay --offline /tmp/dump.bin
```

The synthetic dump is what makes the decode and render path testable without an
iPhone attached: it emits the exact byte stream the engine would, at a real
phone resolution, and CI runs the whole pipeline against it.

## Licence

The application is MIT. The bundled engine is GPL-3.0 and remains a separate
program. FFmpeg is LGPL-2.1+ and is linked dynamically; SDL2 is zlib. See
[docs/LEGAL.md](docs/LEGAL.md).
