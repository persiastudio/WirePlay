"""Opt-in arrival tracing for the QuickTime path (WIREPLAY_TRACE=1).

Averages hide the thing being chased: a stall of 100-200 ms every few frames
barely moves a mean fps figure. This module answers three questions from one
run, with no change to behaviour when it is off:

  * does the phone itself go quiet?   -> "usb gap" lines (device -> host)
  * how often, and how big, are IDRs? -> "IDR" lines and the window summary
  * is the pipe to the app blocking?  -> "pipe write" lines

If the USB gaps repeat with the same period as the IDRs, the stall is the
encoder emitting a keyframe. If USB arrival is smooth but pipe writes block,
the app side is not draining. If both are smooth, the stall is downstream of
the engine and the app's own `[wireplay] loop:` line says where.

Kept free of any heavy import so it can be unit-tested without a phone.
"""
from __future__ import annotations

import os
from typing import Callable, Optional

GAP_MS = 28.0          # about two frame intervals at 60 fps
PIPE_MS = 15.0         # a write that long is the reader not keeping up
MAX_IDR_LINES = 30     # then only the window summary, to keep logs readable


def enabled(environ=None) -> bool:
    env = os.environ if environ is None else environ
    return env.get("WIREPLAY_TRACE", "") not in ("", "0")


class ArrivalTracer:
    def __init__(self, log: Callable[[str], None], gap_ms: float = GAP_MS,
                 pipe_ms: float = PIPE_MS) -> None:
        self._log = log
        self._gap_ms = gap_ms
        self._pipe_ms = pipe_ms
        self._prev: Optional[float] = None
        self._prev_gap: Optional[float] = None
        self._prev_idr: Optional[float] = None
        self._idr_lines = 0
        self._since_idr = 0
        self.reset_window()

    def reset_window(self) -> None:
        self._w_frames = 0
        self._w_gaps = 0
        self._w_worst_gap_ms = 0.0
        self._w_idr = 0
        self._w_idr_bytes = 0
        self._w_p_bytes = 0
        self._w_p = 0
        self._w_idr_intervals_ms = []

    def video(self, now: float, size: int, is_key: bool, queue_depth: int) -> None:
        """Call once per video access unit, at USB arrival time."""
        self._w_frames += 1
        self._since_idr += 1
        if self._prev is not None:
            gap_ms = (now - self._prev) * 1000.0
            if gap_ms > self._w_worst_gap_ms:
                self._w_worst_gap_ms = gap_ms
            if gap_ms > self._gap_ms:
                self._w_gaps += 1
                since = ((now - self._prev_gap) * 1000.0
                         if self._prev_gap is not None else float("nan"))
                self._prev_gap = now
                self._log(
                    "[trace] usb gap %.1f ms (%.0f ms after the previous gap) "
                    "this AU: %d bytes, key=%d, engine queue=%d"
                    % (gap_ms, since, size, int(is_key), queue_depth))
        self._prev = now
        if is_key:
            interval_ms = ((now - self._prev_idr) * 1000.0
                           if self._prev_idr is not None else float("nan"))
            self._prev_idr = now
            self._w_idr += 1
            self._w_idr_bytes += size
            if interval_ms == interval_ms:  # not NaN
                self._w_idr_intervals_ms.append(interval_ms)
            if self._idr_lines < MAX_IDR_LINES:
                self._idr_lines += 1
                self._log("[trace] IDR %d bytes, %.0f ms and %d AUs after the "
                          "previous IDR" % (size, interval_ms, self._since_idr))
            self._since_idr = 0
        else:
            self._w_p += 1
            self._w_p_bytes += size

    def pipe_write(self, seconds: float, mtype: int, nbytes: int,
                   queue_depth: int) -> None:
        """Call around the blocking write of one frame to the app."""
        if seconds * 1000.0 > self._pipe_ms:
            self._log("[trace] pipe write blocked %.1f ms (type 0x%02x, %d bytes, "
                      "engine queue=%d)" % (seconds * 1000.0, mtype, nbytes,
                                            queue_depth))

    def summary(self) -> str:
        """One line for the window since the last call; resets the window."""
        iv = self._w_idr_intervals_ms
        line = (
            "[trace] window: %d AUs, %d usb gaps >%.0f ms (worst %.0f ms), "
            "%d IDRs avg %.0f KB%s, non-IDR avg %.1f KB"
            % (self._w_frames, self._w_gaps, self._gap_ms, self._w_worst_gap_ms,
               self._w_idr,
               (self._w_idr_bytes / self._w_idr / 1024.0) if self._w_idr else 0.0,
               (" every %.0f ms" % (sum(iv) / len(iv))) if iv else "",
               (self._w_p_bytes / self._w_p / 1024.0) if self._w_p else 0.0))
        self.reset_window()
        return line
