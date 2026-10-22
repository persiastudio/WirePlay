"""wp_trace: the arrival tracer that localises periodic stalls."""
import math

import wp_trace


def _lines():
    out = []
    return out, out.append


def test_enabled_only_for_a_real_value():
    assert not wp_trace.enabled({})
    assert not wp_trace.enabled({"WIREPLAY_TRACE": ""})
    assert not wp_trace.enabled({"WIREPLAY_TRACE": "0"})
    assert wp_trace.enabled({"WIREPLAY_TRACE": "1"})


def test_smooth_stream_logs_no_gaps():
    out, log = _lines()
    t = wp_trace.ArrivalTracer(log)
    for i in range(120):
        t.video(i / 60.0, 12000, False, 1)
    assert not [l for l in out if "usb gap" in l]


def test_a_gap_is_logged_with_the_period_since_the_previous_gap():
    out, log = _lines()
    t = wp_trace.ArrivalTracer(log)
    now = 0.0
    # 60 fps, with a 100 ms hole after every 10th AU (~170 ms period).
    for i in range(40):
        now += 0.100 if i % 10 == 9 else 1 / 60.0
        t.video(now, 12000, False, 2)
    gaps = [l for l in out if "usb gap" in l]
    assert len(gaps) == 4
    assert "100.0 ms" in gaps[1]
    assert "after the previous gap" in gaps[1]
    # consecutive holes are 10 AUs apart: 9 normal intervals + the 100 ms hole
    assert "250 ms after the previous gap" in gaps[1]


def test_idr_cadence_and_size_land_in_the_summary():
    out, log = _lines()
    t = wp_trace.ArrivalTracer(log)
    now = 0.0
    for i in range(60):
        now += 1 / 60.0
        key = i % 10 == 0
        t.video(now, 200_000 if key else 20_000, key, 0)
    s = t.summary()
    assert "6 IDRs" in s
    assert "avg 195 KB" in s
    assert "every 167 ms" in s
    assert "non-IDR avg 19.5 KB" in s
    # the window resets
    assert "0 IDRs" in t.summary()


def test_idr_lines_are_capped():
    out, log = _lines()
    t = wp_trace.ArrivalTracer(log)
    now = 0.0
    for _ in range(100):
        now += 0.05
        t.video(now, 1000, True, 0)
    assert len([l for l in out if "IDR" in l and "after the previous" in l]) == wp_trace.MAX_IDR_LINES


def test_pipe_write_only_logs_slow_writes():
    out, log = _lines()
    t = wp_trace.ArrivalTracer(log)
    t.pipe_write(0.002, 0x11, 60000, 1)
    assert not out
    t.pipe_write(0.080, 0x11, 60000, 5)
    assert out and "blocked 80.0 ms" in out[0]
