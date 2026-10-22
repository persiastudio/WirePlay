#!/usr/bin/env python3
"""WirePlay engine — iPhone → PC screen + audio over USB.

This process is the GPL-3.0 half of WirePlay. Since Milestone 2 it owns
exactly one thing: the QuickTime USB transport (see
``quicktime_backend.py``) plus the thin launcher around it. The CoreDevice
path was removed with the de-Pythonization plan
(``docs/DEPYTHONIZATION.md``): it needed Developer Mode, which the product
forbids, and it dragged pymobiledevice3's dependency tree into every
start. This file now needs pyusb and the standard library only.

It speaks the framed protocol in ``docs/WIRE_PROTOCOL.md`` on stdout and
reads commands from stdin. Run it as a child process, never as a
library — see ``docs/LEGAL.md``.

Logs go to stderr. stdout is binary frames only.
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import collections
import contextlib
import json
import logging
import queue
import struct
import sys
import threading
import time
from typing import Any, Optional

# ---------------------------------------------------------------------------
# Protocol constants — must match docs/WIRE_PROTOCOL.md and src/wp/protocol.hpp
# ---------------------------------------------------------------------------
T_ENGINE_STATUS = 0x01
T_ENGINE_FATAL = 0x02
T_ENGINE_STATS = 0x04
T_ENGINE_DEVICE = 0x08
T_ENGINE_VIDEO_INIT = 0x10
T_ENGINE_VIDEO_AU = 0x11
T_ENGINE_AUDIO_INIT = 0x20
T_ENGINE_AUDIO_AU = 0x21
T_ENGINE_AUDIO_PCM_INIT = 0x22
T_ENGINE_PING = 0xFE
T_ENGINE_END = 0xFF

# Video codec identifiers, fourth field of the kVideoInit header. Wire
# constants: src/wp/protocol.hpp has the matching C++ enum and
# engine/tests/test_wire_protocol.py pins the values on both sides.
CODEC_HEVC = 0
CODEC_H264 = 1

T_APP_PONG = 0x81
T_APP_REQUEST_KEY = 0x82
T_APP_ROTATE = 0x83
T_APP_RESTART_STREAM = 0x84
T_APP_TOUCH = 0x85
T_APP_BUTTON = 0x86
T_APP_SET_BITRATE = 0x87
T_APP_PING = 0xFE
T_APP_END = 0xFF

MAX_FRAME = 64 * 1024 * 1024

# HEVC NAL unit types (H.265 §7.4.2.2, nal_unit_type = (byte0 >> 1) & 0x3F)
NAL_IDR_W_RADL = 19
NAL_IDR_N_LP = 20
NAL_CRA = 21
NAL_VPS, NAL_SPS, NAL_PPS = 32, 33, 34
NAL_AP = 48  # Aggregation Packet (RFC 7798 §4.4.2)
NAL_FU = 49  # Fragmentation Unit (RFC 7798 §4.4.3)
KEY_NAL_TYPES = frozenset({NAL_IDR_W_RADL, NAL_IDR_N_LP, NAL_CRA})

# Apple's DisplayService appends a fixed 14-byte proprietary trailer after the
# final fragment of each coded-slice NAL. Apple's own receiver (avconferenced)
# strips it before decode; it is not part of the bitstream and some decoders
# choke on the trailing bytes, so we strip it too. Matched as an exact suffix,
# so it is a no-op on streams that do not carry it.
DISPLAYSERVICE_NAL_TRAILER = bytes.fromhex("04f00ac0000003000004ec0ab003")

VIDEO_CLOCK = 24000  # Hz — RTP media clock for the video stream

logger = logging.getLogger("wireplay.engine")


# ---------------------------------------------------------------------------
# Live frame queue (engine -> app pipe)
# ---------------------------------------------------------------------------
class LiveFrameQueue:
    """Bounded queue between the USB pump thread and the pipe writer.

    Two rules, both learned from field failures:

    * Audio, init and status frames are never dropped. A shallow shared
      queue once discarded the audio init frame under load and the whole
      session ran silent ("audio off") -- worse than any jitter.
    * Video is dropped only as whole access units, and only in drop
      mode: once a P-frame is lost the prediction chain is garbage until
      the next keyframe anyway, so drop mode discards everything up to
      the next keyframe (the app flushes its decoder on keyframes, so
      the chain restarts clean). Dropping isolated AUs at random turned
      into visible freezing, not freshness.

    Priority frames that arrive to a full queue evict the oldest queued
    video AU (which also arms drop mode: that chain is now broken).
    """

    def __init__(self, maxsize: int = 8) -> None:
        # Two lanes. Video is capped and expendable; priority frames
        # (audio/init/status) travel in their own lane and never evict
        # video. They used to share one capped deque, so under video
        # backlog every audio AU evicted the oldest queued video AU --
        # and every eviction armed drop mode, which discarded video up
        # to the next keyframe. One shared queue made a video hiccup
        # garble the audio and an audio burst freeze the video.
        self._items = collections.deque()
        self._prio = collections.deque()
        self._maxsize = maxsize
        self._cond = threading.Condition()
        self._dropping = False
        self.drops = 0

    def __len__(self) -> int:
        with self._cond:
            return len(self._items) + len(self._prio)

    def _evict_oldest_video(self) -> bool:
        for i, (mtype, _payload) in enumerate(self._items):
            if mtype == T_ENGINE_VIDEO_AU:
                del self._items[i]
                self.drops += 1
                self._dropping = True  # that chain is broken now
                return True
        return False

    def put_priority(self, mtype: int, payload: bytes) -> None:
        """Audio/init/status: own lane, never dropped, never evicts."""
        with self._cond:
            self._prio.append((mtype, payload))
            self._cond.notify()

    def put_video(self, payload: bytes, is_key: bool) -> None:
        with self._cond:
            if self._dropping:
                if not is_key:
                    self.drops += 1
                    return
                self._dropping = False  # keyframe: clean restart, queue it
            if len(self._items) >= self._maxsize:
                if is_key and self._evict_oldest_video():
                    # A keyframe is worth displacing one stale AU for:
                    # everything before it is about to be flushed anyway.
                    self._items.append((T_ENGINE_VIDEO_AU, payload))
                    self._dropping = False
                    self._cond.notify()
                    return
                self._dropping = True
                self.drops += 1
                return
            self._items.append((T_ENGINE_VIDEO_AU, payload))
            self._cond.notify()

    def get(self):
        with self._cond:
            while not self._items and not self._prio:
                self._cond.wait()
            if self._prio:
                return self._prio.popleft()
            return self._items.popleft()


# ---------------------------------------------------------------------------
# Framed stdout sink
# ---------------------------------------------------------------------------
class Sink:
    """Length-prefixed binary frames on stdout.

    Every write is a single ``sys.stdout.buffer.write`` so a frame is never
    interleaved with another coroutine's frame: asyncio is single-threaded and
    ``write`` does not await, so this is atomic for our purposes.

    ``tee`` mirrors every frame to a file, which is what makes the whole
    pipeline testable without an iPhone attached: record once on a real device,
    then replay the file into the app with ``--offline``.
    """

    def __init__(self, tee_path: Optional[str] = None, out=None) -> None:
        self._out = out if out is not None else sys.stdout.buffer
        self.bytes_written = 0
        self._tee = None
        if tee_path:
            self._tee = open(tee_path, "wb", buffering=0)
            logger.info("teeing frames to %s", tee_path)

    def send(self, mtype: int, payload: bytes = b"") -> None:
        length = len(payload) + 1
        if length > MAX_FRAME:
            raise ValueError(f"frame too large: {length}")
        frame = struct.pack(">IB", length, mtype) + payload
        self._out.write(frame)
        # Flush every frame. stdout to a pipe is block-buffered, so without
        # this a small frame -- a 10 ms PCM audio AU is ~2 KB -- sits in the
        # buffer until enough bytes pile up and then arrives in a burst. On
        # screen that read as frame pacing judder; in the speakers it was
        # crackle with sub-200 ms dropouts. A flush per frame at these rates
        # is cheap; late audio is not.
        self._out.flush()
        if self._tee is not None:
            self._tee.write(frame)
        self.bytes_written += length + 4

    def close(self) -> None:
        if self._tee is not None:
            with contextlib.suppress(Exception):
                self._tee.close()
            self._tee = None

    def json(self, mtype: int, obj: Any) -> None:
        self.send(mtype, json.dumps(obj, separators=(",", ":")).encode())

    def status(self, state: str, detail: str = "") -> None:
        self.json(T_ENGINE_STATUS, {"state": state, "detail": detail})
        logger.info("[%s] %s", state, detail or "-")

    def fatal(self, code: int, message: str) -> None:
        logger.error("fatal(%d): %s", code, message)
        self.json(T_ENGINE_FATAL, {"code": code, "message": message})
        self.flush()

    def flush(self) -> None:
        with contextlib.suppress(Exception):
            self._out.flush()


# ---------------------------------------------------------------------------
# usbmux preflight (the app also checks before launching us; this stays as
# a loud-fatal defense for anyone who runs the engine standalone)
# ---------------------------------------------------------------------------
USBMUX_ADDRESS = ("127.0.0.1", 27015)
# Names the Apple Mobile Device Service has shipped under. The service is
# what provides usbmuxd on Windows; Apple has renamed the bundle around it
# (iTunes -> Apple Devices) more than once, so try them all.
_APPLE_SERVICE_NAMES = ("Apple Mobile Device Service", "AppleMobileDeviceService")


def _usbmux_reachable(timeout: float = 2.0) -> bool:
    """True when something answers on the usbmuxd port (Windows: TCP 27015)."""
    try:
        with socket.create_connection(USBMUX_ADDRESS, timeout=timeout):
            return True
    except OSError:
        return False


def _try_start_apple_service() -> None:
    """Best-effort service start/restart. Needs elevation to actually work;
    when it cannot, the caller's error message says what to do by hand."""
    for name in _APPLE_SERVICE_NAMES:
        with contextlib.suppress(Exception):
            r = subprocess.run(["sc.exe", "query", name],
                               capture_output=True, timeout=10)
            if r.returncode != 0:
                continue  # no service by this name on this machine
            if b"RUNNING" not in (r.stdout or b""):
                subprocess.run(["sc.exe", "start", name],
                               capture_output=True, timeout=20)
            else:
                # Running but not answering the socket: it is wedged. A
                # restart is the documented un-wedge; it will simply fail
                # without elevation, which the caller reports honestly.
                subprocess.run(["sc.exe", "stop", name],
                               capture_output=True, timeout=20)
                subprocess.run(["sc.exe", "start", name],
                               capture_output=True, timeout=20)
            return


