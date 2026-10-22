# WirePlay — engine ⇄ app wire protocol

Version `1`. Framing is identical in both directions: one message per frame,
length-prefixed, on a binary pipe.

```
+-------------------+---------+--------------------------+
| len  (4 bytes BE) | type(1) | payload (len-1 bytes)    |
+-------------------+---------+--------------------------+
```

`len` counts the **type byte plus the payload**, so the smallest legal frame is
`00 00 00 01 02` (a bare `kEnd`). Frames longer than 64 MiB are a protocol error.

The engine owns **stdout** for media messages and **stderr** for human-readable
logs. The app writes commands to the engine's **stdin**. Neither side may write
anything that is not a frame to those descriptors.

## Engine → app (`stdout`)

| Type | Name | Payload | Meaning |
|------|------|---------|---------|
| `0x01` | `kStatus` | UTF-8 JSON | Lifecycle / state transition. See [status](#kstatus-0x01). |
| `0x02` | `kFatal` | UTF-8 JSON `{"code": int, "message": str}` | Unrecoverable. Engine exits right after. |
| `0x04` | `kStats` | UTF-8 JSON | Periodic telemetry, ~1 Hz. |
| `0x08` | `kDevice` | UTF-8 JSON | Device identity + native screen geometry. Emitted once after connect, again on rotation. |
| `0x10` | `kVideoInit` | `[u32be width][u32be height][u32be flags][VPS][SPS][PPS]` | Parameter sets. `flags` bit0 = LTRP enabled. Re-emitted when they change. |
| `0x11` | `kVideoAU` | `[u32be rtp_ts][u8 key][u8 flags][Annex-B NALs]` | One access unit, HEVC Annex-B. |
| `0x20` | `kAudioInit` | `[u32be rate][u8 channels][u16be asc_len][ASC]` | AAC AudioSpecificConfig. Always `48000 / 2 / F8 E6 40 00` on current iOS. |
| `0x21` | `kAudioAU` | `[u32be rtp_ts][AAC-ELD AU]` | One audio access unit, 480 samples @48 kHz stereo. |
| `0x22` | `kAudioPcmInit` | `[u32be rate][u8 channels]` | Raw-PCM audio (QuickTime backend): no AudioSpecificConfig exists to send, so this replaces `kAudioInit`; the AUs still travel as `kAudioAU` carrying s16 interleaved samples. |
| `0xFE` | `kPing` | `[u64be token]` | Echoed back by the app as `kPong`. |
| `0xFF` | `kEnd` | *(empty)* | Clean shutdown. |

`kVideoAU.flags`: bit0 = decoder must be flushed/reset before this AU (sent after
a dropped AU invalidated the reference chain). `kVideoAU.key`: `1` for IDR/CRA.

`rtp_ts` is the device's 24 kHz video media clock (audio uses its own 48 kHz
clock); both are only meaningful as deltas within their own stream.

### `kStatus` (0x01)

```json
{"state": "tunneld_wait", "detail": "waiting for pymobiledevice3 remote tunneld on 127.0.0.1:49151"}
```

`state` is one of:

| state | meaning |
|-------|---------|
| `starting` | engine booted, no work done yet |
| `tunneld_wait` | waiting for `pymobiledevice3 remote tunneld` to come up |
| `tunneld_up` | tunneld reachable, no device tunnel yet |
| `device_wait` | tunneld up, no device paired over USB |
| `ddi_wait` | device present, mounting the DeveloperDiskImage / cryptex |
| `rsd_connect` | opening RemoteXPC service discovery |
| `stream_start` | negotiating the DisplayService media stream |
| `streaming` | first video AU delivered; media is flowing |
| `reconnecting` | tunnel or stream died, retrying |
| `stopped` | shutting down cleanly |

The app may render `detail` verbatim in its status line.

### `kDevice` (0x08)

```json
{
  "udid": "00008120-001A2C3D4E5F6789",
  "name": "iPhone de Fulano",
  "product": "iPhone15,3",
  "os": "18.5",
  "screen": {"width": 1290, "height": 2796},
  "orientation": "portrait",
  "transport": "usb"
}
```

### `kStats` (0x04)

```json
{
  "video": {"fps": 59.9, "kbps": 6144, "lost_aus": 0, "reorders": 2, "gaps": 0,
             "idrs": 12, "width": 1290, "height": 2796, "ltrp": true},
  "audio": {"pps": 100, "kbps": 128, "dropped": 0},
  "link":  {"rtp_packets": 184220, "rtcp_sent": 61, "jitter_ms": 0.4}
}
```

## App → engine (`stdin`)

| Type | Name | Payload | Meaning |
|------|------|---------|---------|
| `0x81` | `kPong` | `[u64be token]` | Reply to `kPing`. Three misses ⇒ engine tears the stream down and restarts it. |
| `0x82` | `kRequestKey` | *(empty)* | Force an IDR (PLI) now. Sent by the app after a decode error or a resize. |
| `0x83` | `kRotate` | `[i8 dir]` | `-1` left, `+1` right. |
| `0x84` | `kRestartStream` | *(empty)* | Stop and re-negotiate the media stream (recovers a wedged encoder). |
| `0x85` | `kTouch` | `[u8 down][u16be x][u16be y]` | `x`,`y` normalised 0..65535. |
| `0x86` | `kButton` | UTF-8 name | One of `home lock volume-up volume-down mute siri`. |
| `0x87` | `kSetBitrate` | `[u32be kbps]` | Requires the RCTL loop; ignored otherwise. |
| `0xFE` | `kPing` | `[u64be token]` | |
| `0xFF` | `kEnd` | *(empty)* | App is going away; engine must exit. |

## Why the engine is a separate process

`pymobiledevice3` is GPL-3.0. Running it as a child process over a pipe keeps the
GPL boundary at the process edge, so the C++ application can stay MIT while still
shipping the engine inside the installer. Bundling it as a library would force the
whole application to GPL. See `docs/LEGAL.md`.

## Constant audit (de-Pythonization step 3, 2026-10-22)

Every frame type, codec id and size limit exists in exactly two places --
`src/wp/protocol.hpp` (C++) and the header block of
`engine/wireplay_engine.py` (Python) -- because the two processes are
separate programs in separate languages; that duplication is inherent,
not accidental. It is pinned mechanically by
`engine/tests/test_wire_protocol.py`
(`test_every_engine_constant_matches_the_header`,
`test_no_duplicate_type_values_within_a_direction`,
`test_video_codec_enum_matches_the_header`): change a value on one side
only and the suite goes red. This document is the normative reference;
the constants audited identical on 2026-10-22, including
`MAX_FRAME == kMaxFrame == 64 MiB`. A code generator was considered and
rejected: it would add a build step to protect against a drift the tests
already make impossible.
