# Research: where the 60 fps live (2026-10-20)

Question: the QuickTime-USB session delivers a jittery ~45 fps mean
(bimodal 16.7/33.3 ms gaps) while the same phone can clearly do a
steady 60. Where is the difference decided, and what do we build?

## QuickTime (Valeria) session — no cadence negotiation exists

- Audit against the reference implementation
  (danielpaulus/quicktime_video_hack, Go): no fps/framerate field
  anywhere in the handshake; message set and clock arithmetic match
  ours (NEED-per-FEED, CVRP/CWPA clock replies, HPD/HPA). SPRP/TBAS/
  SRAT are log-only properties in the reference, not gates.
- Independent corroboration that this capture pipeline is variable
  frame rate by design: QuickTime screen capture reports of
  "nominally 60 fps, VFR, min 20 / max 60 / average ~47.7" match our
  measured 43-55 fps windows almost exactly.
- Our host side is not the throttle: `[usb] read_wait=99%`,
  `pump_busy=0.2 ms/feed`. Async/multiple-URB libusb reads cannot
  improve an arrival the host is already waiting on.

Conclusion: the ~45 average is the device encoder policy of the
QuickTime session. It cannot be configured away inside this protocol.

## AirPlay — the receiver advertises the cadence

- In the AirPlay `/info` response the RECEIVER declares its display:
  `displays[]` entries carry `widthPixels/heightPixels`, `refreshRate`
  and `maxFPS`. The sender encodes to match.
- UxPlay (GPL-3.0 reference receiver) exposes exactly this:
  `-fps n` "sets a maximum frame rate for the AirPlay client to
  stream video" (DEFAULT 30), and `-s wxh@r` informs the client of
  the display resolution/refresh (default 1920x1080@60).
  RPiPlay PR #191 confirms maxFPS is honoured by the client:
  it "tells the AirPlay client not to send video at more than
  maxFPS fps"; a missing maxFPS defaulted to half the refresh rate.
- So a 60 fps (or 120 on ProMotion phones) AirPlay stream is a
  *negotiated property*, requested by the receiver: `-fps 120 -s
  <native>@120`. HEVC mirroring also exists on this path (UxPlay
  `-h265`), which the QuickTime session can never offer.

This is the optimised path to real, full-rate frames:
receiver advertises native resolution @ full refresh -> phone
encodes at that cadence -> we present each real frame once.

## USB transport for AirPlay

- 3u proves AirPlay-over-USB exists product-wise (Control Center ->
  Screen Mirroring -> "3uAirPlayer" over the cable; no USB network
  adapter appears on the PC).
- usbmux multiplexes TCP-like connections over the cable
  (host 127.0.0.1:27015 on Windows); the iPhoneMirror/iUsbBridge
  ecosystem demonstrates wired AirPlay control through such a bridge
  (non-commercial licence: reference only, not code we may ship).
- Remaining unknowns (kill order unchanged): whether iOS exposes
  AirPlay services to the cable without Wi-Fi discovery, and legacy
  mirroring compatibility on iOS 27.2. The Wi-Fi UxPlay test on the
  user's own phone answers the second; a usbmux port census during
  a 3u USB session answers the first.

## Bonus finding: PTS-scheduled presentation (implemented)

The phone stamps every frame with its capture time (PTS); we used
to present on arrival, so encoder pauses followed by bursts were
painted on screen as bursts. The receiver-side fix that invents
nothing: present each real frame at its stream time (anchor first
PTS to wall clock; late frames present immediately; holds capped).
This is UxPlay's default `-vsync` (timestamp-based) behaviour, and
the user's own field model of the defect. No duplicated frames —
the permanent rule stands (docs/AIRPLAY_USB.md).

---

## RESOLVED (2026-10-22): the cadence was in the clock replies

Field-proven: **60 fps locked, zero jitter**, after fixing two SYNC
replies in `_handle_sync` (`engine/quicktime_backend.py`):

- **TIME**: `pack_cm_time(..., flags=1)` (kCMTimeFlagsHasBeenRounded,
  what the reference sends on the wire). We sent flags=0 -- a host
  clock CoreMedia treats as **invalid**.
- **SKEW**: the measured host/device audio-clock rate (~48000 on a
  healthy session, `_clock_skew()`), ported from the reference's
  `CalculateSkew`. We sent `0.0` -- "the host clock does not run".

The device schedules frame delivery against the host clock described
by these replies; told the clock was invalid and stopped, it
delivered ~45 fps bimodal with drift-like jitter. The "VFR by device
policy / no cadence negotiation in the protocol" conclusion earlier
in this document was therefore **wrong**: there is no explicit fps
field, but the clock sync *is* the cadence mechanism. The `--no-audio`
A/B also fits: the replies are sent whether or not audio is played.
Pinned by regression tests (`test_clock_skew_*`,
`test_time_reply_sends_valid_cm_time_flags`). The presentation-side
work (PTS-scheduled present-once) remains correct hygiene, but the
win came from the handshake. AirPlay is no longer needed for 60 on
this device class; it stays the long-term path only for >60 Hz
(ProMotion 120 Hz) ambitions.
