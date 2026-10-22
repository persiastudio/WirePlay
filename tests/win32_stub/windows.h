// Minimal stand-in for the Win32 + DirectShow headers, so that
// src/dshow_filter.cpp can be compiled on Linux as a syntax and type check.
//
// WHAT THIS IS: a way to catch typos, undeclared identifiers, wrong member
// names, missing definitions and C++ errors in the filter before it reaches a
// Windows compiler. Signatures here are transcribed from the documented COM
// interfaces.
//
// WHAT THIS IS NOT: proof that the filter works, or even that it links. The
// stubs are my transcription, so a place where I misremembered the real API
// would be wrong in both the stub and the filter and would pass here. Runtime
// behaviour is still entirely unverified. Treat "it compiles" as the floor.
#pragma once

// This header impersonates <windows.h> so the DirectShow filter can be type
// checked on Linux. If it is ever picked up by a real Windows build it shadows
// the genuine header and every Win32 API disappears, so fail loudly instead.
#if defined(_WIN32) || defined(_WIN64)
#error "tests/win32_stub/windows.h was included on Windows. The stub include \
directory must never be on the include path of a real Windows build."
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

// --- scalar types ------------------------------------------------------------
// Win32 scalar types are fixed width: LONG is 32 bits even on a 64-bit build.
// Getting this wrong changes the layout of every struct that embeds one, which
// is exactly the kind of divergence this harness exists to catch.
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int BOOL;
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef wchar_t WCHAR;
typedef void* HANDLE;
typedef void* HINSTANCE;
typedef void* HKEY;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef WCHAR* LPWSTR;
typedef const WCHAR* LPCWSTR;
typedef char* LPSTR;
typedef int64_t LONGLONG;
typedef LONGLONG REFERENCE_TIME;
typedef long HRESULT;
typedef int64_t LONG_PTR;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define MAX_PATH 260
#define WINAPI
#define IN
#define OUT

// --- GUIDs -------------------------------------------------------------------
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID;
typedef GUID CLSID;
typedef GUID IID;
typedef const GUID& REFIID;
typedef const GUID& REFCLSID;
typedef const GUID& REFGUID;

// --- COM macros --------------------------------------------------------------
#define STDMETHOD(method) virtual HRESULT method
#define STDMETHOD_(type, method) virtual type method
#define STDMETHODIMP HRESULT
#define STDMETHODIMP_(type) type
#define STDAPI extern "C" HRESULT
#define override_stub

#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif

// --- error codes -------------------------------------------------------------
#define S_OK ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_UNEXPECTED ((HRESULT)0x8000FFFFL)
#define VFW_E_INVALIDMEDIATYPE ((HRESULT)0x80040200L)
#define VFW_E_NOT_COMMITTED ((HRESULT)0x80040211L)
#define CLASS_E_CLASSNOTAVAILABLE ((HRESULT)0x80040111L)
#define CLASS_E_NOAGGREGATION ((HRESULT)0x80040110L)
#define VFW_E_NOT_CONNECTED ((HRESULT)0x80040209L)
#define VFW_E_ALREADY_CONNECTED ((HRESULT)0x8004020AL)
#define VFW_E_INVALID_DIRECTION ((HRESULT)0x80040208L)
#define VFW_E_NOT_FOUND ((HRESULT)0x80040216L)
#define VFW_E_NO_ACCEPTABLE_TYPES ((HRESULT)0x80040207L)
#define VFW_E_NO_TRANSPORT ((HRESULT)0x8004020CL)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x))

// --- basic COM interfaces ----------------------------------------------------
struct IUnknown {
    // The real IUnknown has no virtual destructor; COM objects delete
    // themselves from Release(). Declared here so `delete this` is not UB in
    // the harness and does not warn.
    virtual ~IUnknown() {}
    virtual HRESULT QueryInterface(REFIID riid, void** ppv) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
};
inline bool operator==(const GUID& a, const GUID& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3;
}
inline bool operator!=(const GUID& a, const GUID& b) { return !(a == b); }

// DEFINE_GUID with INITGUID defined emits the definition; that is what the
// filter does for its own CLSID. Note this stub ignores INITGUID entirely, so
// it cannot catch an INITGUID-before-<uuids.h> ordering mistake -- see
// engine/tests/test_windows_branches.py.
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}

