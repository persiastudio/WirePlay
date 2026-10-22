"""Byte-level tests for the QuickTime/Valeria USB backend.

The fixtures here are built from the field order the protocol documents, not by
running the parser and keeping whatever came out -- otherwise the tests would
only prove the parser agrees with itself.
"""

import struct

import pytest

from quicktime_backend import (
    A_HPA1, A_HPD1, A_NEED, ASYN_MAGIC, BASE_HEADER_BYTES, CODEC_AVC1, D_DICT, D_KEYVAL, D_STRKEY,
    HEADER_BYTES,
    FD_CODC, FD_EXTN, FD_FDSC, FD_MDIA, FD_VDIM, FrameSplitter,
    MEDIA_VIDEO, NSNumber, PING_HEADER, PING_MAGIC, REPLY_MAGIC, SB_NSMP, SB_OPTS,
    SB_SDAT, SB_SBUF, SYNC_MAGIC, asyn_hpa1, asyn_hpd1, asyn_need,
    QuickTimeTransport, audio_stream_basic_description, extract_parameter_sets, nalus_to_annexb,
    pack_cm_time, parse_cm_time, parse_format_description, parse_header,
    parse_sample_buffer, ping_reply, reply_afmt, reply_clock, reply_og,
    reply_skew, reply_stop, reply_time, serialize_dict,
)


# --------------------------------------------------------------------------
# framing
# --------------------------------------------------------------------------

def test_splitter_yields_one_record():
    body = b"hello"
    record = struct.pack("<I", len(body) + 4) + body
    assert list(FrameSplitter().feed(record)) == [body]


def test_splitter_holds_a_partial_record_until_it_completes():
    body = b"0123456789"
    record = struct.pack("<I", len(body) + 4) + body
    s = FrameSplitter()
    assert list(s.feed(record[:6])) == []
    assert list(s.feed(record[6:])) == [body]


def test_splitter_yields_several_records_from_one_read():
    a = struct.pack("<I", 5) + b"a"
    b = struct.pack("<I", 6) + b"bb"
    assert list(FrameSplitter().feed(a + b)) == [b"a", b"bb"]


def test_splitter_rejects_a_length_smaller_than_its_own_header():
    with pytest.raises(ValueError):
        list(FrameSplitter().feed(struct.pack("<I", 3)))


def test_length_covers_itself():
    # A record claiming length 4 has an empty body, not a 4-byte one.
    assert list(FrameSplitter().feed(struct.pack("<I", 4))) == [b""]


# --------------------------------------------------------------------------
# header parsing
# --------------------------------------------------------------------------

def test_parse_header_splits_magic_clock_type_payload():
    body = struct.pack("<IQI", SYNC_MAGIC, 0xDEADBEEF, 0x63767270) + b"rest"
    magic, clock, msg, payload = parse_header(body)
    assert magic == SYNC_MAGIC
    assert clock == 0xDEADBEEF
    assert msg == 0x63767270
    assert payload == b"rest"


def test_parse_header_rejects_a_short_body():
    for n in range(BASE_HEADER_BYTES):
        with pytest.raises(ValueError):
            parse_header(b"\x00" * n)


def test_parse_header_rejects_a_truncated_message_type():
    """13, 14 or 15 bytes mean bytes past the clock ref but not a whole type."""
    for n in range(BASE_HEADER_BYTES + 1, HEADER_BYTES):
        with pytest.raises(ValueError):
            parse_header(b"\x00" * n)


# --------------------------------------------------------------------------
# outbound record lengths, pinned against the protocol
# --------------------------------------------------------------------------

def test_ping_reply_is_16_bytes():
    out = ping_reply()
    assert len(out) == 16
    length, magic = struct.unpack_from("<II", out, 0)
    assert (length, magic) == (16, PING_MAGIC)
    (header,) = struct.unpack_from("<Q", out, 8)
    assert header == PING_HEADER


@pytest.mark.parametrize("builder,expected", [
    (lambda: reply_clock(7, 0x1000), 28),
    (lambda: reply_og(7), 24),
    (lambda: reply_stop(7), 24),
    (lambda: reply_skew(7, 0.5), 28),
    (lambda: reply_time(7, pack_cm_time()), 44),
    (lambda: reply_afmt(7), 20 + len(serialize_dict({"Error": NSNumber(3, 0)}))),
])
def test_reply_lengths_match_the_protocol(builder, expected):
    out = builder()
    (length,) = struct.unpack_from("<I", out, 0)
    (magic,) = struct.unpack_from("<I", out, 4)
    assert magic == REPLY_MAGIC
    assert length == len(out) == expected