def _ensure_usbmux(status, wait_seconds: float = 15.0) -> bool:
    """Make sure Apple's usbmux service is up before any QuickTime attempt.

    Proven on the target machine the hard way: with usbmuxd down, the phone
    exposes the capture configuration and then never starts the session --
    no ping, no error, in every open-sequence variant we probed. With it up,
    the same sequence streams immediately. So this is a hard prerequisite,
    checked loudly instead of being rediscovered as "the stream just hangs".

    Windows-only: elsewhere usbmuxd is a system daemon with a different
    socket and the app has no business managing it.
    """
    if sys.platform != "win32":
        return True
    if _usbmux_reachable():
        return True
    status("usbmux (Apple Mobile Device Service) is not responding; "
           "trying to start it")
    _try_start_apple_service()
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        if _usbmux_reachable(timeout=1.0):
            status("usbmux is responding now")
            return True
        time.sleep(0.5)
    return False


def _run_quicktime(args: argparse.Namespace) -> int:
    """Capture over the QuickTime/Valeria USB protocol.

    The path that needs no Developer Mode and no DeveloperDiskImage. See
    engine/quicktime_backend.py for the wire format.

    Audio is 48 kHz stereo s16 PCM straight off the device. It goes out under
    T_ENGINE_AUDIO_PCM_INIT (not the AAC init record, whose payload is an
    AudioSpecificConfig) and then as ordinary audio AUs; the app queues the
    samples to SDL untouched. Announcing an audio device is also what makes
    the phone route its sound to the cable at all, so with no consumer wired
    up the phone went silent and nothing played -- which is why on_audio is
    not optional here.
    """
    from quicktime_backend import QuickTimeTransport, QuickTimeUnavailable
    import wp_trace
    import usb.core

    sink = Sink(tee_path=args.dump)
    # Created before the callbacks below so they can stop feeding the pipe
    # the instant the app says goodbye. When the window closes, the app
    # stops reading our stdout; a frame write into a full pipe blocks
    # forever, which used to park the pump thread inside Sink.send so the
    # stop signal was never acted on and close_session() never ran -- the
    # phone stayed in its dock/casting state until the cable was pulled.
    stop_requested = threading.Event()

    # Frame writes go through a bounded queue and a dedicated writer
    # thread. Checking the stop flag in the callbacks (the previous fix)
    # was not enough: a callback already inside Sink.send when the app
    # closed stayed blocked on the full, unread pipe, and the pump thread
    # never reached close_session(). Now the pump thread only ever does a
    # non-blocking put; if the app stops reading, frames drop here instead
    # of parking the thread that has to release the phone.
    # Depth 12, shallow on purpose, and drop-aware: see LiveFrameQueue.
    # (Started at 6; that broke prediction chains often enough to show
    # as ~1 s colour glitches after every hiccup. Steady-state depth in
    # the field is 1-4, so the extra room costs no latency in regime --
    # it only absorbs stalls instead of dropping access units.)
    # A 60-deep queue here field-measured as 500-750 ms of latency; a
    # naive 6-deep shared queue dropped the audio init and random P-AUs
    # and ran a whole session silent and freezing. The queue class owns
    # the policy; this code just feeds it.
    _frame_queue = LiveFrameQueue(maxsize=16)

    def _enqueue(mtype, payload):
        # Priority frames (audio, init, status): never dropped.
        if stop_requested.is_set():
            return
        _frame_queue.put_priority(mtype, payload)

    def _enqueue_video(payload, is_key):
        if stop_requested.is_set():
            return
        _frame_queue.put_video(payload, is_key)

    # WIREPLAY_TRACE=1: log every USB arrival gap, the IDR cadence and any
    # blocked pipe write (see wp_trace.py). Off by default, zero cost.
    _tracer = (wp_trace.ArrivalTracer(lambda m: logger.info("%s", m))
               if wp_trace.enabled() else None)

    def _frame_writer():
        while True:
            mtype, payload = _frame_queue.get()
            try:
                if _tracer is not None:
                    _w0 = time.monotonic()
                    sink.send(mtype, payload)
                    _tracer.pipe_write(time.monotonic() - _w0, mtype,
                                       len(payload), len(_frame_queue))
                else:
                    sink.send(mtype, payload)
            except Exception:
                return

    threading.Thread(target=_frame_writer, daemon=True).start()

    def _transport_log(m):
        # stderr first: after kAppEnd the app stops reading frames off the
        # pipe, so pipe-only logging makes teardown (and in particular the
        # "iPhone released" line) invisible right when it matters most.
        logger.info("[quicktime] %s", m)
        if stop_requested.is_set():
            return  # nobody is draining the pipe any more; do not block on it
        try:
            sink.status("quicktime", m)
        except Exception:
            pass

    transport = QuickTimeTransport(
        udid=args.udid,
        logger=_transport_log,
    )
    state = {"init_sent": False, "frames": 0, "rtp_ts": 0, "arrivals": []}

    def on_format(width, height, sps, pps):
        if state["init_sent"]:
            return
        logger.info("video format: %sx%s (sps=%d pps=%d bytes)",
                    width, height, len(sps or b""), len(pps or b""))
        # Geometry stays zero: the app takes it from the decoded frame, which is
        # the authority. The codec field selects the decoder, and this protocol
        # only ever carries H.264.
        _enqueue(T_ENGINE_VIDEO_INIT, struct.pack(">IIII", 0, 0, 0, CODEC_H264))
        state["init_sent"] = True

    def on_video(annexb: bytes, pts):
        if stop_requested.is_set():
            return  # app is gone: drop, never write into an unread pipe
        if not state["init_sent"]:
            on_format(0, 0, None, None)
        is_key = 1 if _au_has_idr(annexb) else 0
        if _tracer is not None:
            _tracer.video(time.monotonic(), len(annexb), bool(is_key),
                          len(_frame_queue))
        # The header timestamp is the DEVICE's presentation time in
        # 90 kHz units -- not a synthetic counter. It used to advance by
        # a fixed 3000 (30 fps at 90 kHz) regardless of the stream,
        # which threw the phone's real capture timing away and left the
        # app's PTS-scheduled presentation with nothing to schedule
        # (field verdict: "changed nothing"). Only when the backend
        # gives us no PTS do we fall back to advancing the last value.
        if pts is not None:
            state["rtp_ts"] = int(pts * 90000) & 0xFFFFFFFF
        _enqueue_video(
            struct.pack(">IBB", state["rtp_ts"] & 0xFFFFFFFF, is_key, 0) + annexb,
            bool(is_key))
        if pts is None:
            state["rtp_ts"] += 1500  # ~60 fps fallback cadence
        state["frames"] += 1
        # Frame-arrival statistics: the jitter report, measured instead of
        # felt. Every 600 frames, log the inter-arrival gap distribution
        # for the window and how many frames the pipe queue dropped. A
        # healthy 60 fps stream has p50 near 16.7 ms and a small p99;
        # periodic judder shows up as a fat tail (or as queue drops).
        state["arrivals"].append(time.monotonic())
        if state["frames"] % 600 == 0:
            logger.info("[progress] %d video frames forwarded", state["frames"])
            if _tracer is not None:
                logger.info("%s", _tracer.summary())
            _enqueue(T_ENGINE_STATUS, json.dumps(
                {"state": "progress",
                 "detail": "%d video frames forwarded" % state["frames"]},
                separators=(",", ":")).encode())
            arr = state["arrivals"]
            if len(arr) > 60:
                gaps = sorted((b - a) * 1000.0 for a, b in zip(arr, arr[1:]))
                n = len(gaps)
                def _pct(q):
                    return gaps[min(n - 1, int(q * n))]
                drops_total = _frame_queue.drops
                drops_window = drops_total - state.get("drops_prev", 0)
                state["drops_prev"] = drops_total
                logger.info(
                    "[stats] window fps=%.1f gap ms p50=%.1f p95=%.1f "
                    "p99=%.1f max=%.1f queue_drops=+%d (%d total) queue_depth=%d",
                    (n / ((arr[-1] - arr[0]) or 1.0)),
                    _pct(0.50), _pct(0.95), _pct(0.99), gaps[-1],
                    drops_window, drops_total, len(_frame_queue))
            state["arrivals"] = arr[-1:]

    audio_state = {"init_sent": False, "ts": 0}

    def on_audio(pcm: bytes):
        if stop_requested.is_set():
            return  # app is gone: drop, never write into an unread pipe
        if args.no_audio or not pcm:
            return
        if not audio_state["init_sent"]:
            _enqueue(T_ENGINE_AUDIO_PCM_INIT, struct.pack(">IB", 48000, 2))
            audio_state["init_sent"] = True
            logger.info("audio format: 48000 Hz 2 ch raw PCM")
        _enqueue(T_ENGINE_AUDIO_AU,
                 struct.pack(">I", audio_state["ts"] & 0xFFFFFFFF) + pcm)
        audio_state["ts"] += len(pcm) // 4  # one s16 stereo frame is 4 bytes

    transport.on_format = on_format
    transport.on_video = on_video
    transport.on_audio = on_audio

    if not _ensure_usbmux(lambda m: sink.status("quicktime", m)):
        sink.fatal(5, "Apple's usbmux service (usbmuxd) is not responding on "
                      "this PC, and the iPhone will not start a mirroring "
                      "session without it. Open the Apple Devices app once "
                      "(or iTunes), or start 'Apple Mobile Device Service' "
                      "from services.msc, then try again. Nothing is wrong "
                      "with the phone or the cable.")
        return 3

    try:
        transport.open()
    except QuickTimeUnavailable as exc:
        sink.fatal(5, str(exc))
        return 3
    except Exception as exc:
        sink.fatal(6, "%s: %s" % (type(exc).__name__, exc))
        return 3

    sink.json(T_ENGINE_DEVICE, {"udid": args.udid or "", "transport": "usb-quicktime"})
    sink.status("streaming", "QuickTime capture running")
    logger.info("QuickTime capture running")

    # The app says goodbye twice over: a kAppEnd frame, then EOF when it
    # closes our stdin (EngineProcess::stop then waits 3 s before killing us).
    # This path used to never read stdin at all, so both signals vanished and
    # the only teardown was the job-object kill -- which cannot run
    # close_session(), leaving the phone presenting as a dock, with its audio
    # routed to the cable, until it was unplugged.
    def _watch_app_stdin() -> None:
        try:
            stream = sys.stdin.buffer
            while not stop_requested.is_set():
                header = stream.read(4)
                if len(header) < 4:
                    break  # EOF: the app is gone
                (length,) = struct.unpack(">I", header)
                if length < 1 or length > MAX_FRAME:
                    break
                body = stream.read(length)
                if len(body) < length:
                    break
                if body[0] == T_APP_END:
                    logger.info("app sent kEnd")
                    break
        except Exception:
            pass
        stop_requested.set()

    threading.Thread(target=_watch_app_stdin, daemon=True).start()
    try:
        while not stop_requested.is_set():
            try:
                transport.pump()
            except usb.core.USBTimeoutError as exc:
                # Idle device; the stream resumes on its own.
                logger.debug("read timeout: %s", exc)
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        sink.fatal(7, "%s: %s" % (type(exc).__name__, exc))
        return 3
    finally:
        stop_requested.set()  # teardown logs go to stderr only from here on
        transport.close_session()
        sink.close()
    return 0


