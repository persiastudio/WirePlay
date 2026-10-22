# Milestone 2 — state snapshot and rollback plan (2026-10-22)

**Definition:** everything field-confirmed up to and including the D3D12
presenter and the fast undock. The user keeps an external backup of the
project folder at exactly this state; this document says what "this state"
is and how to get back to it.

## What Milestone 2 contains (all field-confirmed on the target machine)
- QuickTime-over-USB engine (Python), 60 fps on iPhone 11 Pro Max / iOS 27.2,
  `--audio-delay` sync, undock ≈2 s (`teardown timing` log).
- Presenters: **D3D12 default** (best image quality, user verdict) with
  automatic fallback to D3D11; Vulkan and OpenGL also smooth. Decode stays
  D3D11VA zero-copy; NV12 crosses 11→12 via shared texture + shared fence.
- Output stage: `--scaling lanczos` default (Lanczos-3), `aniso` (16x),
  `linear`; `--msaa 2|4|8|16`, `--aniso`, `--no-camera-effects`.
- Virtual camera "WirePlay iPhone" live in Streamlabs; F8 preview pause;
  window spec (half phone res, centred, ≤90% usable bounds).
- AirPlay-over-USB investigated and closed-blocked (`docs/AIRPLAY_USB.md`).

## Known-good verification at this state
- Sandbox: ctest **4/4**, pytest **158 passed** (after the undock drain test).
- Canonical build/run block: `docs/COMMANDS.md` (starts with `cls`, full
  cmake configure + build; plain run = D3D12 + Lanczos + `--msaa 8` as used
  in the field confirmations).
- Field markers: `preview renderer: Direct3D 12`, `D3D12 MSAA active: x8`,
  presented ≈ decoded (≈55–60 fps), `stash_drops 0`, `audio_cleared=0`,
  camera published ≈ decoded, `teardown timing: total ~2 s`,
  `capture session closed; the iPhone was released`.

## Rollback plan (to this exact state)
1. Close WirePlay and any engine/python leftovers (see COMMANDS.md,
   "If a folder will not delete" — the Job Object should make this a no-op).
2. Replace `C:\Users\gabri\wireplay` with the Milestone 2 backup copy.
3. Run the canonical block from `docs/COMMANDS.md` (configure + build +
   tests + camera registration + run). Expected: ctest 4/4, pytest 158,
   `Direct3D 12` in the log, numbers as above.
4. If the folder will not replace, a process is holding it — same section
   of COMMANDS.md. Do not "fix" code during a rollback; restore first,
   diagnose after.

No database, registry state (beyond the camera DLL registration, redone by
the canonical block), or machine state is part of the milestone: the
project folder plus the documented build is the whole state. The libusb0
UpperFilter driver install (docs/BUILDING.md) is machine state that
Milestone 2 assumes, as before.