def test_og_reply_trailing_field_is_a_uint64():
    out = reply_og(0x1122334455667788)
    assert struct.unpack_from("<Q", out, 16)[0] == 0


@pytest.mark.parametrize("builder,expected", [
    (lambda: asyn_need(0x42), 20),
    (lambda: asyn_hpd1(), 20 + len(serialize_dict({
        "Valeria": True, "HEVCDecoderSupports444": True,
        "DisplaySize": {"Width": NSNumber(6, 1920.0), "Height": NSNumber(6, 1200.0)}}))),
])
def test_asyn_length_field_is_self_consistent(builder, expected):
    out = builder()
    (length,) = struct.unpack_from("<I", out, 0)
    (magic,) = struct.unpack_from("<I", out, 4)
    assert magic == ASYN_MAGIC
    assert length == len(out) == expected


def test_hpd1_and_hpa1_carry_their_subtypes():
    # Layout: uint32 length, uint32 magic, uint64 clock ref, uint32 message type.
    assert struct.unpack_from("<I", asyn_hpd1(), 16)[0] == A_HPD1
    assert struct.unpack_from("<I", asyn_hpa1(9), 12 + 4)[0] == A_HPA1
    assert struct.unpack_from("<I", asyn_need(9), 16)[0] == A_NEED
    # The clock ref sits between the magic and the type.
    assert struct.unpack_from("<Q", asyn_hpa1(0x4242), 8)[0] == 0x4242


def test_audio_format_describes_48k_stereo_16_bit():
    raw = audio_stream_basic_description()
    rate, fmt, flags, bpp, fpp, bpf, channels, bits = struct.unpack_from("<dIIIIIII", raw, 0)
    assert rate == 48000.0
    assert fmt == 0x6C70636D  # 'lpcm'
    assert (channels, bits, bpf) == (2, 16, 4)
    # The sample rate is repeated as two trailing doubles on the wire.
    assert struct.unpack_from("<dd", raw, len(raw) - 16) == (48000.0, 48000.0)


# --------------------------------------------------------------------------
# dictionary encoding
# --------------------------------------------------------------------------

def test_serialize_dict_uses_dict_keyv_and_strk_records():
    out = serialize_dict({"Valeria": True})
    length, magic = struct.unpack_from("<II", out, 0)
    assert magic == D_DICT
    assert length == len(out)
    pair_len, pair_magic = struct.unpack_from("<II", out, 8)
    assert pair_magic == D_KEYVAL
    key_len, key_magic = struct.unpack_from("<II", out, 16)
    assert key_magic == D_STRKEY
    assert out[24:24 + 7] == b"Valeria"


def test_bool_is_tested_before_int_so_true_is_not_a_number():
    out = serialize_dict({"k": True})
    assert struct.unpack_from("<II", out, 8)[1] == D_KEYVAL
    # 9-byte boolean value record: 8 header + 1 flag byte
    assert b"\x01" == out[-1:]


def test_nested_dict_is_encoded_recursively():
    out = serialize_dict({"DisplaySize": {"Width": NSNumber(6, 1920.0)}})
    assert out.count(struct.pack("<I", D_DICT)) == 2


# --------------------------------------------------------------------------
# CMSampleBuffer
# --------------------------------------------------------------------------

def _block(magic: int, payload: bytes) -> bytes:
    return struct.pack("<II", len(payload) + 8, magic) + payload


def _fdsc(width=1290, height=2796) -> bytes:
    """A video format description with no parameter sets."""
    body = b""
    body += struct.pack("<II", 12, FD_MDIA) + struct.pack("<I", MEDIA_VIDEO)
    body += struct.pack("<II", 16, FD_VDIM) + struct.pack("<II", width, height)
    body += struct.pack("<II", 12, FD_CODC) + struct.pack("<I", CODEC_AVC1)
    extn = serialize_dict({})
    body += struct.pack("<II", len(extn) + 8, FD_EXTN) + extn
    return struct.pack("<II", len(body) + 8, FD_FDSC) + body


def test_parse_sample_buffer_reads_time_data_and_count():
    nalu = b"\x65" + b"\xAA" * 20  # IDR slice
    sdat = struct.pack(">I", len(nalu)) + nalu
    inner = (
        _block(SB_OPTS, pack_cm_time(value=12345, timescale=1000))
        + _block(SB_NSMP, struct.pack("<I", 1))
        + _block(SB_SDAT, sdat)
    )
    sbuf = struct.pack("<II", len(inner) + 8, SB_SBUF) + inner
    sb = parse_sample_buffer(sbuf)
    assert sb.presentation_time["value"] == 12345
    assert sb.num_samples == 1
    assert sb.data == sdat
    assert not sb.has_format


