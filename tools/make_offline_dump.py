#!/usr/bin/env python3
"""Build a synthetic frame dump for `wireplay --offline`.

This is what makes the decode + render path testable without an iPhone
attached: it produces the exact byte stream the engine would have written over
stdout, so the C++ app can be run end to end against it.

    python3 tools/make_offline_dump.py --out dump.bin --frames 120

Needs: pip install av
"""

from __future__ import annotations

import argparse
import fractions
import json
import pathlib
import re
import struct
import sys

import av

# Must match docs/WIRE_PROTOCOL.md, engine/wireplay_engine.py and
# src/wp/protocol.hpp. engine/tests/test_wire_protocol.py cross-checks them.
T_STATUS = 0x01
T_DEVICE = 0x08
T_VIDEO_INIT = 0x10
T_VIDEO_AU = 0x11
T_AUDIO_INIT = 0x20
T_AUDIO_AU = 0x21
T_END = 0xFF


def frame(mtype: int, payload: bytes = b"") -> bytes:
    return struct.pack(">IB", len(payload) + 1, mtype) + payload


def annexb_nals(data: bytes) -> list[bytes]:
    """Split Annex-B, recording where each start code *begins*.

    A stream may mix 3- and 4-byte start codes, so the previous NAL ends where
    the next code starts -- not a fixed number of bytes before it.
    """
    codes = [m.start() for m in re.finditer(b"\x00\x00\x01", data)]
    out = []
    for i, start in enumerate(codes):
        begin = start + 3
        end = codes[i + 1] if i + 1 < len(codes) else len(data)
        if end > begin:
            out.append(data[begin:end])
    return out


def gradient_frame(width: int, height: int, step: int) -> av.VideoFrame:
    """A smooth Y gradient plus a moving white bar.

    Deliberately *not* random noise: a gradient encodes to a realistic mix of
    skips and inter blocks, which is what exercises the decoder's reference
    handling the way a real screen stream does.
    """
    # Built via numpy rather than by filling plane buffers by hand: the Y plane
    # is padded to line_size, not width, and writing width*height bytes into it
    # is a size error the first time the width is not a multiple of 64 -- which
    # is every phone resolution there is.
    import numpy as np

    yy, xx = np.mgrid[0:height, 0:width]
    y = (((yy + step * 2) + (xx >> 3)) & 0xFF).astype(np.uint8)
    bar_x = (step * 16) % max(1, width - 80)
    y[:, bar_x : bar_x + 80] = 235  # moving bar => real inter frames

    img = av.VideoFrame(width, height, "yuv420p")
    # Filled plane by plane, honouring line_size: the Y plane is padded to a
    # multiple of the encoder's alignment, not to width, and every phone
    # resolution there is (1290x2796, 1179x2556) is odd in at least one axis.
    for i, plane in enumerate(img.planes):
        rows = height if i == 0 else height // 2
        cols = width if i == 0 else width // 2
        src = y if i == 0 else np.full((rows, cols), 128, dtype=np.uint8)
        padded = b"".join(bytes(src[r]) + bytes(plane.line_size - cols) for r in range(rows))
        plane.update(padded[: plane.buffer_size])
    img.pts = step
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--width", type=int, default=1290)
    ap.add_argument("--height", type=int, default=2796)
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--gop", type=int, default=60)
    ap.add_argument("--codec", choices=("hevc", "h264"), default="hevc",
                    help="which codec to encode, and what kVideoInit declares")
    args = ap.parse_args()

    # libx265 for the CoreDevice path, libx264 for the QuickTime USB path. The
    # app picks its decoder from the codec field in kVideoInit, so the same
    # end-to-end run exercises both.
    enc = av.CodecContext.create("libx264" if args.codec == "h264" else "libx265", "w")
    enc.width, enc.height, enc.pix_fmt = args.width, args.height, "yuv420p"
    enc.time_base = fractions.Fraction(1, args.fps)
    # tune=zerolatency: no B-frames, no reordering -- what the device's
    # real-time encoder produces. Deliberately NO global_header: iOS repeats
    # VPS/SPS/PPS in front of every IDR inside the access unit, and a decoder
    # that never sees them reports "PPS id out of range" and outputs nothing.
    # Reproducing that in-band repetition is the whole point of the dump.
    params = f"log-level=none:keyint={args.gop}:min-keyint={args.gop}:bframes=0"
    enc.thread_count = 0  # 0 = let the encoder use every core it wants
    enc.options = {
        "x265-params" if args.codec == "hevc" else "x264-params": params,
        # ultrafast on purpose: this file only has to be *decodable*, it is
        # never watched. The default preset made generating 300 frames of
        # 1290x2796 the slowest step of the whole test loop for no benefit.
        "preset": "ultrafast",
        "tune": "zerolatency",
    }
    enc.open()

    out = bytearray()
    out += frame(T_STATUS, json.dumps({"state": "streaming", "detail": "synthetic dump"}).encode())
    out += frame(
        T_DEVICE,
        json.dumps({
            "udid": "SYNTHETIC",
            "product": "iPhone15,3",
            "os": "18.5",
            "transport": "usb-userspace",
        }).encode(),
    )

    # kVideoInit carries the parameter sets up front, the way the engine emits
    # them as soon as it has seen them. They are also repeated in-band by the
    # encoder below, so the app works either way.
    # width/height stay 0: the app takes geometry from the decoded frame, which
    # is the authority. The codec field is what selects the decoder.
    out += frame(T_VIDEO_INIT, struct.pack(">IIII", 0, 0, 0, 1 if args.codec == "h264" else 0))

    # The AudioSpecificConfig Apple's DisplayService advertises: AAC-ELD,
    # 48 kHz, stereo, 480 samples per frame.
    out += frame(T_AUDIO_INIT, struct.pack(">IBH", 48000, 2, 4) + bytes([0xF8, 0xE6, 0x40, 0x00]))

    written = 0
    rtp_ts = 0
    ts_step = 24000 // args.fps  # 24 kHz media clock

    def emit(packets):
        nonlocal written, rtp_ts
        for pkt in packets:
            body = bytes(pkt)
            if not body:
                continue
            is_key = 1 if pkt.is_keyframe else 0
            out.extend(frame(T_VIDEO_AU, struct.pack(">IBB", rtp_ts, is_key, 0) + body))
            rtp_ts += ts_step
            written += 1

    for i in range(args.frames):
        emit(enc.encode(gradient_frame(args.width, args.height, i)))
    emit(enc.encode(None))

    out += frame(T_END)
    args.out.write_bytes(bytes(out))
    print(
        f"wrote {args.out}: {written} video AUs, {len(out)} bytes, "
        f"{args.width}x{args.height} @ {args.fps}fps"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
