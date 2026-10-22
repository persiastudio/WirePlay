"""QuickTime / Valeria USB screen-and-audio capture.

This is the backend that needs no Developer Mode and no DeveloperDiskImage: the
device exposes a hidden USB configuration that carries an H.264 video stream and
48 kHz PCM audio straight over bulk endpoints. It is what every screen-mirroring
app the user compares this one against uses, and it is why they work on a stock
phone.

Wire format, as observed and as implemented by the MIT-licensed reference
projects this was derived from (YueChen-C/ios-screen-record and
danielpaulus/quicktime_video_hack):

* Every record on the wire is a little-endian uint32 *total* length, including
  those 4 bytes, followed by length-4 bytes of body.
* Every body starts with a 16-byte header: uint32 packet magic ("ping", "sync"
  or "asyn"), uint64 clock reference, uint32 message type.
* "sync" packets are a request/reply handshake the device drives. We answer with
  "rply" records. Nothing streams until CVRP has been answered.
* "asyn" FEED packets carry a CMSampleBuffer: length-prefixed H.264 NALUs for
  video, raw PCM for audio. EAT packets carry audio.

The pure functions here (framing, header parsing, record building, CMSampleBuffer
parsing) are covered by byte-fixture tests in engine/tests/test_quicktime.py.
The USB transport at the bottom is not: nothing in this repository has ever been
run against a device on this path.
"""

from __future__ import annotations

import struct
from typing import Any, Dict, Iterator, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Magics. Stored as the integers the wire carries, little-endian.
# ---------------------------------------------------------------------------

PING_MAGIC = 0x70696E67
SYNC_MAGIC = 0x73796E63
ASYN_MAGIC = 0x6173796E
REPLY_MAGIC = 0x72706C79

PING_LENGTH = 16
PING_HEADER = 0x0000000100000000

# sync message types
S_TIME = 0x74696D65  # "time"
S_CWPA = 0x63777061  # "cwpa" - create audio
S_AFMT = 0x61666D74  # "afmt" - audio format
S_CVRP = 0x63767270  # "cvrp" - create video; the reply starts the video stream
S_CLOK = 0x636C6F6B  # "clok" - clock
S_OG = 0x676F2120    # "go! "
S_SKEW = 0x736B6577  # "skew"
S_STOP = 0x73746F70  # "stop"

# asyn message types
A_FEED = 0x66656564  # "feed" - CMSampleBuffer, video
A_EAT = 0x65617421   # "eat!" - CMSampleBuffer, audio
A_TJMP = 0x746A6D70
A_SRAT = 0x73726174
A_SPRP = 0x73707270
A_TBAS = 0x74626173
A_RELS = 0x72656C73  # "rels" - device released the session
A_HPD1 = 0x68706431  # display connected
A_HPA1 = 0x68706131  # audio connected
A_HPD0 = 0x68706430
A_HPA0 = 0x68706130
A_NEED = 0x6E656564  # "need" - ask for the next buffer

# serialised dictionary
D_KEYVAL = 0x6B657976  # "keyv"
D_STRKEY = 0x7374726B  # "strk"
D_BOOL = 0x62756C76    # "bulv"
D_DICT = 0x64696374    # "dict"
D_DATA = 0x64617476    # "datv"
D_STR = 0x73747276     # "strv"
D_NUM = 0x6E6D6276     # "nmbv"

# CMSampleBuffer
SB_SBUF = 0x73627566
SB_OPTS = 0x6F707473
SB_STIA = 0x73746961
SB_SDAT = 0x73646174
SB_SATT = 0x73617474
SB_SARY = 0x73617279
SB_SSIZ = 0x7373697A
SB_NSMP = 0x6E736D70

# CMFormatDescription
FD_FDSC = 0x66647363  # "fdsc"
FD_MDIA = 0x6D646961  # "mdia"
FD_VDIM = 0x7664696D  # "vdim"
FD_CODC = 0x636F6463  # "codc"
FD_EXTN = 0x6578746E  # "extn"
FD_ASBD = 0x61736264  # "asbd"
MEDIA_VIDEO = 0x76696465  # "vide"
MEDIA_SOUND = 0x736F756E  # "soun"
CODEC_AVC1 = 0x61766331   # "avc1"

CM_TIME_BYTES = 24
HEADER_BYTES = 16
# Every record carries a magic and a clock reference; only records longer than
# the minimum carry a message type. The ping is exactly 16 bytes total, so once
# the splitter strips the four-byte length its body is 12 bytes and has no type
# field at all.
BASE_HEADER_BYTES = 12

# USB. The device hides the capture configuration until asked for it.
USB_INTERFACE_SUBCLASS_MUX = 0xFE
USB_INTERFACE_SUBCLASS_AV = 0x2A


# ---------------------------------------------------------------------------
# Length/magic records
# ---------------------------------------------------------------------------

def _lm(length: int, magic: int) -> bytes:
    return struct.pack("<II", length, magic)


def parse_length_magic(buf: bytes, expected_magic: int) -> Tuple[int, bytes]:
    """Split a `<I length><I magic>` record. Length counts the 8 header bytes."""
    if len(buf) < 8:
        raise ValueError("record shorter than its length/magic header")
    length, magic = struct.unpack("<II", buf[:8])
    if magic != expected_magic:
        raise ValueError(
            "expected magic %08x, found %08x" % (expected_magic, magic))
    if length > len(buf):
        raise ValueError("record claims %d bytes, only %d present" % (length, len(buf)))
    return length, buf[8:]


# ---------------------------------------------------------------------------
# Numbers and dictionaries
# ---------------------------------------------------------------------------

class NSNumber:
    """The tagged number the device's dictionaries use.

    Type specifier 3 is uint32, 4 uint64, 6 float64.
    """

    def __init__(self, type_specifier: int, value: Any) -> None:
        self.type_specifier = type_specifier
        self.value = value

    def to_bytes(self) -> bytes:
        if self.type_specifier == 3:
            return b"\x03" + struct.pack("<I", self.value)
        if self.type_specifier == 4:
            return b"\x04" + struct.pack("<Q", self.value)
        if self.type_specifier == 6:
            return b"\x06" + struct.pack("<d", self.value)
        raise ValueError("unsupported number type specifier %r" % self.type_specifier)

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return "NSNumber(%d, %r)" % (self.type_specifier, self.value)


