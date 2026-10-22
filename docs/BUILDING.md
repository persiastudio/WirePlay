# Building

> For the command list alone, without the explanation, see
> [`COMMANDS.md`](COMMANDS.md). It is the sheet to paste from, and it is kept
> in step with the code.

The FFmpeg dependency enables vcpkg's `nvcodec` feature (--enable-cuda --enable-nvdec --enable-nvenc): it needs NO CUDA toolkit at build or run time -- FFmpeg loads the driver dynamically; an installed NVIDIA driver is enough. NVDEC is an experimental opt-in (`--decoder cuda`); the default remains D3D11VA zero-copy. First configure after this change rebuilds FFmpeg (one long build, then incremental again).

## Windows (the supported target)

You need:

- **Visual Studio 2026** or the Build Tools, with the "Desktop development
  with C++" workload (toolset **v145**)
- **CMake 4.2 or newer** — the `Visual Studio 18 2026` generator was added in
  CMake 4.2, so an older CMake cannot generate for VS 2026 at all. VS 2022 also
  works, with the `vs2022` preset or `-G "Visual Studio 17 2022"`.
- **vcpkg**


```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT C:\vcpkg
```

### Presets

```powershell
cmake --preset vs2026        # or: cmake --preset vs2022
cmake --build --preset vs2026-release
ctest --test-dir build -C Release --output-on-failure
```

The presets need `VCPKG_ROOT` in the environment, which is how they find the
toolchain file. Visual Studio generators ignore `CMAKE_BUILD_TYPE`; the
configuration is chosen at build time, which is what the `--config Release`
below does.

Both steps, every time. `cmake --build build` on its own only works while
`build/` exists, so if you deleted or replaced the source folder -- including
`build/`, which is not part of the distributed tree -- the configure step has to
run again first. Running only the build line against a missing `build/` fails
before it compiles anything.

The same thing spelled out, with no presets:

```powershell
cd wireplay
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### What CI actually runs, and why it differs

`.github/workflows/build.yml` uses `windows-latest`, which is the
`windows-2025` image. Per the runner image manifest that is **Visual Studio
Enterprise 2022 (17.14)** with CMake 3.30.5 / 3.31.5 / 4.1.2 — not VS 2026,
despite the image also carrying the label `windows-2025-vs2026`. Since none of
those CMake versions know the VS 2026 generator, CI configures with
`"Visual Studio 17 2022"`. VS 2026 is the local developer target; the two
generators produce the same code from the same sources.

If a future image really does ship VS 2026 plus CMake ≥ 4.2, switch the
generator in that one line and nothing else has to change.

`vcpkg.json` declares SDL2 and FFmpeg, so the first configure downloads and
builds them. That takes a while — FFmpeg especially. Subsequent builds are
incremental.

The executable lands in `build\Release\wireplay.exe`.

`vcpkg.json` puts the build in **manifest mode**, so the packages are installed
into the build tree at `build\vcpkg_installed\x64-windows\bin\` -- not under
`C:\vcpkg\installed\`, which only exists in classic mode. With the
`x64-windows` (dynamic) triplet, vcpkg's MSBuild integration copies the DLLs the
executable links against next to it automatically at build time, so a local run
normally needs no extra step. Check before copying:

```powershell
Get-ChildItem build\Release\*.dll | Select-Object Name
```

If something is missing, copy from the manifest install tree:

```powershell
Copy-Item build\vcpkg_installed\x64-windows\bin\*.dll build\Release\
```

The CI workflow's bundle step copies the same set into `dist/` for packaging,
which is a different directory that the applocal step does not touch.

### USB filter driver (QuickTime backend prerequisite)

One-time, per machine: the QuickTime backend reaches the phone through libusb,
which on Windows means the libusb0 filter driver installed as an UpperFilter on
the Apple USB device. Apple's driver stays in place, so iTunes / Apple Devices /
3uTools keep working; never Zadig-replace it.

```powershell
Start-Process powershell -Verb RunAs -ArgumentList '-ExecutionPolicy','Bypass','-File','tools\install_usb_filter.ps1'
```

Already installed on the development machine; only repeat after uninstalling
(`tools\Uninstall-UsbFilter.ps1`). This stays out of the per-round command
block on purpose.

### Engine

The engine is Python, so it does not compile:

```powershell
py -3 -m pip install pyusb
py -3 engine\wireplay_engine.py --help
```

`wireplay.exe` looks for `wireplay-engine` on `PATH` by default. Either put a
small wrapper on your PATH, or always pass the command explicitly:

```powershell
wireplay.exe --engine "py -3 C:\path\to\wireplay\engine\wireplay_engine.py"
```

### The virtual camera

`wireplay_camera` (output `WirePlayCamera.dll`) builds on Windows with no extra
components. It is written against raw COM rather than the DirectShow
baseclasses, because those ship with the SDK *samples* and current SDKs do not
include them — which is why an earlier version of this project warned
"DirectShow baseclasses not found" and silently skipped the camera.

The build does not register the filter. Registration writes to `HKLM`, so it is
a separate step from an elevated prompt:

```
cmake --build build --target register_camera     # elevated
regsvr32 /u build\Release\WirePlayCamera.dll   # to remove
```

On Linux and in CI the filter is still compiled, against the stub Win32 headers
in `tests/win32_stub/`, as target `wireplay_camera_syntax`. That is a type check
and nothing more: it catches typos, missing interface overrides and wrong
signatures, but the stubs are a transcription of the COM interfaces, so an API
misremembered in both places would pass. Do not read it as "the camera works".

## Linux (development only)

Fine for working on the decode and render path. The userspace tunnel works, but
nothing here has been run against a device on Linux.

```bash
sudo apt install cmake libsdl2-dev \
     libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Tests