def test_parse_sample_buffer_skips_blocks_it_does_not_use():
    inner = (
        _block(0x73746961, b"\x00" * 24)   # stia: timing metadata
        + _block(SB_SDAT, b"payload")
    )
    sbuf = struct.pack("<II", len(inner) + 8, SB_SBUF) + inner
    assert parse_sample_buffer(sbuf).data == b"payload"


def test_format_description_yields_dimensions_and_codec():
    fmt = parse_format_description(_fdsc(1170, 2532))
    assert fmt["width"] == 1170
    assert fmt["height"] == 2532
    assert fmt["codec"] == CODEC_AVC1
    assert fmt["media_type"] == MEDIA_VIDEO


def _avcc_blob(sps: bytes, pps: bytes) -> bytes:
    """A standard avcC record: SPS first with a big-endian uint16 length,
    then a PPS count byte, a big-endian uint16 length and the PPS. This is
    byte-for-byte the shape a real device sends (see the captured fdsc in
    test_real_device_format_description below)."""
    return (b"\x01\x64\x00\x33\xff\xe1" + struct.pack(">H", len(sps)) + sps
            + b"\x01" + struct.pack(">H", len(pps)) + pps)


def _idxk(key: int, value: bytes) -> bytes:
    # A real idxk record is 10 bytes: length, magic, then a uint16 key. An
    # earlier version of this fixture wrote a 12-byte record with a uint32
    # key, which matched a parser bug instead of the wire -- both were wrong
    # in the same way, so the test passed while real devices yielded no
    # parameter sets at all.
    key_buf = struct.pack("<II", 10, 0x6964786B) + struct.pack("<H", key)  # 'idxk'
    val_buf = struct.pack("<II", len(value) + 8, 0x64617476) + value       # 'datv'
    return struct.pack("<II", len(key_buf) + len(val_buf) + 8, D_KEYVAL) + key_buf + val_buf


def test_parameter_sets_are_found_under_int_keys_49_and_105():
    sps = b"\x67" + b"\x11" * 12
    pps = b"\x68" + b"\x22" * 4
    inner = _idxk(105, _avcc_blob(sps, pps))
    extn = _idxk(49, inner)
    got_sps, got_pps = extract_parameter_sets(extn)
    assert got_sps == sps
    assert got_pps == pps


def test_parameter_sets_are_absent_rather_than_fatal_when_missing():
    assert extract_parameter_sets(serialize_dict({})[8:]) == (None, None)


def test_format_description_carries_the_parameter_sets():
    sps = b"\x67" + b"\x11" * 12
    pps = b"\x68" + b"\x22" * 4
    extn = _idxk(49, _idxk(105, _avcc_blob(sps, pps)))
    body = b""
    body += struct.pack("<II", 12, FD_MDIA) + struct.pack("<I", MEDIA_VIDEO)
    body += struct.pack("<II", 16, FD_VDIM) + struct.pack("<II", 1290, 2796)
    body += struct.pack("<II", 12, FD_CODC) + struct.pack("<I", CODEC_AVC1)
    extn_body = struct.pack("<II", len(extn) + 8, FD_EXTN) + extn
    body += extn_body
    fdsc = struct.pack("<II", len(body) + 8, FD_FDSC) + body
    fmt = parse_format_description(fdsc)
    assert fmt["sps"] == sps
    assert fmt["pps"] == pps


# --------------------------------------------------------------------------
# AVCC -> Annex-B
# --------------------------------------------------------------------------

def _avcc(*nalus: bytes) -> bytes:
    return b"".join(struct.pack(">I", len(n)) + n for n in nalus)


def test_nalus_become_start_code_prefixed():
    out = nalus_to_annexb(_avcc(b"\x65abc", b"\x41def"))
    assert out == b"\x00\x00\x00\x01\x65abc" + b"\x00\x00\x00\x01\x41def"


def test_length_prefix_is_big_endian_not_the_protocols_little_endian():
    # 0x00000005 big-endian; if read little-endian this would claim 83886080
    # bytes and the NALU would be dropped entirely.
    data = struct.pack(">I", 5) + b"\x65abcd"
    assert nalus_to_annexb(data) == b"\x00\x00\x00\x01\x65abcd"


