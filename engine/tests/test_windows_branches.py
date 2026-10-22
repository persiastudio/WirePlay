"""Static guards for code that this Linux test rig can never compile.

Both of the Windows-only failures that reached a real MSVC build were invisible
here by construction:

* ``framebus.cpp`` used ``kSyncOffsetAligned`` in its ``#ifdef _WIN32`` half. The
  declared constant is ``kSlotOffsetAligned``. g++ compiles the POSIX half, so
  the typo was a guaranteed C2065 on Windows and a guaranteed pass here.
* A CMakeLists rewrite deleted the ``if(MSVC)`` block for the ``wireplay``
  target. ``NOMINMAX`` silently stopped being defined and every ``std::min``
  in ``main.cpp`` became C2589.

Neither is caught by running tests, because neither path runs here. These check
the text instead. They are deliberately narrow: they look for exactly these two
shapes and nothing else.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = sorted(
    list((ROOT / "src").rglob("*.cpp"))
    + list((ROOT / "src").rglob("*.hpp"))
    + list((ROOT / "tools").rglob("*.cpp"))
)


def _win32_regions(text):
    """Yield the line ranges inside #ifdef _WIN32 / #if defined(_WIN32) blocks.

    Handles nesting and the #else half. The #ifndef _WIN32 half is the one this
    rig does compile, so it is skipped.
    """
    lines = text.split("\n")
    stack = []  # list of [active_now, ever_active]
    out = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if re.match(r"#\s*ifdef\s+_WIN32\b", stripped) or re.match(
            r"#\s*if\s+defined\s*\(\s*_WIN32\s*\)", stripped
        ):
            stack.append([True, True])
        elif re.match(r"#\s*if(def|ndef)?\b", stripped):
            stack.append([False, False])
        elif re.match(r"#\s*elif\b", stripped):
            if stack:
                stack[-1] = [False, stack[-1][1]]
        elif re.match(r"#\s*else\b", stripped):
            if stack:
                stack[-1] = [False, stack[-1][1]]
        elif re.match(r"#\s*endif\b", stripped):
            if stack and stack[-1][0]:
                out.append(i)  # marked below via ranges
            if stack:
                stack.pop()
        elif stack and stack[-1][0]:
            out.append(i)
    return out, lines


DECLARATION = re.compile(
    r"(?:constexpr|const)\s+(?:unsigned\s+|signed\s+)?[A-Za-z_][A-Za-z0-9_:<>]*\s+"
    r"(k[A-Z][A-Za-z0-9_]*)\b"
)


def test_every_constant_used_in_a_win32_branch_is_declared():
    declared = set()
    for path in SOURCES:
        declared.update(DECLARATION.findall(path.read_text()))
        declared.update(re.findall(r"#\s*define\s+(k[A-Z][A-Za-z0-9_]*)", path.read_text()))

    assert "kSlotOffsetAligned" in declared, "the harness lost sight of the real constant"

    missing = {}
    for path in SOURCES:
        text = path.read_text()
        idxs, lines = _win32_regions(text)
        for i in idxs:
            for name in re.findall(r"\bk[A-Z][A-Za-z0-9_]*\b", lines[i]):
                if name not in declared:
                    missing.setdefault(str(path.relative_to(ROOT)), []).append(
                        "%d:%s" % (i + 1, name)
                    )

    assert not missing, (
        "constants used only inside #ifdef _WIN32 that are never declared; this "
        "is a guaranteed MSVC C2065 that no Linux build can catch: %r" % missing
    )


def test_the_win32_branch_guard_still_sees_both_halves():
    """If the region walker ever stops matching, the test above passes vacuously."""
    text = (ROOT / "src" / "wp" / "framebus.cpp").read_text()
    idxs, lines = _win32_regions(text)
    body = "\n".join(lines[i] for i in idxs)
    assert "CreateFileMapping" in body, "the #ifdef _WIN32 half was not collected"
    assert "shm_open" not in body, "the POSIX half leaked into the Windows region"


CMAKE = (ROOT / "CMakeLists.txt").read_text()

# Targets that compile main.cpp or framebus.cpp, i.e. everything that ends up
# including windows.h through an SDL or FFmpeg header before our own.
NEEDS_NOMINMAX = ["wireplay", "wireplay_camera", "wireplay_framebus_tests",
                  "wireplay_camera_probe"]


def test_every_windows_target_defines_nominmax():
    """A deleted if(MSVC) block must not pass unnoticed again."""
    for target in NEEDS_NOMINMAX:
        block = re.search(
            r"target_compile_definitions\(\s*%s\s+PRIVATE([^)]*)\)" % re.escape(target),
            CMAKE,
            re.S,
        )
        assert block, "no target_compile_definitions for %s -- was the MSVC block deleted?" % target
        assert "NOMINMAX" in block.group(1), "%s does not get NOMINMAX on the command line" % target
        # Deliberately NOT asserting _CRT_SECURE_NO_WARNINGS here: C4996 is
        # fixed at the call site instead. test_no_crt_suppression.py enforces
        # that it stays gone from every target.


def test_initguid_is_included_after_the_sdk_headers():
    """INITGUID before <uuids.h> redefines every system GUID: ~100 x C2374."""
    text = (ROOT / "src" / "dshow_filter.cpp").read_text()
    assert not re.search(r"#\s*define\s+INITGUID", text), (
        "hand-defining INITGUID is what caused the redefinition storm; use "
        "<initguid.h> after the SDK headers instead"
    )
    initguid = text.index("#include <initguid.h>")
    for sdk in ("<dshow.h>", "<strmif.h>", "<uuids.h>"):
        assert text.index("#include %s" % sdk) < initguid, (
            "%s must be included before initguid.h" % sdk
        )
    # and the filter's own GUID still has to be a definition in this TU
    assert "DEFINE_GUID(CLSID_WirePlayCamera" in text


def test_the_stub_agrees_with_the_sdk_on_imediaeventsink():
    """The harness once "verified" a method that does not exist.

    `dshow_filter.cpp` called `IMediaEventSink::NotifyEvent`. That is a helper
    on the SDK's CBaseFilter, not a member of the interface -- IMediaEventSink
    has exactly one method, `Notify(long, LONG_PTR, LONG_PTR)`. The stub
    declared `NotifyEvent`, so both sides were wrong in the same direction and
    the compile passed. MSVC answered C2039 and pointed at strmif.h.

    This is the failure mode a stub harness cannot catch on its own, so the
    corrected name is pinned on both sides.
    """
    stub = (ROOT / "tests" / "win32_stub" / "windows.h").read_text()
    iface = re.search(r"struct IMediaEventSink : IUnknown \{(.*?)\n\};", stub, re.S)
    assert iface, "the stub lost IMediaEventSink"
    methods = re.findall(r"virtual HRESULT (\w+)\(", iface.group(1))
    assert methods == ["Notify"], "expected the single strmif.h method, got %r" % methods

    filt = (ROOT / "src" / "dshow_filter.cpp").read_text()
    # Anything called on the sink must be either the interface's one method or
    # something inherited from IUnknown. Nothing else exists to call.
    called = set(re.findall(r"event_sink_->(\w+)\(", filt))
    assert "Notify" in called, "the geometry notification stopped being sent"
    assert called <= {"Notify", "QueryInterface", "AddRef", "Release"}, called


def test_no_override_on_a_destructor_of_an_iunknown_derived_class():
    """IUnknown has no virtual destructor, so `~T() override` is C3668.

    Caught three times: ~CWirePlayFilter, ~CWirePlayPin, and the PinEnum nested
    inside EnumPins(). The third survived two rounds because the first two were
    fixed by hand at their own sites instead of being searched for. This
    searches the whole file.
    """
    text = (ROOT / "src" / "dshow_filter.cpp").read_text()
    bad = re.findall(r"~\w+\s*\([^)]*\)\s*override", text)
    assert not bad, "destructor marked override, but IUnknown has none: %r" % bad
    # and the pattern must still be findable, or the assertion above is vacuous
    assert re.search(r"~\w+\s*\([^)]*\)\s*override", "~PinEnum() override {")