def _serialize_value(value: Any) -> bytes:
    # bool must be tested before int: True is an int in Python.
    if isinstance(value, bool):
        return _lm(9, D_BOOL) + (b"\x01" if value else b"\x00")
    if isinstance(value, NSNumber):
        payload = value.to_bytes()
        return _lm(len(payload) + 8, D_NUM) + payload
    if isinstance(value, str):
        payload = value.encode("utf-8")
        return _lm(len(payload) + 8, D_STR) + payload
    if isinstance(value, (bytes, bytearray)):
        return _lm(len(value) + 8, D_DATA) + bytes(value)
    if isinstance(value, dict):
        return serialize_dict(value)
    raise ValueError("cannot serialise %r" % (value,))


def serialize_dict(data: Dict[str, Any]) -> bytes:
    """Build the device's string-keyed dictionary encoding."""
    body = b""
    for key, value in data.items():
        raw_key = key.encode("utf-8")
        key_buf = _lm(len(raw_key) + 8, D_STRKEY) + raw_key
        value_buf = _serialize_value(value)
        body += _lm(len(key_buf) + len(value_buf) + 8, D_KEYVAL) + key_buf + value_buf
    return _lm(len(body) + 8, D_DICT) + body


# ---------------------------------------------------------------------------
# Wire framing
# ---------------------------------------------------------------------------

class FrameSplitter:
    """Splits the byte stream into records.

    Each record is a little-endian uint32 total length -- including the four
    length bytes -- followed by that many minus four bytes of body. Bulk reads
    arrive in arbitrary chunks and a single read can hold several records, so
    the splitter keeps the tail and yields whatever is complete.
    """

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> Iterator[bytes]:
        self._buf.extend(data)
        while len(self._buf) >= 4:
            (total,) = struct.unpack_from("<I", self._buf, 0)
            if total < 4:
                # A length below the size of the length field itself cannot be
                # a record; the stream is desynchronised and guessing would
                # only produce garbage further along.
                raise ValueError("record length %d is smaller than its own header" % total)
            if len(self._buf) < total:
                return
            body = bytes(self._buf[4:total])
            del self._buf[:total]
            yield body


def parse_header(body: bytes) -> Tuple[int, int, int, bytes]:
    """Split a record body into (packet magic, clock ref, message type, payload)."""
    if len(body) < BASE_HEADER_BYTES:
        raise ValueError("body shorter than the %d-byte header" % BASE_HEADER_BYTES)
    if BASE_HEADER_BYTES < len(body) < HEADER_BYTES:
        # Bytes past the clock reference but too few for a whole message type.
        # A record is either the minimum length or carries a full type; anything
        # in between is a desynchronised stream and must not be guessed at.
        raise ValueError("body of %d bytes carries a truncated message type" % len(body))
    magic, clock_ref = struct.unpack_from("<IQ", body, 0)
    msg_type = 0
    if len(body) >= HEADER_BYTES:
        msg_type = struct.unpack_from("<I", body, BASE_HEADER_BYTES)[0]
    return magic, clock_ref, msg_type, body[HEADER_BYTES:]


def parse_sync_header(body: bytes) -> Tuple[int, int, bytes]:
    """A sync body is the common header, then a uint64 correlation id."""
    clock_ref, correlation_id, payload = _sync_fields(body)
    return clock_ref, correlation_id, payload


def _sync_fields(body: bytes) -> Tuple[int, int, bytes]:
    magic, clock_ref, msg_type, rest = parse_header(body)
    if magic != SYNC_MAGIC:
        raise ValueError("not a sync packet (magic %08x)" % magic)
    if len(rest) < 8:
        raise ValueError("sync packet has no correlation id")
    (correlation_id,) = struct.unpack_from("<Q", rest, 0)
    return clock_ref, correlation_id, rest[8:]


# ---------------------------------------------------------------------------
# Outbound records
# ---------------------------------------------------------------------------

def _reply(correlation_id: int, payload: bytes) -> bytes:
    # 20, not 16: the length field counts itself and the magic (8 bytes) plus
    # the correlation id and status word (12) plus the payload.
    return _lm(20 + len(payload), REPLY_MAGIC) + \
        struct.pack("<QI", correlation_id, 0) + payload


def reply_clock(correlation_id: int, clock_ref: int) -> bytes:
    return _reply(correlation_id, struct.pack("<Q", clock_ref))


def reply_og(correlation_id: int) -> bytes:
    # Byte-faithful to what the device expects: the trailing field is a uint64
    # zero, not the uint32 zero plus payload that _reply() would emit. The total
    # length is the same either way, but there is no reason to differ.
    return _lm(24, REPLY_MAGIC) + struct.pack("<QQ", correlation_id, 0)


def reply_stop(correlation_id: int) -> bytes:
    return _reply(correlation_id, struct.pack("<I", 0))


def reply_skew(correlation_id: int, skew: float) -> bytes:
    return _reply(correlation_id, struct.pack("<d", skew))


def reply_time(correlation_id: int, cm_time: bytes) -> bytes:
    if len(cm_time) != CM_TIME_BYTES:
        raise ValueError("CMTime must be %d bytes, got %d" % (CM_TIME_BYTES, len(cm_time)))
    return _reply(correlation_id, cm_time)


def reply_afmt(correlation_id: int) -> bytes:
    """The audio format answer: an empty error field, meaning success."""
    return _reply(correlation_id, serialize_dict({"Error": NSNumber(3, 0)}))


def _asyn(clock_ref: int, msg_type: int, payload: bytes = b"") -> bytes:
    return _lm(20 + len(payload), ASYN_MAGIC) + \
        struct.pack("<QI", clock_ref, msg_type) + payload


def ping_reply() -> bytes:
    return _lm(PING_LENGTH, PING_MAGIC) + struct.pack("<Q", PING_HEADER)


def asyn_need(clock_ref: int) -> bytes:
    """Ask for the next sample buffer. The device streams while we keep asking."""
    return _asyn(clock_ref, A_NEED)


def asyn_hpd0() -> bytes:
    return _asyn(1, A_HPD0)


def asyn_hpa0(clock_ref: int) -> bytes:
    return _asyn(clock_ref, A_HPA0)


def audio_stream_basic_description() -> bytes:
    """48 kHz stereo signed 16-bit little-endian PCM, as the device expects it.

    The struct is followed by the sample rate repeated as two doubles; that
    duplication is on the wire, not a mistake in this encoder.
    """
    fields = struct.pack(
        "<dIIIIIII",
        48000.0,   # SampleRate
        0x6C70636D,  # FormatID 'lpcm'
        12,        # FormatFlags: little-endian packed integer
        4,         # BytesPerPacket
        1,         # FramesPerPacket
        4,         # BytesPerFrame
        2,         # ChannelsPerFrame
        16,        # BitsPerChannel
    ) + struct.pack("<I", 0)  # Reserved
    return fields + struct.pack("<dd", 48000.0, 48000.0)


