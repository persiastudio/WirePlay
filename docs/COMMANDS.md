# Commands

**This file is the single source of truth for how to build and test this
project.** Paste it into every reply that changes code — the user deletes
`C:\Users\gabri\wireplay` between rounds, so they lose their shell history and
anything not written down here has to be re-derived from scratch.

## The rule for pasting commands

**Every command block must be complete, and must assume with certainty that
`C:\Users\gabri\wireplay` was deleted and replaced since the last message.**

That folder is wiped every round. Everything inside it is gone, not just the
source: `build/` is gone, so `cmake --build build` fails with "is not a
directory" until configure runs again. `dump.bin` is gone, so the offline test
has nothing to replay. Never hand over a partial block that starts mid-sequence
— the user cannot run it, and the failure looks like a broken build rather than
an incomplete instruction.

Steps that are idempotent (`pip install`, the DLL check) stay in the block.
Repeating them costs a second; omitting one costs a round trip.

When a flag, a path or a target name changes, update this file in the same
change. A command sheet that drifts from the code is worse than none, because it
is trusted.

Everything below runs from PowerShell in `C:\Users\gabri\wireplay`.

**Every pasted block ends with a blank line** (an extra Enter after the
last command). Without it the last line sits unexecuted on the prompt and
the user has to press Enter by hand.

**Every pasted block starts with `cls`.** The terminal carries the previous
round's output otherwise, and the user ends up scrolling through stale logs to
find where the new run begins. First line of the block, before the `cd`:

```powershell
cls
cd C:\Users\gabri\wireplay
```

## 1. Configure

Needed whenever the folder was deleted or replaced — `build/` goes with it.

```powershell
cls
cd C:\Users\gabri\wireplay
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
      -DVCPKG_TARGET_TRIPLET=x64-windows
```

Or with the preset, which needs `VCPKG_ROOT` in the environment:

```powershell
cmake --preset vs2026
```

The first configure builds SDL2 and FFmpeg from source via vcpkg. That is slow —
FFmpeg especially. Later configures reuse it.

## 2. Build

```powershell
cmake --build build --config Release --parallel
```

Or `cmake --build --preset vs2026-release`.

Both steps, every time. `cmake --build build` alone only works while `build/`
exists.

## 3. Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
python -m pytest engine/tests -q
```

`-C Release` is required, not decoration: Visual Studio generators are
multi-config, and without it ctest looks in the wrong configuration.

Expected: **4/4** and **103 passed**.

The Python suite needs `pip install pytest`. It imports only `pytest` and
`pymobiledevice3` beyond the standard library. These are development checks —
they are not needed to run the app.

## 4. DLLs next to the executable

`vcpkg.json` puts the build in **manifest mode**, so packages install into the
build tree — not under `C:\vcpkg\installed\`, which is classic mode only. With
the dynamic `x64-windows` triplet the MSBuild integration normally copies the
DLLs beside the executable during the build, so check before copying:

```powershell
Get-ChildItem build\Release\*.dll | Select-Object Name
```

If `avcodec`, `avutil`, `swscale`, `swresample` and `SDL2` are missing:

```powershell
Copy-Item build\vcpkg_installed\x64-windows\bin\*.dll build\Release\
```

## 5. Smoke test without an iPhone

Validates decode, SDL and the camera bus. Needs `pip install av numpy` —
`make_offline_dump.py` builds the test gradient with numpy, and imports it
lazily, so a missing numpy surfaces at frame 0 rather than at startup.

```powershell
python tools\make_offline_dump.py --out dump.bin --frames 300
.\build\Release\wireplay.exe --offline dump.bin --no-audio -v
```

A synthetic picture opens at 1290x2796 and the log names the decoder.

## 6. Test against a real device

The QuickTime USB backend needs no Developer Mode and no DeveloperDiskImage:

```powershell
.\build\Release\wireplay.exe --backend quicktime --engine "python engine\wireplay_engine.py" -v
```

Output-effects variants (opt-in, off by default; D3D11/OpenGL, camera included
unless `--no-camera-effects`):

```powershell
.\build\Release\wireplay.exe --backend quicktime --engine "python engine\wireplay_engine.py" --msaa 8 --aniso 16 -v
.\build\Release\wireplay.exe --backend quicktime --engine "python engine\wireplay_engine.py" --msaa 16 --aniso 16 --no-camera-effects -v
```

Preview scaling (default `lanczos`; `aniso` = anisotropic 16x, `linear` =
performance mode):

```powershell
.\build\Release\wireplay.exe --backend quicktime --engine "python engine\wireplay_engine.py" --scaling aniso -v
.\build\Release\wireplay.exe --backend quicktime --engine "python engine\wireplay_engine.py" --scaling linear -v
```

Renderers: plain run = D3D12 (auto-fallback to D3D11). Pin the old presenter
with `--renderer d3d11`:

```powershell
.\build\Release\wireplay.exe --backend quicktime --engine "python engine\wireplay_engine.py" --renderer d3d11 -v
```

It needs libusb to reach the phone, which on Windows means **libusb0.sys
installed as an UpperFilter on the Apple USB device**. That leaves Apple's driver
as the function driver, so iTunes, the Apple Devices app and 3uTools keep
working. Never replace Apple's driver with WinUSB to get this working -- that
breaks all of them.

**Hard prerequisite, proven on the target machine: Apple's usbmux service
must be answering.** With usbmuxd down (`Failed to connect to usbmuxd socket`
from pymobiledevice3), the phone exposes the capture configuration and then
never starts the session -- silence in every open-sequence variant probed.
The engine now checks 127.0.0.1:27015 before opening, tries to start/restart
the Apple Mobile Device Service, and fails loudly (fatal 5) if it stays down,
instead of hanging. Check by hand with the first command below.

Useful without any of it, to check the cable and trust state:

```powershell
python -m pymobiledevice3 usbmux list
python -m pymobiledevice3 lockdown info
```

**Do not run `pymobiledevice3 amfi enable-developer-mode`.** Nothing here needs
it. The product rule is install, open, use.

Exit codes from the engine on this path: 5 = no libusb backend, 6 = the capture
configuration could not be reached, 7 = the stream failed mid-session.

## 7. Virtual camera (optional, needs an elevated prompt)

```powershell
cmake --build build --config Release --target register_camera
regsvr32 /u build\Release\WirePlayCamera.dll     # to remove
```

After registration the filter appears as **WirePlay iPhone** in OBS and
Streamlabs.

## If a folder will not delete

A process is still holding it as its working directory, and Windows often does
not say which. The engine now runs inside a job object, so this should not
happen with a current build — for an older one, or for a process that predates
the fix:

```powershell
Get-CimInstance Win32_Process -Filter "Name='wireplay.exe' OR Name='python.exe' OR Name='python3.exe' OR Name='py.exe'" |
  Select-Object ProcessId, Name, CommandLine | Format-List

Stop-Process -Name wireplay -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='python3.exe' OR Name='py.exe'" |
  Where-Object { $_.CommandLine -match 'wireplay_engine|wireplay-engine|pymobiledevice3' } |
  ForEach-Object { Write-Host "killing $($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force }
```

If something still holds it: `Win+R` → `resmon` → **CPU** tab → expand
**Associated Handles** → search `wireplay`.

## Linux (development only)

```bash
sudo apt install cmake libsdl2-dev \
     libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 -m pytest engine/tests -q
```