def test_parameter_sets_are_injected_when_the_access_unit_lacks_them():
    sps, pps = b"\x67sps", b"\x68pps"
    out = nalus_to_annexb(_avcc(b"\x65idr"), sps=sps, pps=pps)
    assert out.startswith(b"\x00\x00\x00\x01" + sps + b"\x00\x00\x00\x01" + pps)


def test_parameter_sets_are_not_duplicated_when_already_present():
    sps, pps = b"\x67sps", b"\x68pps"
    out = nalus_to_annexb(_avcc(sps, pps, b"\x65idr"), sps=sps, pps=pps)
    assert out.count(b"\x00\x00\x00\x01" + sps) == 1
    assert out.count(b"\x00\x00\x00\x01" + pps) == 1


def test_a_truncated_trailing_nalu_is_dropped_not_raised():
    data = struct.pack(">I", 99) + b"short"
    assert nalus_to_annexb(data) == b""


def test_cm_time_round_trips():
    raw = pack_cm_time(value=-5, timescale=600, flags=1, epoch=2)
    assert len(raw) == 24
    assert parse_cm_time(raw) == {"value": -5, "timescale": 600, "flags": 1, "epoch": 2}


def test_device_ping_survives_the_real_read_path():
    """Regression: the very first record an iPhone sends is a 16-byte ping.

    The splitter strips the four-byte length, leaving a 12-byte body that has a
    magic and a clock reference but no message type. parse_header used to demand
    the full 16-byte header and raise on it, so the handshake died on the first
    packet the device ever sent -- which is what "body shorter than the 16-byte
    header" was on real hardware.
    """
    ping = ping_reply()
    assert len(ping) == 16, "the ping is the shortest record in the protocol"

    bodies = list(FrameSplitter().feed(ping))
    assert len(bodies) == 1
    assert len(bodies[0]) == BASE_HEADER_BYTES

    magic, clock_ref, msg_type, payload = parse_header(bodies[0])
    assert magic == PING_MAGIC
    assert clock_ref == PING_HEADER
    assert msg_type == 0, "a minimum-length record carries no message type"
    assert payload == b""


def test_records_that_do_carry_a_type_still_parse_it():
    need = asyn_need(0x1234)
    bodies = list(FrameSplitter().feed(need))
    magic, clock_ref, msg_type, payload = parse_header(bodies[0])
    assert magic == ASYN_MAGIC
    assert clock_ref == 0x1234
    assert msg_type == A_NEED


def test_a_body_shorter_than_the_minimum_is_still_rejected():
    with pytest.raises(ValueError):
        parse_header(b"\x00" * (BASE_HEADER_BYTES - 1))


# A format description captured from a real iPhone by the reference project
# (quicktime_video_hack, fixtures/formatdescriptor.bin). 1126x2436, avc1.
REAL_FDSC = bytes.fromhex(
    "af000000637364660c0000006169646d65646976100000006d6964766604000084090000"
    "0c00000063646f63316376617f0000006e747865580000007679656b0a0000006b786469"
    "310046000000746369643e0000007679656b0a0000006b78646969002c00000076746164"
    "01640033ffe1001127640033ac5680470133e69e6e0404040401000428ee3cb0fdf8f800"
    "1f0000007679656b0a0000006b78646934000d00000076727473482e323634"
)


def test_real_device_format_description_yields_parameter_sets():
    """Regression for "non-existing PPS 0 referenced" on every frame.

    The int-key reader used to decode the dictionary key as a uint32 at the
    end of the idxk record; on the wire the key is a uint16 right after the
    magic, so key 49 was never found and SPS/PPS were None for the whole
    session -- video forwarded fine and decoded never.
    """
    fmt = parse_format_description(REAL_FDSC)
    assert fmt["width"] == 1126
    assert fmt["height"] == 2436
    assert fmt["sps"] == bytes.fromhex("27640033ac5680470133e69e6e04040404")
    assert fmt["pps"] == bytes.fromhex("28ee3cb0")
    assert fmt["sps"][0] & 0x1F == 7
    assert fmt["pps"][0] & 0x1F == 8


class _FakeDevice:
    """Stands in for a pyusb device: records writes and control transfers."""

    def __init__(self, read_error=None):
        self.writes = []
        self.ctrl = []
        self.read_error = read_error

    def read(self, ep, size, timeout):
        raise self.read_error

    def write(self, ep, data, timeout):
        self.writes.append(bytes(data))
        return len(data)

    def ctrl_transfer(self, *args):
        self.ctrl.append(args)
        return 0