def annexb(nals: list[bytes]) -> bytes:
    """Join raw NAL units into one Annex-B access unit (4-byte start codes)."""
    return b"".join(b"\x00\x00\x00\x01" + nal for nal in nals)


def _au_has_idr(annexb: bytes) -> bool:
    """True when the access unit carries an IDR slice (NAL type 5).

    This decides two load-bearing things -- where the engine's drop
    mode may resume a stream, and when the app flushes its decoder --
    so it must mean a self-sufficient frame. The old test, "first NAL
    is IDR or SPS", was silently wrong: nalus_to_annexb prepends the
    stored SPS to every AU that lacks one, and P-frames lack one, so
    SPS (type 7) led almost every AU and nearly everything was
    labelled keyframe. The engine then resumed broken chains at
    arbitrary P-frames and, once the app read the flag correctly,
    flushed the decoder every frame -- either way, motion turned to
    macroblock soup while the bitstream itself decoded "clean".
    Slice data cannot fake a start code (emulation prevention), so a
    plain scan for type-5 NAL headers is exact enough.
    """
    # C-speed scan for a type-5 NAL header. This runs on the USB pump
    # thread for every access unit, and a byte-at-a-time Python loop
    # over multi-hundred-KB AUs cost 5-9 ms per feed in the field
    # (pump_busy went 0.2 -> 9 ms, read_wait 99% -> 48%): the pump
    # became the pipeline's bottleneck and everything downstream --
    # queue saturation, drop episodes, latency -- was that one loop.
    i = 0
    while True:
        j = annexb.find(b"\x00\x00\x01", i)
        if j < 0:
            return False
        if j + 3 < len(annexb) and (annexb[j + 3] & 0x1F) == 5:
            return True
        i = j + 3


