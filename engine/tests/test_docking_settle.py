"""Regression: Docking must let the phone settle after re-enumeration.

The proven sequence sleeps 1.5 s after the enable request, then re-finds
the device once. A "faster" polling variant that selected the capture
configuration the instant it appeared in the descriptors produced dead
sessions on the real phone (ping but no stream, then not even a ping,
2026-10-20). The 1.5 s is settle time, not dead time. Do not remove it
without a per-variant probe on hardware proving the replacement.
"""
import pathlib

SRC = (
    pathlib.Path(__file__).resolve().parent.parent / "quicktime_backend.py"
).read_text()


def test_docking_keeps_the_settle_sleep():
    assert "time.sleep(1.5)" in SRC


def test_docking_has_no_instant_polling_variant():
    assert "_locate_av" not in SRC


BACKEND = (
    pathlib.Path(__file__).resolve().parent.parent / "quicktime_backend.py"
).read_text()


def test_capture_config_selected_by_value_not_index():
    # set_configuration() takes bConfigurationValue. Descriptor index + 1
    # only coincides with it while the phone numbers configurations
    # 1..N in order -- true on the bench phone, guaranteed nowhere
    # (external code review of the BSOD dossier, 2026-10-20).
    assert "config_index = cfg.bConfigurationValue" in BACKEND
    assert "config_index = i + 1" not in BACKEND
