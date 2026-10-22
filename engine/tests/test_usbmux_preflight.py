"""The usbmux preflight: the phone will not start a QuickTime session while
Apple's usbmux service is down, so the engine must check loudly instead of
hanging in silence. Proven on real hardware; see _ensure_usbmux."""
import sys

import wireplay_engine as eng


def test_preflight_passes_immediately_when_usbmux_answers(monkeypatch):
    monkeypatch.setattr(eng.sys, "platform", "win32")
    monkeypatch.setattr(eng, "_usbmux_reachable", lambda timeout=2.0: True)
    notes = []
    assert eng._ensure_usbmux(notes.append) is True
    assert notes == []  # nothing to say when all is well


def test_preflight_starts_the_service_and_waits_for_it(monkeypatch):
    monkeypatch.setattr(eng.sys, "platform", "win32")
    state = {"answers": False, "started": False}

    def reachable(timeout=2.0):
        return state["answers"]

    def start():
        state["started"] = True
        state["answers"] = True

    monkeypatch.setattr(eng, "_usbmux_reachable", reachable)
    monkeypatch.setattr(eng, "_try_start_apple_service", start)
    notes = []
    assert eng._ensure_usbmux(notes.append, wait_seconds=2.0) is True
    assert state["started"] is True
    assert any("not responding" in n for n in notes)


def test_preflight_gives_up_and_says_so(monkeypatch):
    monkeypatch.setattr(eng.sys, "platform", "win32")
    monkeypatch.setattr(eng, "_usbmux_reachable", lambda timeout=2.0: False)
    monkeypatch.setattr(eng, "_try_start_apple_service", lambda: None)
    assert eng._ensure_usbmux(lambda m: None, wait_seconds=0.2) is False


def test_preflight_is_a_no_op_off_windows(monkeypatch):
    monkeypatch.setattr(eng.sys, "platform", "linux")
    monkeypatch.setattr(eng, "_usbmux_reachable",
                        lambda timeout=2.0: (_ for _ in ()).throw(AssertionError))
    assert eng._ensure_usbmux(lambda m: None) is True


def test_reachable_checks_the_real_usbmux_port():
    assert eng.USBMUX_ADDRESS == ("127.0.0.1", 27015)
    # Nothing is listening on the sandbox's port, so this must be False --
    # it proves the helper really opens a socket rather than assuming.
    if sys.platform != "win32":
        assert eng._usbmux_reachable(timeout=0.5) is False
