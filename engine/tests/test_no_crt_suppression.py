"""`_CRT_SECURE_NO_WARNINGS` is a mute button, not a fix.

MSVC marks a specific list of C functions deprecated (C4996) because each has a
bounds-checked or resource-managed replacement. Defining
`_CRT_SECURE_NO_WARNINGS` silences the warning for every file in the target,
including ones nobody has read, so the next genuinely unsafe call arrives with
no complaint at all.

So this repo defines it nowhere, and every call site uses the replacement:

* `char[160]` + `snprintf`  -> `std::string`
* `fopen`/`fwrite`/`fclose` -> `std::ofstream` (RAII closes every exit path)
* `getenv("TMPDIR")`        -> `std::filesystem::temp_directory_path()`
* `freopen`/`fopen` where a real `FILE*` is unavoidable ->
  `_freopen_s`/`fopen_s`, which return the error instead of NULL + global errno

The last category is the honest one. `FrameReader::hasPending()` polls the file
descriptor of the engine pipe and `wp::writeFrame()` takes a `FILE*`, so no
iostream can replace them; there the guarded `_s` variant is the modern API.

These tests read the sources rather than run them, because C4996 only exists on
the compiler this sandbox does not have.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = sorted(
    list((ROOT / "src").rglob("*.cpp"))
    + list((ROOT / "src").rglob("*.hpp"))
    + list((ROOT / "tools").rglob("*.cpp"))
    + list((ROOT / "tests" / "cpp").rglob("*.cpp"))
    + list((ROOT / "tests" / "cpp").rglob("*.hpp"))
)

# The C4996 list, reduced to the ones a C++17 codebase might actually reach.
# The `_s` variants do not match: `fopen_s(` has `_s` between the name and the
# parenthesis, so `\bfopen\s*\(` skips it. Same for `_freopen_s`.
DEPRECATED = re.compile(
    r"\b(?:strcpy|strncpy|strcat|strncat|sprintf|vsprintf|_snprintf|_vsnprintf|"
    r"scanf|sscanf|fscanf|vsscanf|fopen|freopen|tmpfile|getenv|strtok|"
    r"localtime|gmtime|asctime|ctime|itoa|wcscpy|wcsncpy|wcsncat|swprintf)\s*\("
)

MUTE = re.compile(r"\b_(?:CRT|SCL)_SECURE_NO_WARNINGS\b")


def test_no_target_silences_c4996():
    """The mute button must not come back, on any target or in any preset."""
    for name in ("CMakeLists.txt", "CMakePresets.json"):
        text = (ROOT / name).read_text()
        assert not MUTE.search(text), (
            "%s defines _CRT_SECURE_NO_WARNINGS: fix the call site instead" % name
        )


def strip_comments(text):
    """Blank out C/C++ comments, keeping line numbers intact.

    Without this the scanner flags prose: the comment that explains why
    `getenv` was replaced itself contains `getenv(`. A hand-rolled scanner
    rather than a regex because `//` inside a string literal is not a comment.
    """
    out = []
    i, n = 0, len(text)
    in_line = in_block = False
    quote = None
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            i += 1
            continue
        if in_block:
            if c == "*" and nxt == "/":
                in_block = False
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
            continue
        if quote:
            out.append(c)
            if c == "\\":
                if nxt:
                    out.append(nxt)
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c == "/" and nxt == "/":
            in_line = True
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_block = True
            i += 2
            continue
        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _non_msvc_regions(text):
    """Line indices compiled by a non-MSVC compiler only.

    Those are the bodies of `#ifndef _MSC_VER` and the `#else` half of
    `#ifdef _MSC_VER`. A deprecated call is acceptable there, because the
    platform that deprecates it is not compiling that line.
    """
    lines = text.split("\n")
    stack = []  # [kind, collecting]
    out = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if re.match(r"#\s*ifdef\s+_MSC_VER\b", stripped):
            stack.append(["ifdef", False])
        elif re.match(r"#\s*ifndef\s+_MSC_VER\b", stripped):
            stack.append(["ifndef", True])
        elif re.match(r"#\s*if(def|ndef)?\b", stripped):
            stack.append(["other", False])
        elif re.match(r"#\s*else\b", stripped) and stack:
            kind, _ = stack[-1]
            stack[-1] = [kind, kind == "ifdef"]
        elif re.match(r"#\s*endif\b", stripped) and stack:
            stack.pop()
        elif stack and stack[-1][1]:
            out.append(i)
    return out, lines


def test_no_deprecated_crt_call_reaches_msvc():
    offenders = {}
    for path in SOURCES:
        text = strip_comments(path.read_text())
        allowed, lines = _non_msvc_regions(text)
        for i, line in enumerate(lines):
            if i in allowed:
                continue
            for m in DEPRECATED.finditer(line):
                offenders.setdefault(str(path.relative_to(ROOT)), []).append(
                    "%d:%s" % (i + 1, m.group(0).rstrip("("))
                )

    assert not offenders, (
        "deprecated CRT calls visible to MSVC; use the _s variant, an iostream, "
        "or std::filesystem -- do not re-add _CRT_SECURE_NO_WARNINGS: %r" % offenders
    )


def test_the_guard_is_not_passing_by_collecting_nothing():
    """Two sanity checks, or the test above is vacuous.

    1. the walker really finds the one sanctioned deprecated call, inside the
       `#else` half of `#ifdef _MSC_VER` in test_protocol.cpp;
    2. moving it outside that guard makes the previous test fail.
    """
    path = ROOT / "tests" / "cpp" / "test_protocol.cpp"
    text = strip_comments(path.read_text())
    allowed, lines = _non_msvc_regions(text)
    hits = [lines[i].strip() for i in allowed if DEPRECATED.search(lines[i])]
    assert hits == ["return std::fopen(path.c_str(), mode);"], hits

    unguarded = text.replace(
        "#ifdef _MSC_VER\n    FILE* f = nullptr;\n    return fopen_s(&f, path.c_str(), mode) == 0 ? f : nullptr;\n#else\n    return std::fopen(path.c_str(), mode);\n#endif",
        "    return std::fopen(path.c_str(), mode);",
    )
    assert unguarded != text, "the openBinary guard changed shape; update this test"
    allowed2, lines2 = _non_msvc_regions(unguarded)
    assert any(
        DEPRECATED.search(lines2[i]) for i in range(len(lines2)) if i not in allowed2
    ), "removing the #ifdef should expose a deprecated call to MSVC"


def test_msvc_only_variants_are_the_secure_ones():
    """Where a FILE* is unavoidable, the MSVC branch must use the _s API."""
    for path in SOURCES:
        text = path.read_text()
        for m in re.finditer(r"#ifdef _MSC_VER\n(.*?)\n#else", text, re.S):
            body = m.group(1)
            assert re.search(r"\b\w+_s\s*\(", body), (
                "%s has an #ifdef _MSC_VER branch that calls no _s function; "
                "if it is not using the secure API it should not exist"
                % path.relative_to(ROOT)
            )


def test_strip_comments_keeps_line_numbers_and_respects_strings():
    """If the stripper is wrong, the test above is either blind or noisy."""
    src = 'a\n// fopen(\n/* strcpy( */\nb; const char* u = "http://x//fopen(";\nc;\n'
    out = strip_comments(src)
    assert out.count("\n") == src.count("\n"), "line numbers moved"
    assert not DEPRECATED.search(out.split("\n")[1]), "line comment not stripped"
    assert not DEPRECATED.search(out.split("\n")[2]), "block comment not stripped"
    assert "//fopen(" in out, "a // inside a string literal is not a comment"
    # the real tree must still contain the sanctioned call after stripping
    kept = strip_comments((ROOT / "tests" / "cpp" / "test_protocol.cpp").read_text())
    assert "std::fopen(path.c_str(), mode)" in kept
