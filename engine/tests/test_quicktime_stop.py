"""Regression: closing the app window must reach close_session().

Field report: the stream ran fine, the app logged "app sent kEnd", and then
nothing -- no teardown lines at all, and the phone kept presenting as a
dock until the cable was pulled.

Cause: when the window closes, the app stops reading the engine's stdout
pipe. The QuickTime callbacks kept writing video/audio frames into that
unread pipe; a write into a full pipe blocks, so the pump thread parked
inside Sink.send, never noticed the stop signal, and close_session() --
the only code that tells the phone to release -- never ran. The app then
killed the engine after its grace period.

These tests pin the three guards that fix it, in engine source order:
the stop event exists before the callbacks, the callbacks drop frames
once it is set, and transport logging writes to stderr first and skips
the pipe entirely while stopping.
"""
import pathlib

SRC = (
    pathlib.Path(__file__).resolve().parent.parent / "wireplay_engine.py"
).read_text()


def _quicktime_section() -> str:
    start = SRC.index("def _run_quicktime")
    end = SRC.index("def _first_nal_type")
    return SRC[start:end]


def test_stop_event_predates_the_callbacks():
    section = _quicktime_section()
    assert section.index("stop_requested = threading.Event()") < section.index(
        "def on_video"
    )


def test_callbacks_drop_frames_once_stopping():
    section = _quicktime_section()
    for callback in ("def on_video", "def on_audio"):
        body = section[section.index(callback):]
        # The callback checks the stop flag, then hands the frame to the
        # writer queue -- it must never write to the pipe itself, because
        # a write into an unread pipe blocks the pump thread forever.
        assert body.index("stop_requested.is_set()") < body.index("_enqueue"), callback
        assert "sink.send" not in body.split("def ")[1], callback


def test_only_the_writer_thread_touches_the_pipe():
    section = _quicktime_section()
    writer = section[section.index("def _frame_writer"):]
    assert "sink.send(mtype, payload)" in writer.split("def ")[1]
    assert "_frame_queue.put_priority" in section
    assert "_frame_queue.put_video" in section


def test_queue_module_is_imported():
    # The writer queue is built at runtime inside _run_quicktime; a missing
    # import only explodes when a real device session starts, which the
    # text-level tests above cannot see. It happened. Pin it.
    assert "\nimport queue\n" in SRC


def test_transport_log_never_blocks_teardown_on_the_pipe():
    section = _quicktime_section()
    body = section[section.index("def _transport_log"):]
    # stderr first, then the pipe only while the app is still reading.
    assert body.index('logger.info("[quicktime] %s", m)') < body.index(
        'sink.status("quicktime", m)'
    )
    assert "if stop_requested.is_set():" in body.split("sink.status")[0]


def test_teardown_sets_stop_before_closing_the_session():
    section = _quicktime_section()
    assert section.index("stop_requested.set()") < section.index(
        "transport.close_session()"
    )


def test_frame_queue_is_shallow():
    # A deep queue is a latency reservoir: at ~45 fps, 60 frames is over
    # a second of standing buffer, field-measured as 500-750 ms of
    # touch-to-PC delay. Live mirroring drops excess instead.
    assert "LiveFrameQueue(maxsize=16)" in SRC


def _drain(q):
    out = []
    while len(q):
        out.append(q.get())
    return out


def test_au_has_idr_requires_an_idr_slice_not_just_sps():
    import wireplay_engine as eng
    sc = b"\x00\x00\x00\x01"
    # The field failure in miniature: SPS/PPS prepended to a P-frame AU
    # (parameter sets lead, the slice is type 1) must NOT read as a
    # keyframe; only a type-5 slice qualifies.
    assert eng._au_has_idr(sc + b"\x67\x42" + sc + b"\x68\xce" + sc + b"\x41\x9a") is False
    assert eng._au_has_idr(sc + b"\x67\x42" + sc + b"\x65\x88") is True
    assert eng._au_has_idr(sc + b"\x41\x9a") is False
    assert eng._au_has_idr(b"") is False


def test_live_queue_never_drops_audio_init():
    import wireplay_engine as eng
    q = eng.LiveFrameQueue(maxsize=6)
    for i in range(6):
        q.put_video(b"v%d" % i, is_key=(i == 0))
    q.put_priority(eng.T_ENGINE_AUDIO_PCM_INIT, b"init")
    items = _drain(q)
    assert (eng.T_ENGINE_AUDIO_PCM_INIT, b"init") in items
    # Priority has its own lane now: nothing is evicted for audio, and
    # the audio frame is delivered ahead of the queued video backlog.
    assert len(items) == 7
    assert items[0] == (eng.T_ENGINE_AUDIO_PCM_INIT, b"init")
    assert q.drops == 0


def test_live_queue_drop_mode_until_keyframe():
    import wireplay_engine as eng
    q = eng.LiveFrameQueue(maxsize=6)
    for i in range(6):
        q.put_video(b"v%d" % i, is_key=(i == 0))
    q.put_video(b"p-late", is_key=False)   # full: dropped, arms drop mode
    q.put_video(b"p-later", is_key=False)  # drop mode: dropped, no queueing
    q.put_video(b"KEY", is_key=True)       # keyframe: queued, mode cleared
    q.put_video(b"p-after", is_key=False)  # flows again (evicts if needed)
    payloads = [pl for _t, pl in _drain(q)]
    assert b"p-late" not in payloads and b"p-later" not in payloads
    assert b"KEY" in payloads
    assert payloads.index(b"KEY") < len(payloads)  # sanity
    assert q.drops >= 2
