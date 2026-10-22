// Stub: everything lives in windows.h for this syntax harness.
//
// Known gap, stated plainly: the real uuids.h switches between declaring and
// DEFINING the system GUIDs depending on whether INITGUID is set. This stub
// always declares them as static objects, so it can never reproduce the
// ~100 x C2374 redefinition wall that MSVC produced when dshow_filter.cpp had
// INITGUID defined before <uuids.h>. The include ordering that avoids it is
// pinned by engine/tests/test_windows_branches.py instead.
#pragma once
#include <windows.h>
