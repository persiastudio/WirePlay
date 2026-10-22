# AirPlay-over-USB investigation (primary direction, 2026-10-20)

Decision (user): AirPlay-over-USB becomes the primary transport goal, with
the QuickTime USB backend kept as the working fallback. Motivation: the
QuickTime capture stream oscillates ~43-54 fps (peaks to 60) with jitter,
while 3uAirPlayer (paid) sustains a constant 60 on the same hardware.

## What is actually known

- 3uAirPlayer requires Apple's Bonjour service even for its wireless mode,
  and its USB mode is proprietary -- from outside it is NOT possible to
  tell whether it is AirPlay protocol over a USB link or the QuickTime
  capture protocol with a differently driven encoder.
- UxPlay (FDH2/UxPlay, GPLv3) is a mature open-source AirPlay mirroring
  *receiver* (legacy mirroring protocol; H.264 + AAC/ALAC). GPLv3 fits
  our architecture: it would run as/behind the GPL engine process, never
  linked into the MIT app. It builds for Windows (MSYS2/MinGW).
- The RayrenSX/iPhoneMirror ecosystem demonstrates wired mirroring over
  USBMux on Windows, using a reverse bridge (iUsbBridge, non-commercial
  licence -- reference only, not a dependency) so the device can reach
  host services through the mux tunnel.
- AirPlay discovery is mDNS (Bonjour). Over a cable, the receiver must
  become discoverable/reachable via either (a) a USB network interface
  (link-local networking over the cable), or (b) a reverse usbmux tunnel
  initiated with the device's cooperation. Which of these iOS 27.2
  accepts for mirroring is unknown.

## Unknowns to kill, in order

1. When 3uAirPlayer does "USB Mirroring", does a USB network adapter
   appear on the PC (Device Manager / Get-NetAdapter), and does the
   phone show a link-local IPv6/IPv4 address? -> tells us (a) vs (b).
2. Does the iPhone expose anything AirPlay-related over usbmux/lockdown
   service lists? (pymobiledevice3 lockdown services dump.)
3. Does an unmodified UxPlay (Windows build) complete mirroring with
   this iPhone 11 Pro Max on iOS 27.2 over ordinary Wi-Fi? -> proves or
   kills the legacy-protocol assumption before any USB work.
4. If (3) works: can the phone be made to reach the receiver over the
   cable (USB network or reverse tunnel) with pairing intact?

## Hard constraints (unchanged)

- Free and distributable; GPL components stay in the engine process.
- No Developer Mode. No Zadig-style driver replacement.
- QuickTime backend remains the fallback and the shipping path until an
  AirPlay path is field-proven end to end on the user's own iPhone.

## Status

- [x] Experiment 1 (2026-10-20): during a 3u USB session the PC shows NO
  USB network adapter (only Ethernet + VMware virtuals): the link is
  pure USB with no visible networking. User's firsthand correction: 3u
  mirroring is started from the phone's Control Center -> Screen
  Mirroring -> selecting "3uAirPlayer" (shown with an Apple TV logo),
  i.e. 3u really is an AirPlay *receiver* even in its USB mode -- so
  AirPlay reachability over the cable exists somehow inside Apple's
  USB stack (no Windows-visible adapter), most plausibly via usbmux
  port plumbing. (The "3u sustains 60 fps" memory predates 3u's
  paywall; the free tier now caps at 25 fps / 480p.)
- QuickTime-side cadence work (competing hypothesis, in progress):
  NEED is now re-sent immediately on FEED recognition instead of after
  the full sample-buffer parse + forward (missed capture slots match
  the bimodal 16.7/33.3 ms gap distribution behind the ~45 fps mean).
- [ ] Experiment 2 (lockdown/usbmux service census)
- [ ] Experiment 3 (UxPlay Wi-Fi mirroring proof on iOS 27.2)
- [ ] Experiment 4 (USB reachability for the receiver)

## QuickTime cadence audit (2026-10-20)

The QuickTime protocol was audited against the reference Go
implementation (danielpaulus/quicktime_video_hack) looking for any
encoder-cadence negotiation: there is none. No fps/framerate field
exists anywhere in the handshake; the message set and clock arithmetic
match ours, and SPRP/TBAS/SRAT are log-only properties in the
reference, not gates. Combined with our own measurements (USB read
99% idle-wait, 0.2 ms/FEED host work), the ~45 fps bimodal arrival is
the device-side encoder policy of the QuickTime ("Valeria") session,
which was designed for screen recording, not live mirroring. AirPlay
differs exactly here: the receiver declares its display (resolution
and refresh) and the phone encodes to match. This audit is why the
cadence fight moves to AirPlay instead of more QuickTime tuning.

## Permanent presentation rule (user directive, 2026-10-20)

No artificial frames, ever. A frame-pacing experiment that
re-presented the held frame on the display cadence was field-rejected:
duplicated frames are fake frames, and a 45 Hz source duplicated to a
high refresh cadence judders worse than honest 45. Presentation is
strictly arrival-driven: every decoded frame presented exactly once.
Smoothness must be earned at the source (session cadence), never
painted on at the sink.

## CONCLUSION (2026-10-22) — AirPlay-over-USB discovery BLOCKED on Windows; spike closed

Field facts (user-corrected):
- 3uTools "USB AirPlay" is NOT phone-discovered AirPlay: with Wi-Fi off, nothing appears in
  Screen Mirroring (neither 3u nor UxPlay). Its USB mode is a PC-side "start casting" button:
  host-initiated USB capture via libusb filter driver + legacy Apple Mobile Device USB driver.
  Same family as our QuickTime backend.
- CoreDevice tunnel over pure USB works WITHOUT Developer Mode (pymobiledevice3
  `lockdown start-tunnel`); unicast unicast to device ::1 @2 ms. Media would flow.
- But mDNS does NOT cross that tunnel: 30 s pktmon capture with Screen Mirroring open =
  zero UDP-5353 packets on any interface; device-originated Bonjour browse empty.
- Unicast mDNS advertisement injection to the device tunnel IP (ephemeral and 5353 source
  port variants) was ignored by iOS.
- System Bonjour + Apple Mobile Device Service (usbmux 127.0.0.1:27015) running does not
  surface a host _airplay registration to a cable-only phone (user field observation).
- BLE discovery rejected by user (no dongle, no radios). Hotspot/Wi-Fi rejected as transport.

Verdict: no viable pure-USB discovery channel for phone-initiated AirPlay on this PC.
QuickTime-USB remains the shipping engine; proceed to UI app phase.