#define STUB_GUID(name) static const GUID name = {0, 0, 0, {0}}

STUB_GUID(IID_IUnknown);
STUB_GUID(IID_IEnumMediaTypes);
STUB_GUID(IID_IEnumPins);
STUB_GUID(IID_IFilterMapper2);

struct IClassFactory : IUnknown {
    virtual HRESULT CreateInstance(IUnknown* outer, REFIID riid, void** ppv) = 0;
    virtual HRESULT LockServer(BOOL lock) = 0;
};

// --- media types -------------------------------------------------------------
typedef struct _AM_MEDIA_TYPE {
    GUID majortype;
    GUID subtype;
    BOOL bFixedSizeSamples;
    BOOL bTemporalCompression;
    ULONG lSampleSize;
    GUID formattype;
    IUnknown* pUnk;
    ULONG cbFormat;
    BYTE* pbFormat;
} AM_MEDIA_TYPE;

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER;

typedef struct tagVIDEOINFOHEADER {
    long rcSource[4];
    long rcTarget[4];
    DWORD dwBitRate;
    DWORD dwBitErrorRate;
    REFERENCE_TIME AvgTimePerFrame;
    BITMAPINFOHEADER bmiHeader;
} VIDEOINFOHEADER;

typedef struct _AllocatorProperties {
    long cBuffers;
    long cbBuffer;
    long cbAlign;
    long cbPrefix;
} ALLOCATOR_PROPERTIES;

enum PIN_DIRECTION { PINDIR_INPUT = 0, PINDIR_OUTPUT };
enum FILTER_STATE { State_Stopped = 0, State_Paused, State_Running };

typedef struct _FilterInfo {
    WCHAR achName[128];
    struct IFilterGraph* pGraph;
} FILTER_INFO;

typedef struct _PinInfo {
    IUnknown* pFilter;
    PIN_DIRECTION dir;
    WCHAR achName[128];
} PIN_INFO;

// --- DirectShow interfaces ---------------------------------------------------
struct IPin;
struct IMemAllocator;
struct IMediaSample;
struct IReferenceClock;

struct IEnumMediaTypes : IUnknown {
    virtual HRESULT Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) = 0;
    virtual HRESULT Skip(ULONG cMediaTypes) = 0;
    virtual HRESULT Reset() = 0;
    virtual HRESULT Clone(IEnumMediaTypes** ppEnum) = 0;
};

struct IEnumPins : IUnknown {
    virtual HRESULT Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) = 0;
    virtual HRESULT Skip(ULONG cPins) = 0;
    virtual HRESULT Reset() = 0;
    virtual HRESULT Clone(IEnumPins** ppEnum) = 0;
};

struct IPin : IUnknown {
    virtual HRESULT Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) = 0;
    virtual HRESULT ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) = 0;
    virtual HRESULT Disconnect() = 0;
    virtual HRESULT ConnectedTo(IPin** ppPin) = 0;
    virtual HRESULT ConnectionMediaType(AM_MEDIA_TYPE* pmt) = 0;
    virtual HRESULT QueryPinInfo(PIN_INFO* pInfo) = 0;
    virtual HRESULT QueryDirection(PIN_DIRECTION* pPinDir) = 0;
    virtual HRESULT QueryId(LPWSTR* Id) = 0;
    virtual HRESULT QueryAccept(const AM_MEDIA_TYPE* pmt) = 0;
    virtual HRESULT EnumMediaTypes(IEnumMediaTypes** ppEnum) = 0;
    virtual HRESULT QueryInternalConnections(IPin** apPin, ULONG* nPin) = 0;
    virtual HRESULT EndOfStream() = 0;
    virtual HRESULT BeginFlush() = 0;
    virtual HRESULT EndFlush() = 0;
    virtual HRESULT NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) = 0;
};

typedef struct tagSIZE {
    long cx;
    long cy;
} SIZE;

