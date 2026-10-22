"""The probe runs ONE variant per process, like the app runs one session.

The multi-variant version blue-screened the bench machine three times
(WDF_VIOLATION 0x10D). These tests pin the safe shape: a variant argument
is required, there is no loop over variants, and cleanup goes through
the helper whose control transfer opens the device before any dispose.
"""
import pathlib

SRC = (
    pathlib.Path(__file__).resolve().parent.parent.parent / "tools" / "qt_probe.py"
).read_text()


def test_one_variant_per_process():
    assert "one variant per run" in SRC
    assert "for variant in" not in SRC


def test_cleanup_opens_before_disposing():
    body = SRC[SRC.index("def _cleanup"):]
    body = body[: body.index("def ", 10)]
    assert body.index("ctrl_transfer") < body.index("dispose_resources")


def test_variant_c_is_not_offered():
    # C's opening set_configuration() is the documented crash trigger
    # (instrumented trace, 2026-10-20). It stays off the menu.
    assert '"C":' not in SRC.split("_VARIANTS")[1].split("}")[0]