def asyn_hpd1(width: int = 1920, height: int = 1200) -> bytes:
    """Announce a display. The device will not send video before this."""
    return _asyn(
        1,
        A_HPD1,
        serialize_dict({
            "Valeria": True,
            "HEVCDecoderSupports444": True,
            "DisplaySize": {
                "Width": NSNumber(6, float(width)),
                "Height": NSNumber(6, float(height)),
            },
        }),
    )


def asyn_hpa1(clock_ref: int) -> bytes:
    """Announce an audio device. The device will not send audio before this."""
    return _asyn(
        clock_ref,
        A_HPA1,
        serialize_dict({
            "BufferAheadInterval": NSNumber(6, 0.073),
            "deviceUID": "Valeria",
            "ScreenLatency": NSNumber(6, 0.04),
            "formats": audio_stream_basic_description(),
            "EDIDAC3Support": NSNumber(3, 0),
            "deviceName": "Valeria",
        }),
    )


# ---------------------------------------------------------------------------
# CMTime
# ---------------------------------------------------------------------------

def pack_cm_time(value: int = 0, timescale: int = 1_000_000_000,
                 flags: int = 0, epoch: int = 0) -> bytes:
    return struct.pack("<qiIq", value, timescale, flags, epoch)


def parse_cm_time(buf: bytes) -> Dict[str, int]:
    value, timescale, flags, epoch = struct.unpack_from("<qiIq", buf, 0)
    return {"value": value, "timescale": timescale, "flags": flags, "epoch": epoch}


# ---------------------------------------------------------------------------
# CMFormatDescription: dimensions, codec, and the SPS/PPS
# ---------------------------------------------------------------------------

def _take(buf: bytes, magic: int) -> Tuple[bytes, bytes]:
    """Return (record body, rest) for a length/magic block."""
    length, _ = parse_length_magic(buf, magic)
    return buf[8:length], buf[length:]


def parse_format_description(buf: bytes) -> Dict[str, Any]:
    """Pull width, height, codec, SPS and PPS out of an 'fdsc' block."""
    body, _ = _take(buf, FD_FDSC)
    media_body, rest = _take(body, FD_MDIA)
    (media_type,) = struct.unpack_from("<I", media_body, 0)
    out: Dict[str, Any] = {"media_type": media_type}
    if media_type == MEDIA_SOUND:
        return out

    vdim, rest = _take(rest, FD_VDIM)
    out["width"], out["height"] = struct.unpack_from("<II", vdim, 0)
    codc, rest = _take(rest, FD_CODC)
    (out["codec"],) = struct.unpack_from("<I", codc, 0)
    extn, _ = _take(rest, FD_EXTN)
    out["sps"], out["pps"] = extract_parameter_sets(extn)
    return out


def extract_parameter_sets(extn: bytes) -> Tuple[Optional[bytes], Optional[bytes]]:
    """Find SPS and PPS inside the 'extn' dictionary.

    They live under integer key 49, then integer key 105, as an avcC blob.
    Returns (sps, pps), each classified by its NAL header byte -- never by
    position -- because the reference implementation's positional labels are
    swapped relative to the standard avcC layout, and position is exactly the
    kind of assumption that silently breaks on the next iOS.
    """
    blob = _find_int_key(extn, 49)
    if blob is None:
        return None, None
    blob = _find_int_key(blob, 105)
    if blob is None:
        return None, None
    return _parse_avcc(blob)


def _parse_avcc(blob: bytes) -> Tuple[Optional[bytes], Optional[bytes]]:
    """Pull the two parameter-set NALs out of an avcC blob, classified by type.

    Standard avcC: version, profile, compat, level, 0xFF, 0xE1, a big-endian
    uint16 SPS length and the SPS, then a PPS count, a big-endian uint16 PPS
    length and the PPS. The layout the reference implementation reads (a
    one-byte length at offset 7) is tried as a fallback; either way the two
    candidates are told apart by NAL type 7 (SPS) versus 8 (PPS).
    """
    candidates = []
    if len(blob) >= 8 and blob[0] == 1:
        i = 6
        (n,) = struct.unpack_from(">H", blob, i)
        i += 2
        if 0 < n and i + n <= len(blob):
            candidates.append(blob[i:i + n])
            i += n
            if i < len(blob):
                i += 1  # PPS count byte
                if i + 2 <= len(blob):
                    (m,) = struct.unpack_from(">H", blob, i)
                    i += 2
                    if 0 < m and i + m <= len(blob):
                        candidates.append(blob[i:i + m])
    if len(blob) >= 12:
        pps_len = blob[7]
        if 0 < pps_len and 8 + pps_len <= len(blob):
            candidates.append(blob[8:8 + pps_len])
            sps_off = 10 + pps_len
            if sps_off < len(blob):
                sps_len = blob[sps_off]
                if 0 < sps_len and sps_off + 1 + sps_len <= len(blob):
                    candidates.append(blob[sps_off + 1:sps_off + 1 + sps_len])
    sps = pps = None
    for cand in candidates:
        nal_type = cand[0] & 0x1F
        if nal_type == 7 and sps is None:
            sps = cand
        elif nal_type == 8 and pps is None:
            pps = cand
    return sps, pps


def _find_int_key(buf: bytes, want: int) -> Optional[bytes]:
    """Search a dictionary body for integer key `want` and return its value."""
    i = 0
    while i + 8 <= len(buf):
        pair_len, pair_magic = struct.unpack_from("<II", buf, i)
        if pair_magic != D_KEYVAL or pair_len < 8 or i + pair_len > len(buf):
            return None
        pair = buf[i + 8:i + pair_len]
        i += pair_len
        if len(pair) < 8:
            continue
        key_len, key_magic = struct.unpack_from("<II", pair, 0)
        if key_magic != 0x6964786B or key_len < 10:  # "idxk"
            continue
        if len(pair) < key_len:
            continue
        # The key is a uint16 immediately after the magic: a real device sends
        # a 10-byte idxk record (01 00 ... -> key 49 as bytes 31 00). Reading a
        # uint32 at key_len-4 instead -- as this once did -- folds two bytes of
        # the magic into the value, so key 49 was never found, SPS/PPS came
        # back None on every real stream, and the decoder spent the whole
        # session reporting "non-existing PPS 0 referenced".
        (key,) = struct.unpack_from("<H", pair, 8)
        if key != want:
            continue
        rest = pair[key_len:]
        if len(rest) < 8:
            return None
        val_len, _ = struct.unpack_from("<II", rest, 0)
        if val_len > len(rest):
            return None
        return rest[8:val_len]
    return None