typedef struct tagVIDEO_STREAM_CONFIG_CAPS {
    GUID guid;
    ULONG VideoStandard;
    SIZE InputSize;
    SIZE MinCroppingSize;
    SIZE MaxCroppingSize;
    int CropGranularityX;
    int CropGranularityY;
    int CropAlignX;
    int CropAlignY;
    SIZE MinOutputSize;
    SIZE MaxOutputSize;
    int OutputGranularityX;
    int OutputGranularityY;
    int StretchTapsX;
    int StretchTapsY;
    int ShrinkTapsX;
    int ShrinkTapsY;
    REFERENCE_TIME MinFrameInterval;
    REFERENCE_TIME MaxFrameInterval;
    long MinBitsPerSecond;
    long MaxBitsPerSecond;
} VIDEO_STREAM_CONFIG_CAPS;

static const IID IID_IAMStreamConfig = {0x35736eb0, 0x8f3b, 0x11d1, {0x84, 0x4f, 0x00, 0xa0, 0xc9, 0x11, 0xcf, 0x81}};

struct IAMStreamConfig : IUnknown {
    virtual HRESULT SetFormat(AM_MEDIA_TYPE* pmt) = 0;
    virtual HRESULT GetFormat(AM_MEDIA_TYPE** ppmt) = 0;
    virtual HRESULT GetNumberOfCapabilities(int* piCount, int* piSize) = 0;
    virtual HRESULT GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) = 0;
};

struct IMediaSample : IUnknown {
    virtual HRESULT GetPointer(BYTE** ppBuffer) = 0;
    virtual long GetSize() = 0;
    virtual HRESULT GetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd) = 0;
    virtual HRESULT SetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd) = 0;
    virtual HRESULT IsSyncPoint() = 0;
    virtual HRESULT SetSyncPoint(BOOL bIsSyncPoint) = 0;
    virtual HRESULT IsPreroll() = 0;
    virtual HRESULT SetPreroll(BOOL bIsPreroll) = 0;
    virtual long GetActualDataLength() = 0;
    virtual HRESULT SetActualDataLength(long len) = 0;
    virtual HRESULT GetMediaType(AM_MEDIA_TYPE** ppMediaType) = 0;
    virtual HRESULT SetMediaType(AM_MEDIA_TYPE* pMediaType) = 0;
    virtual HRESULT IsDiscontinuity() = 0;
    virtual HRESULT SetDiscontinuity(BOOL bDiscontinuity) = 0;
    virtual HRESULT GetMediaTime(LONGLONG* pTimeStart, LONGLONG* pTimeEnd) = 0;
    virtual HRESULT SetMediaTime(LONGLONG* pTimeStart, LONGLONG* pTimeEnd) = 0;
};

struct IMemAllocator : IUnknown {
    virtual HRESULT SetProperties(ALLOCATOR_PROPERTIES* pRequest,
                                  ALLOCATOR_PROPERTIES* pActual) = 0;
    virtual HRESULT GetProperties(ALLOCATOR_PROPERTIES* pProps) = 0;
    virtual HRESULT Commit() = 0;
    virtual HRESULT Decommit() = 0;
    virtual HRESULT GetBuffer(IMediaSample** ppBuffer, REFERENCE_TIME* pStartTime,
                              REFERENCE_TIME* pEndTime, DWORD dwFlags) = 0;
    virtual HRESULT ReleaseBuffer(IMediaSample* pBuffer) = 0;
};

struct IMemInputPin : IUnknown {
    virtual HRESULT GetAllocator(IMemAllocator** ppAllocator) = 0;
    virtual HRESULT NotifyAllocator(IMemAllocator* pAllocator, BOOL bReadOnly) = 0;
    virtual HRESULT GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pProps) = 0;
    virtual HRESULT Receive(IMediaSample* pSample) = 0;
    virtual HRESULT ReceiveMultiple(IMediaSample** pSamples, long nSamples,
                                    long* nSamplesProcessed) = 0;
    virtual HRESULT ReceiveCanBlock() = 0;
};

struct IReferenceClock : IUnknown {
    virtual HRESULT GetTime(REFERENCE_TIME* pTime) = 0;
    virtual HRESULT AdviseTime(REFERENCE_TIME baseTime, REFERENCE_TIME streamTime, void* hEvent,
                               DWORD* pdwAdviseCookie) = 0;
    virtual HRESULT AdvisePeriodic(REFERENCE_TIME startTime, REFERENCE_TIME periodTime,
                                   void* hSemaphore, DWORD* pdwAdviseCookie) = 0;
    virtual HRESULT Unadvise(DWORD dwAdviseCookie) = 0;
};

