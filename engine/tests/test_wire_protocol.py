"""The engine and the C++ app are different languages in different processes.

These numbers are the only contract between them, and a mismatch is silent
corruption rather than an error: the app would happily interpret an audio AU as
a video AU. So read the constants back out of the C++ header and assert they
match the Python ones. tests/cpp/test_protocol.cpp pins the same values from
the other side.
"""

from __future__ import annotations

import io
import pathlib
import re

import pytest

from wireplay_engine import Sink, annexb
import wireplay_engine as eng

REPO = pathlib.Path(__file__).resolve().parents[2]
HEADER = REPO / "src" / "wp" / "protocol.hpp"

EXPECTED = {
    "kStatus": eng.T_ENGINE_STATUS,
    "kFatal": eng.T_ENGINE_FATAL,
    "kStats": eng.T_ENGINE_STATS,
    "kDevice": eng.T_ENGINE_DEVICE,
    "kVideoInit": eng.T_ENGINE_VIDEO_INIT,
    "kVideoAU": eng.T_ENGINE_VIDEO_AU,
    "kAudioInit": eng.T_ENGINE_AUDIO_INIT,
    "kAudioAU": eng.T_ENGINE_AUDIO_AU,
    "kAudioPcmInit": eng.T_ENGINE_AUDIO_PCM_INIT,
    "kPing": eng.T_ENGINE_PING,
    "kEnd": eng.T_ENGINE_END,
    "kPong": eng.T_APP_PONG,
    "kRequestKey": eng.T_APP_REQUEST_KEY,
    "kRotate": eng.T_APP_ROTATE,
    "kRestartStream": eng.T_APP_RESTART_STREAM,
    "kTouch": eng.T_APP_TOUCH,
    "kButton": eng.T_APP_BUTTON,
    "kSetBitrate": eng.T_APP_SET_BITRATE,
}


def header_constants() -> dict[str, int]:
    text = HEADER.read_text()
    out: dict[str, int] = {}
    for name, value in re.findall(r"k(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", text):
        out["k" + name] = int(value, 16)
    return out


def enum_block(text: str, enum_name: str) -> dict[str, int]:
    """Constants of one enum only.

    kPing/kAppPing deliberately share 0xFE, and kEnd/kAppEnd share 0xFF,
    because they travel on opposite pipes. A duplicate check has to be scoped
    to a single direction to mean anything.
    """
    m = re.search(r"enum\s+" + enum_name + r"\s*:[^{]*\{(.*?)\}", text, re.S)
    assert m, f"enum {enum_name} not found in protocol.hpp"
    return {
        "k" + name: int(value, 16)
        for name, value in re.findall(r"k(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", m.group(1))
    }


def test_header_exists():
    assert HEADER.is_file(), f"{HEADER} is missing"


def test_every_engine_constant_matches_the_header():
    cpp = header_constants()
    assert cpp, "no constants parsed out of protocol.hpp"
    missing = [k for k in EXPECTED if k not in cpp]
    assert not missing, f"protocol.hpp is missing: {missing}"
    for name, py_value in EXPECTED.items():
        assert cpp[name] == py_value, (
            f"{name}: python says {py_value:#04x}, protocol.hpp says {cpp[name]:#04x}"
        )


@pytest.mark.parametrize("enum_name", ["EngineMsg", "AppMsg"])
def test_no_duplicate_type_values_within_a_direction(enum_name):
    """Two types sharing a byte on the same pipe would be undetectable at runtime."""
    consts = enum_block(HEADER.read_text(), enum_name)
    assert consts, f"no constants parsed out of {enum_name}"
    values: dict[int, list[str]] = {}
    for name, value in consts.items():
        values.setdefault(value, []).append(name)
    dupes = {hex(v): n for v, n in values.items() if len(n) > 1}
    assert not dupes, f"duplicate type values in {enum_name}: {dupes}"


class TestSinkFraming:
    """Sink must emit the exact framing the C++ FrameReader parses."""

    def test_frame_layout(self):
        buf = io.BytesIO()
        Sink(out=buf).send(eng.T_ENGINE_END)
        # len counts the type byte, so a payload-less frame is 00 00 00 01 FF.
        assert buf.getvalue() == bytes([0, 0, 0, 1, 0xFF])

    def test_json_frame(self):
        buf = io.BytesIO()
        Sink(out=buf).json(eng.T_ENGINE_STATUS, {"state": "streaming"})
        raw = buf.getvalue()
        assert int.from_bytes(raw[:4], "big") == len(raw) - 4
        assert raw[4] == eng.T_ENGINE_STATUS
        assert raw[5:] == b'{"state":"streaming"}'

    def test_video_au_framing(self):
        """The app reads [ts][key][flags] then Annex-B; the order is load-bearing."""
        import struct

        buf = io.BytesIO()
        body = annexb([b"\x42\x01xyz"])
        Sink(out=buf).send(eng.T_ENGINE_VIDEO_AU, struct.pack(">IBB", 24000, 1, 0) + body)
        raw = buf.getvalue()
        payload = raw[5:]
        ts, key, flags = struct.unpack(">IBB", payload[:6])
        assert (ts, key, flags) == (24000, 1, 0)
        assert payload[6:] == body

    def test_rejects_oversized_frame(self):
        with pytest.raises(ValueError):
            Sink(out=io.BytesIO()).send(eng.T_ENGINE_VIDEO_AU, bytes(eng.MAX_FRAME))


class TestHelpers:
    def test_annexb(self):
        assert annexb([b"\x40\x01ab", b"\x42\x01c"]) == (
            b"\x00\x00\x00\x01\x40\x01ab\x00\x00\x00\x01\x42\x01c"
        )



# ---------------------------------------------------------------------------
# kVideoInit grew a fourth field when the QuickTime USB backend (H.264) joined
# the CoreDevice one (HEVC). The app has to build its decoder before the first
# access unit arrives, so the codec travels in this header.
# ---------------------------------------------------------------------------
def test_video_codec_enum_matches_the_header():
    """The wire codec ids are shared with C++; read them back out of the header."""
    text = HEADER.read_text()
    m = re.search(r"enum VideoCodec\s*:\s*uint32_t\s*\{(.*?)\}", text, re.S)
    assert m, "protocol.hpp lost the VideoCodec enum"
    values = dict(re.findall(r"(kCodec\w+)\s*=\s*(\d+)", m.group(1)))
    assert values == {"kCodecHevc": "0", "kCodecH264": "1"}, values
    assert eng.CODEC_HEVC == 0 and eng.CODEC_H264 == 1


def test_video_init_is_four_fields_on_every_side():
    """Three producers/consumers, one layout -- a mismatch picks the wrong decoder.

    Nothing would report it: the app would read VPS bytes as a codec id, fall
    back to HEVC, and fail on the first H.264 access unit.
    """
    engine = (REPO / "engine" / "wireplay_engine.py").read_text()
    assert 'struct.pack(">IIII", 0, 0, 0, CODEC_H264)' in engine

    dump = (REPO / "tools" / "make_offline_dump.py").read_text()
    assert 'struct.pack(">IIII", 0, 0, 0, 1 if args.codec == "h264" else 0)' in dump

    main = (REPO / "src" / "main.cpp").read_text()
    case = re.search(r"case wp::kVideoInit: \{(.*?)\n        \}", main, re.S)
    assert case, "main.cpp lost the kVideoInit case"
    assert len(re.findall(r"readU32BE", case.group(1))) == 4, \
        "the app must read width, height, flags and codec"