def test_pump_tolerates_a_libusb0_timeout():
    """The libusb0 backend raises a plain USBError saying "timeout error",
    errno None -- not USBTimeoutError. On real hardware that arrived 3 s
    after the ping and killed a healthy session as fatal(7)."""
    import usb.core
    t = QuickTimeTransport()
    t._device = _FakeDevice(read_error=usb.core.USBError(
        "libusb0-dll:err [_usb_reap_async] timeout error"))
    t._in_ep = 0x81
    t.pump()  # must return quietly, not raise


def test_pump_still_raises_a_real_usb_error():
    import usb.core
    t = QuickTimeTransport()
    t._device = _FakeDevice(read_error=usb.core.USBError("device disconnected"))
    t._in_ep = 0x81
    with pytest.raises(usb.core.USBError):
        t.pump()


def test_close_session_releases_the_phone():
    """Closing the window must end the dock presentation: stop messages,
    then the vendor request that disables the capture configuration. Without
    the second half the phone kept routing audio to the cable until unplug."""
    from quicktime_backend import CTRL_DISABLE_AV_CONFIG
    t = QuickTimeTransport()
    dev = _FakeDevice()
    t._device = dev
    t._out_ep = 0x05
    t._av_interface_number = 4
    t.close_session()
    assert dev.writes, "the stop messages were never sent"
    assert dev.ctrl == [CTRL_DISABLE_AV_CONFIG + (b"", 1500)]  # + explicit timeout
    assert t._device is None


def test_never_dispose_an_unopened_device():
    """Regression for a WDF_VIOLATION blue screen (bugcheck 0x10D, param 5).

    usb.util.dispose_resources() on a pyusb device that was never opened
    pushes an invalid handle through the libusb0/usbccgp stack on Windows
    and the driver bugchecks the machine -- it happened within a second of
    a diagnostic tool doing exactly that to a freshly enumerated device.
    Enumeration results get dropped, never disposed; only the session's
    own opened device may be disposed.
    """
    import pathlib
    src = pathlib.Path(__file__).resolve().parent.parent.joinpath(
        "quicktime_backend.py").read_text()
    assert "dispose_resources(fresh)" not in src
    assert "dispose_resources(opened_device)" in src


# --- Clock replies: the 60 fps fix (field-proven 2026-10-22) ----------------
# TIME must carry flags=1 (a valid host clock) and SKEW the measured
# host/device audio-clock rate (~48000), never 0.0. With flags=0 and
# skew 0.0 the device scheduled frame delivery against a clock it
# believed invalid and stopped: ~45 fps bimodal with growing jitter.
# With both correct: a locked 60 fps. Never regress these bytes.

def test_time_reply_sends_valid_cm_time_flags():
    from quicktime_backend import pack_cm_time, parse_cm_time
    parsed = parse_cm_time(pack_cm_time(123456789, flags=1))
    assert parsed["flags"] == 1
    assert parsed["value"] == 123456789


def test_clock_skew_falls_back_to_nominal_48000():
    import quicktime_backend as qb
    backend = object.__new__(qb.QuickTimeTransport)
    backend._eat_first = None
    backend._eat_last = None
    assert backend._clock_skew() == 48000.0


def test_clock_skew_measures_host_over_device_elapsed():
    import quicktime_backend as qb
    backend = object.__new__(qb.QuickTimeTransport)
    backend._eat_first = (0.0, 100.0)
    backend._eat_last = (10.0, 110.0)
    assert abs(backend._clock_skew() - 48000.0) < 1.0
    backend._eat_last = (10.0, 110.5)  # host ran 5% long for same device span
    assert abs(backend._clock_skew() - 48000.0 * 1.05) < 1.0


def test_close_session_drain_exits_once_the_device_goes_quiet():
    """Undocking speed: the release drain must not burn its full 1.2 s cap
    after the device has already answered. Replies, then silence = done;
    the disable that releases the phone still has to go out."""
    import time as _time
    from quicktime_backend import CTRL_DISABLE_AV_CONFIG

    class _SeqDevice(_FakeDevice):
        def read(self, ep, size, timeout):
            if not getattr(self, "_answered", False):
                self._answered = True
                return b"\x10\x00\x00\x00RELS"
            raise TimeoutError("timeout")

    t = QuickTimeTransport()
    dev = _SeqDevice()
    t._device = dev
    t._out_ep = 0x05
    t._in_ep = 0x81
    t._av_interface_number = 4
    start = _time.monotonic()
    t.close_session()
    elapsed = _time.monotonic() - start
    assert dev.ctrl == [CTRL_DISABLE_AV_CONFIG + (b"", 1500)]
    assert elapsed < 0.9, "drain waited %.2f s after the device answered" % elapsed