struct IPersist : IUnknown {
    virtual HRESULT GetClassID(CLSID* pClassID) = 0;
};

struct IMediaFilter : IPersist {
    virtual HRESULT Stop() = 0;
    virtual HRESULT Pause() = 0;
    virtual HRESULT Run(REFERENCE_TIME tStart) = 0;
    virtual HRESULT GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) = 0;
    virtual HRESULT SetSyncSource(IReferenceClock* pClock) = 0;
    virtual HRESULT GetSyncSource(IReferenceClock** pClock) = 0;
};

struct IFilterGraph : IUnknown {};

struct IBaseFilter : IMediaFilter {
    virtual HRESULT EnumPins(IEnumPins** ppEnum) = 0;
    virtual HRESULT FindPin(LPCWSTR Id, IPin** ppPin) = 0;
    virtual HRESULT QueryFilterInfo(FILTER_INFO* pInfo) = 0;
    virtual HRESULT JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) = 0;
    virtual HRESULT QueryVendorInfo(LPWSTR* pVendorInfo) = 0;
};

struct IMediaEventSink : IUnknown {
    // Named Notify, as in strmif.h. NotifyEvent is CBaseFilter's helper in the
    // SDK baseclasses, not a member of this interface -- writing NotifyEvent
    // here is what let dshow_filter.cpp compile against a method that does
    // not exist, and MSVC answered with C2039.
    virtual HRESULT Notify(long EventCode, LONG_PTR Param1, LONG_PTR Param2) = 0;
};

// Minimal REGFILTER2 family: only the fields dshow_filter.cpp fills in.
// The real ones live in strmif.h; DllRegisterServer must pass a full
// REGFILTER2 to IFilterMapper2::RegisterFilter (a null prf2 with a
// category crashed regsvr32 on Windows), so the syntax build needs them.
struct REGPINTYPES {
    const GUID* clsMajorType;
    const GUID* clsMinorType;
};
struct REGFILTERPINS {
    LPWSTR strName;
    BOOL bRendered;
    BOOL bOutput;
    BOOL bZero;
    BOOL bMany;
    const GUID* clsConnectsToFilter;
    const WCHAR* strConnectsToPin;
    UINT nMediaTypes;
    const REGPINTYPES* lpMediaType;
};
struct REGFILTER2 {
    DWORD dwVersion;
    DWORD dwMerit;
    ULONG cPins;
    const REGFILTERPINS* rgPins;
};

struct IFilterMapper2 : IUnknown {
    virtual HRESULT RegisterFilter(REFCLSID clsid, LPCWSTR Name, IUnknown** ppMoniker,
                                   const GUID* pCategory, LPCWSTR Instance,
                                   const void* pRegFilter) = 0;
    virtual HRESULT UnregisterFilter(const GUID* pCategory, LPCWSTR Instance, REFCLSID Filter) = 0;
};

// --- GUID constants ----------------------------------------------------------
STUB_GUID(MEDIATYPE_Video);
STUB_GUID(MEDIASUBTYPE_RGB32);
STUB_GUID(FORMAT_VideoInfo);
STUB_GUID(GUID_NULL);
STUB_GUID(CLSID_MemoryAllocator);
STUB_GUID(CLSID_FilterMapper2);
STUB_GUID(CLSID_VideoInputDeviceCategory);
STUB_GUID(IID_IMediaEventSink);
STUB_GUID(IID_IMemInputPin);
STUB_GUID(IID_IMemAllocator);
STUB_GUID(IID_IPin);
STUB_GUID(IID_IBaseFilter);
STUB_GUID(IID_IMediaFilter);
STUB_GUID(IID_IPersist);
STUB_GUID(IID_IClassFactory);
#define EC_VIDEO_SIZE_CHANGED 0x000E

// --- threading / sync --------------------------------------------------------
typedef struct _CRITICAL_SECTION {
    void* p;
} CRITICAL_SECTION, *PCRITICAL_SECTION;
inline void InitializeCriticalSection(CRITICAL_SECTION*) {}
inline void DeleteCriticalSection(CRITICAL_SECTION*) {}
inline void EnterCriticalSection(CRITICAL_SECTION*) {}
inline void LeaveCriticalSection(CRITICAL_SECTION*) {}

