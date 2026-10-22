# De-Pythonization plan (registered 2026-10-22, Milestone 2 baseline)

**User decisions (2026-10-22):**
1. **USB stays in Python.** The pyusb/libusb-win32 transport (discovery,
   docking control transfers, bulk pump, disable/teardown) is field-proven
   and fragile to touch ("se mexer ali vai feder"). It does not move.
2. **Everything else ports to C++**, in small field-validated steps, with the
   previous state always runnable as fallback.
3. **No merging the engine into the app executable.** The engine stays a
   separate process (crash isolation, Job Object lifecycle, GPL boundary).
4. **Unify whatever can be unified without negative impact.** Goal: extreme
   optimization and shortening of paths / inter-language translations —
   one implementation per concept, no duplicated logic across languages.

## What this means concretely

### Stays Python (the "USB island")
- `engine/quicktime_backend.py` — device discovery, docking sequence,
  ASYN/SYNC pump, clock replies, teardown. Depends only on **pyusb** (+
  stdlib). This is the entire Python footprint's reason to exist.
- A thin launcher around it (today's `_run_quicktime`) — argparse, the
  frame queue, the pipe Sink — until/unless its pieces move (see below).

### Dies immediately (Step 1)
- The whole **CoreDevice path** (Session/Tunnel/RTP depacketizer/tunneld):
  rejected product direction (Developer Mode is forbidden), yet it forces
  **pymobiledevice3 and its dozens of transitive libs** (aioquic, websockets,
  cryptography, construct, …) into every engine start, including the
  multi-hundred-ms import cost. Removing it is pure win: fewer libs,
  faster start, `--backend auto` becomes QuickTime (the only backend).
  `test_depacketizer.py` goes with it (it tested only that path).

### Ports next (in order, each independently shippable)
- **Step 2 — usbmux preflight to the app (C++).** Today the engine checks
  127.0.0.1:27015 and tries to start the Apple Mobile Device Service before
  the transport opens. That is app-lifecycle work, not USB work: Winsock +
  SCM in `run()` before launching the engine shortens the failure path and
  lets the engine assume its precondition. Engine keeps a loud fatal if
  the port is dead (defense in depth).
- **Step 3 — pipe framing is already one protocol, one owner each side.**
  No change expected; audit for duplicated constants between
  `wp/protocol.hpp` and the engine header block and single-source them in
  docs (`docs/WIRE_PROTOCOL.md` is normative).
- **Step 4 — evaluate the launcher pieces** (frame queue policy, stats):
  these sit on the media path between USB and the app. They stay Python
  while they are glued to the pump; if a future step moves message
  *framing* (not transport) to C++, it happens only with field A/B proof
  that latency did not move. Default: leave them.

### Explicitly NOT doing
- No libusb-1.0 switch (Windows backend cannot talk to libusb0.sys).
- No C++ USB rewrite (user decision 1).
- No engine/app process merge (user decision 3).
- No change to the docking golden sequence or close_session semantics in
  any of these steps; transport bytes stay byte-identical.

## Verification discipline (every step)
- Sandbox: Linux build, ctest, pytest (counts may drop when tests for
  deleted code are deleted — recorded in STATUS when it happens).
- Field: same health markers (decoding 0 errors, presented≈decoded,
  55–60 fps, `audio_cleared=0`, camera publishing, teardown ≈2 s).
- Rollback: Milestone 2 snapshot (see `docs/MILESTONE_2.md`); the user
  also keeps an external backup of this exact state.

## Completion (2026-10-22)

- **Step 1 — DONE, field-validated.** CoreDevice path and the whole
  pymobiledevice3 dependency tree removed; engine 1531 -> ~640 lines,
  imports pyusb + stdlib only. Field run: 6874/6874 presented, teardown
  2.04 s.
- **Step 2 — DONE (code), field validation = one normal run.** The app
  performs the usbmux preflight (Winsock connect to 127.0.0.1:27015,
  SCM start/restart of the Apple Mobile Device Service, 15 s poll)
  before launching the engine and fails loudly in place. The engine
  keeps its own preflight as a fatal-5 defense for standalone runs.
- **Step 3 — DONE (audit).** Constants are inherently duplicated across
  the process/language boundary, mechanically pinned by contract tests;
  `docs/WIRE_PROTOCOL.md` carries the audit note. No codegen.
- **Step 4 — CLOSED, leave as is.** Per this plan's own gate, launcher
  pieces (frame queue, Sink, stats) move only with field A/B latency
  proof. The 2026-10-22 NEED/pipe measurements attribute no latency to
  them (feed-busy max 1.36 ms, pipe never blocks in steady state), so
  there is nothing to prove against. They stay Python, glued to the
  pump, as the plan's default says.

Python's remaining footprint is exactly the sanctioned USB island:
interpreter + pyusb + `quicktime_backend.py` + the thin launcher around
it. De-Pythonization is finished; anything further would mean touching
the USB transport, which user decision 1 forbids.