Three suites, each catching a different class of mistake:

```bash
ctest --test-dir build --output-on-failure   # C++: frame protocol
python -m pytest engine/tests -q             # Python: depacketiser, RTCP, protocol
```

The Python suite cross-checks the depacketiser against pymobiledevice3's own
implementation and asserts that `src/wp/protocol.hpp` still agrees with the
Python constants. The C++ suite pins those same constants from the other side.
Between them, a one-sided edit to the protocol cannot land silently.

## Working without a device

Two ways to get a byte stream in the engine's exact format:

```bash
# Synthetic, no iPhone needed. Real phone resolution, in-band parameter sets.
python tools/make_offline_dump.py --out /tmp/dump.bin --frames 300
./build/wireplay --offline /tmp/dump.bin

# Recorded from a real device, for regression work.
python engine/wireplay_engine.py --dump session.bin
./build/wireplay --offline session.bin
```

`--dump-video clip.h265` on either run writes the raw Annex-B bitstream, which
you can inspect with `ffprobe` or step through in a decoder. (The flag was
`--dump-hevc` until the decoder became codec-agnostic; it writes H.264 too.)

`tools/make_offline_dump.py` encodes with PyAV, so it needs `pip install av`.
The Python test suite needs `pip install pytest`.

## Troubleshooting

**"cannot reach usbmuxd"** — Apple Devices / iTunes is not installed, or its
service is stopped. Install it from the Microsoft Store and run it once.

**tunneld exits immediately** — on Windows the kernel TAP path needs
elevation. Either run elevated, or drop `--tunneld` and use the default
in-process userspace tunnel, which does not.

**stream starts then stalls after ~25 s** — the RTCP receiver reports are not
reaching the device. Check that outbound UDP to the tunnel address is not being
dropped by a local firewall.

**CoreDevice error 9022** — another app is holding the camera or microphone.
iOS refuses the mirror; close it.

**Picture smears under fast motion with `--ltrp`** — you are almost certainly
not on the FFmpeg decode path. Check the log line that names the decoder.