# ---------------------------------------------------------------------------
# CMSampleBuffer
# ---------------------------------------------------------------------------

class SampleBuffer:
    """One decoded CMSampleBuffer."""

    def __init__(self) -> None:
        self.presentation_time: Optional[Dict[str, int]] = None
        self.num_samples: Optional[int] = None
        self.sample_sizes: List[int] = []
        self.data: bytes = b""
        self.format: Optional[Dict[str, Any]] = None

    @property
    def has_format(self) -> bool:
        return self.format is not None

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return "SampleBuffer(samples=%r, data=%d bytes, format=%s)" % (
            self.num_samples, len(self.data), bool(self.format))


def parse_sample_buffer(buf: bytes) -> SampleBuffer:
    """Parse the 'sbuf' block inside a FEED or EAT packet.

    Unknown blocks are skipped by their length rather than rejected: the device
    adds fields between iOS versions, and a hard failure there would take the
    stream down over data we do not need.
    """
    out = SampleBuffer()
    body, _ = _take(buf, SB_SBUF)
    i = 0
    while i + 8 <= len(body):
        length, magic = struct.unpack_from("<II", body, i)
        if length < 8 or i + length > len(body):
            break
        payload = body[i + 8:i + length]
        if magic == SB_OPTS:
            out.presentation_time = parse_cm_time(payload)
        elif magic == SB_SDAT:
            out.data = payload
        elif magic == SB_NSMP:
            (out.num_samples,) = struct.unpack_from("<I", payload, 0)
        elif magic == SB_SSIZ:
            out.sample_sizes = [
                struct.unpack_from("<I", payload, n)[0]
                for n in range(0, len(payload) - 3, 4)
            ]
        elif magic == FD_FDSC:
            out.format = parse_format_description(body[i:i + length])
        # stia, satt and sary carry timing and attachment metadata this backend
        # does not use; skipping them is deliberate.
        i += length
    return out


def nalus_to_annexb(data: bytes, sps: Optional[bytes] = None,
                    pps: Optional[bytes] = None) -> bytes:
    """Convert length-prefixed NALUs (AVCC) to Annex-B.

    The device sends 4-byte big-endian length prefixes, the MP4 convention, not
    the little-endian ones the rest of this protocol uses. Parameter sets are
    emitted first when supplied and not already present, because the decoder
    needs them before the first slice and the reference stream does not repeat
    them in every access unit.
    """
    start_code = b"\x00\x00\x00\x01"
    out = bytearray()
    seen_sps = seen_pps = False
    i = 0
    nalus: List[bytes] = []
    while i + 4 <= len(data):
        (nlen,) = struct.unpack_from(">I", data, i)
        if nlen == 0 or i + 4 + nlen > len(data):
            break
        nalus.append(data[i + 4:i + 4 + nlen])
        i += 4 + nlen
    for nalu in nalus:
        nal_type = nalu[0] & 0x1F
        if nal_type == 7:
            seen_sps = True
        elif nal_type == 8:
            seen_pps = True
    if sps and not seen_sps:
        out += start_code + sps
    if pps and not seen_pps:
        out += start_code + pps
    for nalu in nalus:
        out += start_code + nalu
    return bytes(out)


# ---------------------------------------------------------------------------
# USB transport
#
# Nothing below this line has been run against a device. The framing, record
# building and parsing above are covered by tests; this is the part that needs
# a phone on a cable.
# ---------------------------------------------------------------------------

CTRL_ENABLE_AV_CONFIG = (0x40, 0x52, 0, 2)
CTRL_DISABLE_AV_CONFIG = (0x40, 0x52, 0, 0)
CTRL_SET_INTERFACE_HI = (0x02, 0x01, 0, 0x86)
CTRL_SET_INTERFACE_LO = (0x02, 0x01, 0, 0x05)
READ_CHUNK = 1024 * 1024


def need_credits(environ=None) -> int:
    """How many NEED credits to prime at stream start (WIREPLAY_NEED_CREDITS).

    Default 9 is the current experiment; 1 is the configuration that was
    proven on the bench before it. Anything unparsable falls back to the
    default, and the value is clamped so a typo cannot flood the device.
    """
    import os
    env = os.environ if environ is None else environ
    try:
        n = int(env.get("WIREPLAY_NEED_CREDITS", "9"))
    except ValueError:
        n = 9
    return max(1, min(32, n))


READ_TIMEOUT_MS = 3000


class QuickTimeUnavailable(RuntimeError):
    """The capture configuration could not be reached. Message is user-facing."""


def _describe(usb, device) -> str:
    """Everything readable about a candidate, for the log.

    The filter installer puts libusb0 on the Apple composite device *and* on its
    MI_00 and MI_01 instances, so libusb can report the same phone more than
    once. Guessing which entry is the composite parent is how a control request
    ends up aimed at the wrong device, so say what each one actually is.
    """
    try:
        parts = []
        for i in range(device.bNumConfigurations):
            cfg = usb.core.Configuration(device, i)
            ifaces = ["%02x/%02x" % (it.bInterfaceClass, it.bInterfaceSubClass) for it in cfg]
            parts.append("cfg%d[%s]" % (i + 1, ",".join(ifaces) or "empty"))
        return "class=%02x cfgs=%d %s" % (device.bDeviceClass, device.bNumConfigurations,
                                          " ".join(parts))
    except Exception as exc:
        return "unreadable (%s: %s)" % (type(exc).__name__, exc)


def _interface_count(usb, device) -> int:
    """Most interfaces any configuration of this device exposes."""
    best = 0
    try:
        for i in range(device.bNumConfigurations):
            best = max(best, len(list(usb.core.Configuration(device, i))))
    except Exception:
        return 0
    return best