inline LONG InterlockedIncrement(volatile LONG* p) { return ++(*p); }

// File/environment helpers used only by the filter's opt-in debug log.
#ifndef FILE_APPEND_DATA
#define FILE_APPEND_DATA 0x0004
#define FILE_SHARE_READ 0x0001
#define FILE_SHARE_WRITE 0x0002
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x0080
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif
inline DWORD GetEnvironmentVariableA(const char*, char*, DWORD) { return 0; }
inline DWORD GetTempPathA(DWORD, char*) { return 0; }
inline DWORD GetTickCount() { return 0; }
inline HANDLE CreateFileA(const char*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE) {
    return INVALID_HANDLE_VALUE;
}
inline BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*) { return FALSE; }
inline LONG InterlockedDecrement(volatile LONG* p) { return --(*p); }
inline LONG InterlockedExchange(volatile LONG* p, LONG v) {
    const LONG old = *p;
    *p = v;
    return old;
}

typedef void* LPSECURITY_ATTRIBUTES;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION;
typedef struct _STARTUPINFOA {
    DWORD cb;
    char* lpReserved;
} STARTUPINFOA;

inline HANDLE CreateThread(LPSECURITY_ATTRIBUTES, size_t, LPTHREAD_START_ROUTINE, LPVOID, DWORD,
                           DWORD*) {
    return nullptr;
}
inline DWORD WaitForSingleObject(HANDLE, DWORD) { return 0; }
inline BOOL CloseHandle(HANDLE) { return TRUE; }
inline void Sleep(DWORD) {}
inline BOOL DisableThreadLibraryCalls(HINSTANCE) { return TRUE; }

typedef union _LARGE_INTEGER {
    LONGLONG QuadPart;
} LARGE_INTEGER;
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* p) {
    p->QuadPart = 10000000;
    return TRUE;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* p) {
    p->QuadPart = 0;
    return TRUE;
}

// --- memory / strings / registry --------------------------------------------
void* CoTaskMemAlloc(size_t cb);
void CoTaskMemFree(void* pv);

inline void ZeroMemory(void* p, size_t n) { memset(p, 0, n); }
#define _TRUNCATE ((size_t)-1)
inline int wcsncpy_s(WCHAR* dst, size_t n, const WCHAR* src, size_t) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; ++i) dst[i] = src[i];
    if (n) dst[i] = 0;
    return 0;
}
inline size_t wcslen(const WCHAR* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
inline int wcscmp(const WCHAR* a, const WCHAR* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return int(*a) - int(*b);
}
inline int StringFromGUID2(REFGUID, LPWSTR, int) { return 0; }
#define MAKELPARAM(l, h) ((LONG)(((WORD)(l)) | ((DWORD)((WORD)(h))) << 16))

#define ERROR_SUCCESS 0L
#define KEY_WRITE 0x20006
#define REG_SZ 1
#define HKEY_CLASSES_ROOT ((HKEY)0)
inline LONG RegCreateKeyExW(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                            HKEY* phkResult, DWORD*) {
    *phkResult = nullptr;
    return ERROR_SUCCESS;
}
inline LONG RegSetValueExW(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD) {
    return ERROR_SUCCESS;
}
inline LONG RegCloseKey(HKEY) { return ERROR_SUCCESS; }
inline LONG RegDeleteTreeW(HKEY, LPCWSTR) { return ERROR_SUCCESS; }
inline DWORD GetModuleFileNameW(HINSTANCE, LPWSTR, DWORD) { return 0; }

// Declared, not defined: the syntax-check target is an OBJECT library and is
// never linked. Defining it inline would let the compiler see that the output
// pointer is always null and emit -Wnonnull noise at every use site, which
// would hide a genuine warning.
HRESULT CoCreateInstance(REFCLSID rclsid, IUnknown* pUnkOuter, DWORD dwClsContext, REFIID riid,
                         void** ppv);
#define CLSCTX_INPROC_SERVER 1

// --- DLL entry ---------------------------------------------------------------
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define BOOL_WINENTRY