def _first_nal_type(annexb: bytes) -> int:
    i = 0
    while i + 4 <= len(annexb):
        if annexb[i:i + 3] == b"\x00\x00\x01":
            return annexb[i + 3] & 0x1F
        if annexb[i:i + 4] == b"\x00\x00\x00\x01":
            return annexb[i + 4] & 0x1F
        i += 1
    return -1


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog="wireplay-engine",
        description="WirePlay engine: iPhone screen + audio over USB, framed to stdout.",
    )
    p.add_argument("--udid", help="target device UDID (default: first device on USB)")
    p.add_argument("--no-audio", action="store_true", help="skip the audio stream")
    p.add_argument("--dump", metavar="FILE",
                   help="also write every frame to FILE, for replay with "
                        "`wireplay --offline FILE`")
    p.add_argument(
        "--backend",
        choices=("auto", "coredevice", "quicktime"),
        default="auto",
        help="screen-mirroring protocol; only quicktime remains (the "
             "coredevice path was removed: it needs Developer Mode)",
    )
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    logging.basicConfig(
        stream=sys.stderr,
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    if args.backend == "coredevice":
        logger.error(
            "the coredevice backend was removed (it requires Developer "
            "Mode, which WirePlay never enables); use --backend quicktime")
        return 2
    # `auto` used to mean CoreDevice. QuickTime is the only backend now,
    # so auto and quicktime are the same thing.
    return _run_quicktime(args)


if __name__ == "__main__":
    raise SystemExit(main())