def _find_device(usb, udid: Optional[str] = None, logger=None):
    """Find the iPhone's composite device, not one of its interface instances."""
    def matches(device) -> bool:
        if device.bDeviceClass == USB_INTERFACE_SUBCLASS_MUX:
            return True
        try:
            for cfg in device:
                for intf in cfg:
                    if intf.bInterfaceSubClass == USB_INTERFACE_SUBCLASS_MUX:
                        return True
        except Exception:
            return False
        return False

    found = list(usb.core.find(find_all=True, custom_match=matches) or [])
    if not found:
        raise QuickTimeUnavailable(
            "no iPhone is visible to libusb. Reaching this point means a libusb "
            "backend is present, so if iTunes or 3uTools can see the phone then "
            "the cable and Apple's driver are fine -- what is missing is the "
            "libusb0 filter that lets libusb reach the device alongside Apple's "
            "driver. Run tools\\install_usb_filter.ps1 as administrator with the "
            "phone plugged in and unlocked. Do not use Zadig instead: it replaces "
            "Apple's driver outright and breaks iTunes, the Apple Devices app and "
            "3uTools until it is undone.")

    if logger:
        logger("libusb reports %d matching device(s)" % len(found))
        for n, d in enumerate(found):
            try:
                ident = "%04x:%04x" % (d.idVendor, d.idProduct)
            except Exception:
                ident = "??:??"
            logger("  [%d] %s %s" % (n, ident, _describe(usb, d)))

    if udid:
        for d in found:
            try:
                if udid in (d.serial_number or ""):
                    return d
            except Exception:
                continue
        raise QuickTimeUnavailable("no iPhone with UDID %s on USB" % udid)

    # Prefer the composite parent. Interface instances (MI_00, MI_01) expose a
    # single interface each; the parent exposes both PTP and mux. Only the
    # parent can be switched into the capture configuration.
    ranked = sorted(found, key=lambda d: _interface_count(usb, d), reverse=True)
    if logger and len(found) > 1:
        logger("using the entry with %d interface(s) as the composite parent"
               % _interface_count(usb, ranked[0]))
    return ranked[0]


def _qt_config_exposed(device) -> bool:
    """True while the phone still lists a capture (0x2a) interface anywhere.

    The descriptor set is the ground truth for "is the phone still in
    QuickTime mode": five configurations with an ff/2a interface means yes,
    four without it means the disable took.
    """
    try:
        for i in range(device.bNumConfigurations):
            import usb.core
            cfg = usb.core.Configuration(device, i)
            for intf in cfg:
                if intf.bInterfaceSubClass == USB_INTERFACE_SUBCLASS_AV:
                    return True
    except Exception:
        return False
    return False


