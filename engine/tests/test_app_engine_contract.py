"""The flags the C++ app sends to the engine must be flags the engine accepts.

This test exists because of a real breakage: `buildEngineArgv` in src/main.cpp
started passing `--backend` unconditionally while the engine's argparse had no
such argument. argparse would have rejected it, the engine would have exited
immediately, and the app would have reported a failure with no useful message —
on the user's machine, at first run.

The two sides are different languages with no shared header, so nothing else
catches it.
"""

import pathlib
import re
import sys

import pytest


import wireplay_engine  # noqa: E402

MAIN_CPP = pathlib.Path(__file__).resolve().parents[2] / "src" / "main.cpp"


def build_engine_argv_source() -> str:
    text = MAIN_CPP.read_text(encoding="utf-8")
    start = text.index("std::vector<std::string> buildEngineArgv(")
    end = text.index("\n}\n", start)
    return text[start:end]


def flags_pushed_by_app() -> set[str]:
    """Every literal flag the app can push onto the engine's argv."""
    body = build_engine_argv_source()
    return set(re.findall(r'push_back\("(--?[a-z0-9-]+)"\)', body))


def engine_flags() -> set[str]:
    """Every option string the engine's parser accepts, short forms included.

    Read out of the add_argument calls rather than from a hand-kept list, so
    adding a flag to the engine cannot silently desynchronise this test.
    """
    source = pathlib.Path(wireplay_engine.__file__).read_text(encoding="utf-8")
    flags: set[str] = set()
    for call in re.finditer(r"p\.add_argument\((.*?)\)\n", source, re.DOTALL):
        flags.update(re.findall(r'"(-{1,2}[A-Za-z0-9-]+)"', call.group(1)))
    return flags


def test_main_cpp_exists_and_has_the_builder():
    assert MAIN_CPP.exists(), f"cannot find {MAIN_CPP}"
    assert "buildEngineArgv" in MAIN_CPP.read_text(encoding="utf-8")


def test_app_only_sends_flags_the_engine_accepts():
    sent = flags_pushed_by_app()
    assert sent, "found no flags in buildEngineArgv; the regex probably rotted"
    accepted = engine_flags()
    assert accepted, "found no engine flags; the regex probably rotted"
    unknown = sent - accepted
    assert not unknown, (
        f"main.cpp sends {sorted(unknown)} but the engine does not accept it. "
        "argparse would reject the whole command line and the engine would exit "
        "before mirroring anything."
    )


def test_backend_values_match_on_both_sides():
    """--backend is a closed set spelled in two languages; pin the spelling."""
    main = MAIN_CPP.read_text(encoding="utf-8")
    for value in ("coredevice", "quicktime", "auto"):
        assert f'"{value}"' in main, f"main.cpp does not offer --backend {value}"
    # And the engine's choices must be exactly the same three.
    src = pathlib.Path(wireplay_engine.__file__).read_text(encoding="utf-8")
    m = re.search(r'--backend.*?choices=\(([^)]*)\)', src, re.DOTALL)
    assert m, "engine --backend has no explicit choices"
    choices = set(re.findall(r'"([a-z]+)"', m.group(1)))
    assert choices == {"auto", "coredevice", "quicktime"}


def test_backend_regex_did_not_rot():
    """Guard the guards: if these regexes stop matching, the tests above pass
    vacuously, which is worse than failing."""
    assert "--udid" in flags_pushed_by_app()
    assert "--backend" in flags_pushed_by_app()
    assert "--udid" in engine_flags()
    assert "--backend" in engine_flags()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-q"]))
