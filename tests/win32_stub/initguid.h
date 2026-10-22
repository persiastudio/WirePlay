// Transcription of the Windows SDK's <initguid.h> for the Linux stub harness.
//
// The real header defines INITGUID and pulls in <guiddef.h>, which flips
// DEFINE_GUID from "extern declaration" to "definition" for everything after
// it. That is the whole point of including it *after* the SDK headers: uuids.h
// honours INITGUID too, so if it is set before <uuids.h> this translation unit
// redefines every system GUID (MEDIATYPE_Video, MEDIASUBTYPE_None, ...) and
// MSVC answers with ~100 C2374 errors plus C1003.
//
// src/dshow_filter.cpp relies on that ordering and
// engine/tests/test_windows_branches.py pins it.
#ifndef WIREPLAY_STUB_INITGUID_H
#define WIREPLAY_STUB_INITGUID_H

#if defined(_WIN32) || defined(_WIN64)
#error "this is the Linux test stub, not the Windows SDK header"
#endif

#define INITGUID 1

#endif  // WIREPLAY_STUB_INITGUID_H