class QuickTimeTransport:
    """Drives the hidden capture configuration and the sync/asyn handshake.

    Call open(), then iterate run() -- or drive it from threads as the reference
    does. close_session() tells the device to stop; skipping it can leave the
    device needing a replug.
    """

    def __init__(self, udid: Optional[str] = None, logger=None) -> None:
        self.udid = udid
        self.log = logger
        self._device = None
        self._in_ep = None
        self._out_ep = None
        self._splitter = FrameSplitter()
        self._need_record: bytes = b""
        self._audio_clock_ref: Optional[int] = None
        # First/last (device PTS secs, local monotonic secs) of the audio
        # stream, for the SKEW computation below.
        self._eat_first = None
        self._eat_last = None
        self._need_message: bytes = b""
        self._av_interface_number = None
        self._last_rx_note = 0.0
        self._saw_data = False
        self._start_time: Optional[float] = None
        self.on_video = None   # callable(annexb_bytes, pts_seconds)
        self.on_audio = None   # callable(pcm_bytes)
        self.on_format = None  # callable(width, height, sps, pps)
        self.sps: Optional[bytes] = None
        self.pps: Optional[bytes] = None
        # Pump window statistics: is the ~45 fps ceiling the phone's
        # delivery or our own per-frame processing? chunks and feeds say
        # how the device batches; busy ms/feed says what the Python
        # parse/convert path costs against the frame budget.
        self._win_chunks = 0
        self._win_feeds = 0
        self._win_busy = 0.0
        self._win_wait = 0.0
        self._win_bytes = 0
        self._win_t0 = None
        # NEED-latency tracing (WIREPLAY_TRACE=1, same switch as the
        # arrival tracer in the engine): how long between the read()
        # that carried a FEED returning and our NEED for the next frame
        # actually leaving, how long the write itself takes, how long
        # the device takes to answer a NEED with the next FEED, and how
        # long the per-FEED processing holds the pump. The user-suspect
        # (2026-10-22): the phone only produces on request, so a NEED
        # that leaves late could cost a whole capture slot. Measure,
        # don't argue. Zero cost when the env is off.
        import os as _os
        self._tr = _os.environ.get("WIREPLAY_TRACE", "") not in ("", "0")
        self._tr_read_return = 0.0
        self._tr_last_need = 0.0
        self._tr_n = 0
        self._tr_disp_max = 0.0
        self._tr_wr_max = 0.0
        self._tr_busy_max = 0.0
        self._tr_resp_max = 0.0
        self._tr_slow = 0

    def _info(self, msg: str) -> None:
        if self.log:
            self.log(msg)

    def open(self) -> None:
        import usb.core
        import usb.util

        try:
            device = _find_device(usb, self.udid, self._info)
        except usb.core.NoBackendError:
            # pyusb found no libusb library at all, which is a different problem
            # from "no phone attached" and needs a different fix.
            raise QuickTimeUnavailable(
                "no libusb backend is available to Python. This is not about the "
                "phone: pyusb cannot find a libusb library to talk to. On Windows, "
                "install libusb0.sys as an UpperFilter on the Apple USB device -- "
                "that leaves Apple's driver in place, so iTunes and 3uTools keep "
                "working -- and make sure libusb0.dll is reachable. Do not replace "
                "Apple's driver with WinUSB; that breaks them.")

        # This exact sequence is the one proven on the target phone, repeatedly:
        # disable any leftover capture configuration, take the default
        # configuration, enable capture, re-find the device after it
        # re-enumerates, select the capture configuration, clear both bulk
        # endpoints. A later "tidier" version that skipped the disable/enable
        # cycle when the configuration was already exposed, claimed the
        # interface explicitly and addressed CLEAR_FEATURE by descriptor
        # never got a ping out of the phone -- three failed field tests --
        # while this one streamed for hours. Do not optimise it again without
        # a phone on the bench to prove the replacement.
        self._info("enabling the hidden capture configuration")
        try:
            device.ctrl_transfer(*CTRL_DISABLE_AV_CONFIG, b"")
        except Exception:
            pass
        device.set_configuration()
        result = device.ctrl_transfer(*CTRL_ENABLE_AV_CONFIG, b"")
        if result:
            raise QuickTimeUnavailable(
                "the phone rejected the request to expose its capture "
                "configuration (status %s). That control transfer is what "
                "switches the descriptor set; without it there is no video "
                "interface to claim." % (result,))
        self._info("capture configuration requested; waiting for the phone to re-enumerate")
        import time
        # The 1.5 s is NOT dead time. A polling variant that re-found the
        # device the instant the capture interface appeared in the
        # descriptors -- and selected the configuration immediately --
        # produced dead sessions on the bench (ping but no stream, then
        # not even a ping, 2026-10-20): the phone needs this settle time
        # after re-enumerating before its configuration is selected. Do
        # not "optimise" this wait without a per-variant probe on a real
        # phone proving the replacement, one process per variant.
        time.sleep(1.5)
        # The device drops off the bus and comes back with the extra
        # configuration, so it has to be re-found rather than reused.
        device = None
        for _ in range(10):
            try:
                device = _find_device(usb, self.udid, self._info)
                break
            except QuickTimeUnavailable:
                time.sleep(0.5)
        if device is None:
            raise QuickTimeUnavailable(
                "the iPhone did not come back after the capture configuration "
                "was enabled. Unplug it and plug it in again.")

        # bConfigurationValue is what set_configuration() takes. Using the
        # descriptor index + 1 (as this once did) only works while the phone
        # happens to number its configurations 1..N in descriptor order --
        # true on the bench phone, guaranteed nowhere. Read the real value.
        config_index = None
        av_interface = None
        for i in range(device.bNumConfigurations):
            cfg = usb.core.Configuration(device, i)
            for intf in cfg:
                if intf.bInterfaceSubClass == USB_INTERFACE_SUBCLASS_AV:
                    config_index = cfg.bConfigurationValue
                    av_interface = intf
                    break
            if config_index is not None:
                break
        if config_index is None or av_interface is None:
            raise QuickTimeUnavailable(
                "the iPhone did not expose the capture interface (interface "
                "subclass 0x2a) after the configuration switch. The log above "
                "lists every configuration and interface libusb could read, so "
                "it says whether the switch took effect at all.")

        self._av_interface_number = av_interface.bInterfaceNumber
        device.set_configuration(config_index)
        device.ctrl_transfer(*CTRL_SET_INTERFACE_HI, b"")
        device.ctrl_transfer(*CTRL_SET_INTERFACE_LO, b"")

        for ep in av_interface:
            if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
                self._in_ep = ep
            else:
                self._out_ep = ep
        if self._in_ep is None or self._out_ep is None:
            raise QuickTimeUnavailable("the capture interface has no bulk endpoint pair")

        self._device = device
        self._info("capture endpoints: interface %d, in=0x%02x, out=0x%02x" % (
            self._av_interface_number, self._in_ep.bEndpointAddress,
            self._out_ep.bEndpointAddress))
        self._info("capture configuration active, waiting for the handshake")

    def _write(self, data: bytes) -> None:
        self._write_to(self._device, data)

    def _write_to(self, device, data: bytes) -> None:
        device.write(self._out_ep, data, 100)

    def pump(self) -> None:
        """Read one chunk from the device and handle every complete record in it."""
        import time
        import usb.core
        _t0 = time.monotonic()
        try:
            raw = self._device.read(self._in_ep, READ_CHUNK, READ_TIMEOUT_MS)
        except usb.core.USBTimeoutError:
            self._note_silence()
            return  # idle bus; the device speaks again when it is ready
        except usb.core.USBError as exc:
            # The libusb0 backend reports a read timeout as a plain USBError
            # ("libusb0-dll:err [_usb_reap_async] timeout error", errno None),
            # not as USBTimeoutError -- pyusb only translates the libusb-1.0
            # errno. Treating it as fatal killed healthy sessions 3 s after
            # the ping whenever the phone paused before the handshake.
            if "timeout" in str(exc).lower():
                self._note_silence()
                return
            raise
        _t1 = time.monotonic()
        if self._tr:
            self._tr_read_return = _t1
        if not self._saw_data:
            self._saw_data = True
            self._info("first data from the phone: %d bytes" % len(raw))
        self._win_chunks += 1
        self._win_wait += _t1 - _t0
        for body in self._splitter.feed(bytes(raw)):
            try:
                self._handle(body)
            except struct.error as exc:
                # A record whose payload is shorter than the handler expects.
                # Naming the bytes is what turns this from a dead end into a
                # readable answer about what the phone actually sent.
                raise QuickTimeUnavailable(
                    "the phone sent a record this app could not unpack (%s): "
                    "%d bytes, %s" % (exc, len(body), body[:64].hex())) from exc
        self._win_busy += time.monotonic() - _t1

    def _note_silence(self) -> None:
        """Say, out loud, that we are still waiting for the first packet.

        A phone that never pings and a phone that died look identical in a
        silent log; this line is the difference between "keep waiting, check
        the phone is unlocked" and a user staring at a frozen terminal.
        """
        import time
        now = time.monotonic()
        if now - self._last_rx_note >= 10.0:
            self._last_rx_note = now
            self._info("still waiting for the phone to start sending "
                       "(no data yet -- it must be awake and unlocked)")

    def _handle(self, body: bytes) -> None:
        magic, clock_ref, msg_type, payload = parse_header(body)
        if magic == PING_MAGIC:
            self._info("device pinged; the stream is starting")
            self._write(ping_reply())
        elif magic == SYNC_MAGIC:
            self._handle_sync(clock_ref, msg_type, payload)
        elif magic == ASYN_MAGIC:
            self._handle_asyn(msg_type, payload)

    def _handle_sync(self, clock_ref: int, msg_type: int, payload: bytes) -> None:
        (correlation_id,) = struct.unpack_from("<Q", payload, 0)
        rest = payload[8:]
        if msg_type == S_CLOK:
            self._write(reply_clock(correlation_id, clock_ref + 0x10000))
        elif msg_type == S_CVRP:
            # This is the reply that starts video. The device clock reference
            # from the payload is what NEED has to carry.
            (device_clock,) = struct.unpack_from("<Q", rest, 0)
            self._need_message = asyn_need(device_clock)
            self._write(self._need_message)
            # Frame credits primed at stream start, then one more NEED per
            # FEED as before. The default (9) is the current experiment: if
            # the device is credit-driven, a deep pool takes our round trip
            # out of its capture cadence. The proven configuration was ONE
            # credit; WIREPLAY_NEED_CREDITS=1 (or 2) restores it for an A/B
            # without editing code. The message carries no counter, so if
            # the device treats NEED as a level, the extras are no-ops.
            credits = need_credits()
            for _ in range(credits - 1):
                self._write(self._need_message)
            self._info("NEED credits primed: %d" % credits)
            self._write(reply_clock(correlation_id, device_clock + 0x1000AF))
        elif msg_type == S_CWPA:
            (device_clock,) = struct.unpack_from("<Q", rest, 0)
            self._audio_clock_ref = device_clock
            self._write(asyn_hpd1())
            self._write(reply_clock(correlation_id, device_clock + 1000))
            self._write(asyn_hpd1())
            self._write(asyn_hpa1(device_clock))
        elif msg_type == S_OG:
            self._write(reply_og(correlation_id))
        elif msg_type == S_AFMT:
            self._write(reply_afmt(correlation_id))
        elif msg_type == S_TIME:
            import time
            if self._start_time is None:
                self._start_time = time.monotonic()
            elapsed = time.monotonic() - self._start_time
            # flags=1 (kCMTimeFlagsHasBeenRounded): what the reference
            # sends on the wire, verified against a captured QuickTime
            # session. flags=0 marks the host clock INVALID as far as
            # the device's CoreMedia is concerned -- and the device
            # schedules frame delivery against this clock.
            self._write(reply_time(correlation_id,
                                   pack_cm_time(int(elapsed * 1e9), flags=1)))
        elif msg_type == S_SKEW:
            self._write(reply_skew(correlation_id, self._clock_skew()))
        elif msg_type == S_STOP:
            self._write(reply_stop(correlation_id))

    def _clock_skew(self) -> float:
        """Rate of the device audio clock as seen by the host, in Hz.

        Ported from the reference: skew = device timescale x
        (host elapsed / device elapsed) over the audio stream's first
        and last sample timestamps, which lands within a unit or two
        of 48000 on a healthy session. We used to answer 0.0 -- "the
        host clock does not run" -- which the device's clock-sync
        machinery cannot schedule against. Falls back to the nominal
        48000 before audio has flowed long enough to measure.
        """
        if self._eat_first and self._eat_last:
            dev0, host0 = self._eat_first
            dev1, host1 = self._eat_last
            dev_elapsed = dev1 - dev0
            host_elapsed = host1 - host0
            if dev_elapsed > 0.05 and host_elapsed > 0.0:
                return 48000.0 * host_elapsed / dev_elapsed
        return 48000.0

    def _handle_asyn(self, msg_type: int, payload: bytes) -> None:
        if msg_type not in (A_FEED, A_EAT):
            # ASYN types we do not consume (TJMP/SRAT/SPRP/TBAS/...)
            # carry the device's timebase announcements. Log the
            # first sighting of each: if the device declares a rate
            # other than 1.0 (SRAT) or jumps its timebase (TJMP),
            # that is device-side ground truth about the cadence
            # and must not vanish silently.
            seen = getattr(self, "_asyn_seen", None)
            if seen is None:
                seen = self._asyn_seen = set()
            if msg_type not in seen:
                seen.add(msg_type)
                self._info("device ASYN 0x%08x len=%d head=%s" % (
                    msg_type, len(payload), payload[:24].hex()))
            return
        _tr_t0 = 0.0
        if self._tr and msg_type == A_FEED:
            import time as _tt
            _tr_t0 = _tt.monotonic()
        if msg_type == A_FEED and self._need_message:
            # Ask for the next frame FIRST, parse afterwards. The device
            # only produces on request, and its capture clock ticks at the
            # display refresh: a NEED that leaves after the sample-buffer
            # parse + forward (milliseconds of Python) can miss the slot
            # and cost a whole 16.7 ms cycle -- at 60 Hz cadence, losing
            # one slot in four is exactly a ~45 fps average with a bimodal
            # 16.7/33.3 ms gap distribution, which is what the field logs
            # show. One NEED per FEED, as before -- just early.
            if self._tr:
                import time as _tt
                _wr0 = _tt.monotonic()
                self._write(self._need_message)
                _wr1 = _tt.monotonic()
                _disp = (_tr_t0 - self._tr_read_return) * 1000.0
                _wr = (_wr1 - _wr0) * 1000.0
                if _disp > self._tr_disp_max:
                    self._tr_disp_max = _disp
                if _wr > self._tr_wr_max:
                    self._tr_wr_max = _wr
                _resp = ((_tr_t0 - self._tr_last_need) * 1000.0
                         if self._tr_last_need else -1.0)
                if _resp > self._tr_resp_max:
                    self._tr_resp_max = _resp
                self._tr_last_need = _wr1
                if (_wr1 - self._tr_read_return) * 1000.0 > 8.0:
                    self._tr_slow += 1
                    self._info("[trace] slow NEED: out %.1f ms after read "
                               "return (dispatch %.1f + write %.1f), device "
                               "answered the previous NEED in %.1f ms"
                               % ((_wr1 - self._tr_read_return) * 1000.0,
                                  _disp, _wr, _resp))
            else:
                self._write(self._need_message)
        buf = parse_sample_buffer(payload)
        if buf.has_format and buf.format:
            fmt = buf.format
            if not getattr(self, "_format_logged", False):
                self._format_logged = True
                summary = {
                    k: (("<%d bytes>" % len(v)) if isinstance(v, (bytes, bytearray)) else v)
                    for k, v in fmt.items()
                }
                self._info("device format description: %r" % (summary,))
            if fmt.get("media_type") == MEDIA_VIDEO:
                self.sps, self.pps = fmt.get("sps"), fmt.get("pps")
                if self.on_format:
                    self.on_format(fmt.get("width"), fmt.get("height"), self.sps, self.pps)
            # No return here: on a real device the format description rides in
            # the same sample buffer as the first IDR. Returning early used to
            # throw that access unit away.
        if not buf.data:
            return
        pts = None
        if buf.presentation_time and buf.presentation_time.get("timescale"):
            pts = buf.presentation_time["value"] / buf.presentation_time["timescale"]
        if msg_type == A_EAT and buf.presentation_time and \
                buf.presentation_time.get("timescale"):
            import time as _time
            dev_secs = (buf.presentation_time["value"] /
                        buf.presentation_time["timescale"])
            point = (dev_secs, _time.monotonic())
            if self._eat_first is None:
                self._eat_first = point
            self._eat_last = point
        if msg_type == A_FEED:
            import time as _time
            if self._win_t0 is None:
                self._win_t0 = _time.monotonic()
            self._win_feeds += 1
            self._win_bytes += len(buf.data)
            if self._win_feeds >= 600:
                chunks = max(1, self._win_chunks)
                secs = max(0.001, _time.monotonic() - (self._win_t0 or 0.0))
                self._info(
                    "[usb] feeds/chunk=%.2f pump_busy=%.2f ms/feed "
                    "read_wait=%.0f%% of window bitrate=%.1f Mbps" % (
                        self._win_feeds / chunks,
                        1000.0 * self._win_busy / self._win_feeds,
                        100.0 * self._win_wait /
                        max(0.001, self._win_wait + self._win_busy),
                        8.0 * self._win_bytes / secs / 1e6))
                self._win_chunks = self._win_feeds = 0
                self._win_busy = self._win_wait = 0.0
                self._win_bytes = 0
                self._win_t0 = None
            if self.on_video:
                self.on_video(nalus_to_annexb(buf.data, self.sps, self.pps), pts)
            if self._tr and _tr_t0:
                import time as _tt
                _busy = (_tt.monotonic() - _tr_t0) * 1000.0
                if _busy > self._tr_busy_max: self._tr_busy_max = _busy
                self._tr_n += 1
                if self._tr_n % 600 == 0:
                    self._info(
                        "[trace] need window: %d FEEDs, dispatch max %.2f ms, "
                        "write max %.2f ms, feed-busy max %.2f ms, device "
                        "NEED->FEED max %.1f ms, slow NEEDs %d"
                        % (self._tr_n, self._tr_disp_max, self._tr_wr_max,
                           self._tr_busy_max, self._tr_resp_max, self._tr_slow))
                    self._tr_disp_max = self._tr_wr_max = 0.0
                    self._tr_busy_max = self._tr_resp_max = 0.0
                    self._tr_slow = 0
        elif self.on_audio:
            self.on_audio(buf.data)

    def close_session(self) -> None:
        """End the session and give the phone its normal USB personality back.

        Mirrors the reference teardown, in order: HPA0 stops audio, HPD0 asks
        the device to release the session, we then *wait for the device to
        answer* (it replies with release messages), HPD0 goes out a second
        time, and only then does the vendor request disable the capture
        configuration.

        The disable is verified, not assumed. The reference's own disable
        helper loops the request (theirs does it twenty times): a single
        polite request does not always stick, and when it does not, the phone
        keeps exposing the capture configuration and presenting as a dock --
        with its audio routed to the cable -- until it is unplugged, which is
        exactly the field report that motivated this. So: send the disable,
        re-enumerate, and look at whether a capture interface is still in the
        descriptor set; if it is, ask again on the fresh device handle. Every
        step logs its failures instead of swallowing them, because "closing
        the window did not release the phone" is undiagnosable otherwise.
        """
        if self._device is None:
            return
        import time
        _close_t0 = time.monotonic()
        self._info("closing the capture session ...")
        device, self._device = self._device, None
        if self._out_ep is not None:
            try:
                if self._audio_clock_ref is not None:
                    self._write_to(device, asyn_hpa0(self._audio_clock_ref))
                self._write_to(device, asyn_hpd0())
            except Exception as exc:
                self._info("session stop messages failed: %s" % exc)
            if self._in_ep is not None:
                # Wait for the release exchange, but stop waiting once
                # the device has answered and gone quiet: the old loop
                # always burned the full 1.2 s even when the replies
                # landed in the first 50 ms, and undocking felt slow.
                # Silence BEFORE any answer still waits out the cap --
                # breaking on the first timeout (as this once did)
                # abandoned the release wait entirely.
                deadline = time.monotonic() + 1.2
                answered = False
                while time.monotonic() < deadline:
                    try:
                        data = device.read(self._in_ep, 4096, 250)
                        if data:
                            answered = True
                    except Exception as exc:
                        if "timeout" in str(exc).lower() or \
                                type(exc).__name__ == "USBTimeoutError":
                            if answered:
                                break  # replies arrived, then silence: done
                            continue
                        self._info("release wait ended early: %s" % exc)
                        break
            try:
                self._write_to(device, asyn_hpd0())
            except Exception as exc:
                self._info("second session stop message failed: %s" % exc)

        released = False
        import time
        import usb as _usb_pkg
        import usb.util

        def _dispose_opened(opened_device, release_av=False):
            # Only ever called on a handle this code opened: the session
            # device, or a re-enumerated one we just sent a control
            # transfer on (a ctrl transfer is what opens a pyusb device).
            # Disposing a never-opened device is the pattern that
            # bugchecked Windows (WDF_VIOLATION, param 1 = 5) -- see the
            # regression test. Enumeration results that were only looked
            # at are dropped, never disposed.
            try:
                if release_av and self._av_interface_number is not None:
                    usb.util.release_interface(
                        opened_device, self._av_interface_number)
                usb.util.dispose_resources(opened_device)
            except Exception:
                pass

        def _poll_released(seconds):
            deadline = time.monotonic() + seconds
            while True:
                try:
                    fresh = _find_device(_usb_pkg, self.udid)
                except Exception:
                    return True  # gone mid-re-enumeration: it flipped
                if not _qt_config_exposed(fresh):
                    return True
                if time.monotonic() >= deadline:
                    return False
                time.sleep(0.05)

        # Field evidence (2026-10-20): three disable requests in a row
        # each reported "still exposes the capture configuration", yet
        # the phone let go the moment this process exited -- i.e. when
        # the session handle was finally disposed. While the handle that
        # selected the capture configuration stays open, the phone keeps
        # that configuration active no matter how often it is asked. So:
        # ask once, let go of the handle, THEN check.
        current = device
        # Ask again sooner rather than politely waiting out each poll:
        # field runs show the phone flipping anywhere from the 1st to the
        # 3rd request, ~2 s apart -- six asks at 0.75 s polls reach the
        # stubborn case sooner and keep the same worst-case total.
        for attempt in range(6):
            try:
                # Explicit 1.5 s timeout: a control transfer with pyusb's
                # default (no) timeout can park the teardown forever if
                # the phone is mid-reset, and the app only waits 6 s
                # before killing the engine.
                current.ctrl_transfer(*CTRL_DISABLE_AV_CONFIG, b"", 1500)
                self._info("disable request %d sent" % (attempt + 1))
            except Exception as exc:
                self._info("disable request %d failed: %s" % (attempt + 1, exc))
            _dispose_opened(current, release_av=(attempt == 0))
            if _poll_released(0.75):
                released = True
                break
            if attempt == 5:
                break
            self._info("phone still exposes the capture configuration "
                       "after disable %d; asking again" % (attempt + 1))
            try:
                current = _find_device(_usb_pkg, self.udid)
            except Exception:
                released = True
                break
        self._info("teardown timing: total %.2f s" % (time.monotonic() - _close_t0))
        if released:
            self._info("capture session closed; the iPhone was released")
        else:
            self._info("WARNING: the iPhone still exposes its capture "
                       "configuration; it may keep presenting as a dock "
                       "until the cable is unplugged")

