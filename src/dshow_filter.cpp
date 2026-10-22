// WirePlay virtual camera: a DirectShow source filter that shows whatever the
// app has last decoded.
//
// DirectShow rather than the Media Foundation virtual camera because DirectShow
// is what OBS, Streamlabs, Discord, Teams and Chrome have always enumerated.
//
// Written against raw COM on purpose. The DirectShow baseclasses (CSource,
// CSourceStream, streams.lib) used to ship in the Windows SDK samples, but
// current SDKs do not include them, and a filter that only builds when an
// optional SDK component happens to be installed is not a filter that builds.
// Everything here needs is dshow.h, uuids.h and the standard COM library.
//
// The logic that decides media types lives in camera_types.hpp and is
// unit-tested on Linux; what is left here is COM plumbing.
#include <windows.h>

#include <dshow.h>
#include <strmif.h>
#include <uuids.h>

// initguid.h comes LAST, on purpose. It flips DEFINE_GUID from "declare" to
// "define", and uuids.h honours that flag for every system GUID it contains.
// Including it first makes this translation unit define MEDIATYPE_Video and
// friends a second time, which is a wall of C2374 redefinition errors and would
// also collide with strmiids.lib at link time.
#include <initguid.h>

#include <cstdarg>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "wp/camera_types.hpp"
#include "wp/framebus.hpp"

// Live object count and module handle, both set up by DllMain.
static volatile LONG g_object_count = 0;
static HINSTANCE g_hInstance = nullptr;

// {8E14549A-DB61-4309-AFA1-3A3B5C6E7F2C}
DEFINE_GUID(CLSID_WirePlayCamera, 0x8e14549a, 0xdb61, 0x4309, 0xaf, 0xa1, 0x3a, 0x3b, 0x5c, 0x6e,
            0x7f, 0x2c);

// This string is what appears in OBS's device list, so it should say what it is.
static const wchar_t kFriendlyName[] = L"WirePlay iPhone";

// Opt-in delivery diagnostics. The filter runs inside OBS/Streamlabs where
// there is no console; WIREPLAY_FILTER_DEBUG=1 makes it append to
// %TEMP%\wireplay_filter.log so a black preview becomes a readable trace.
static bool FilterDebugEnabled() {
    static const bool on = [] {
        char v[4] = {};
        return GetEnvironmentVariableA("WIREPLAY_FILTER_DEBUG", v, 4) > 0;
    }();
    return on;
}
static void FilterDbg(const char* fmt, ...) {
    if (!FilterDebugEnabled()) return;
    char path[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, path)) return;
    std::string full = std::string(path) + "wireplay_filter.log";
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[560];
    const int n = std::snprintf(line, sizeof(line), "[%lu] %s\r\n",
                                GetTickCount(), buf);
    if (n <= 0) return;
    HANDLE h = CreateFileA(full.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line, DWORD(n), &written, nullptr);
    CloseHandle(h);
}

class CWirePlayPin;

static void CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src);
static void FreeMediaType(AM_MEDIA_TYPE* pmt);

// ---------------------------------------------------------------------------
// The filter. Owns the pin and the frame bus, and holds the graph clock that
// timestamps come from.
// ---------------------------------------------------------------------------
class CWirePlayFilter : public IBaseFilter {
public:
    CWirePlayFilter();
    ~CWirePlayFilter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClsid) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMSecs, FILTER_STATE* pState) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    LONG ref() const { return ref_; }
    FILTER_STATE state() const { return state_; }
    IReferenceClock* clock() const { return clock_; }
    // The tStart of the last Run(): stream time 0. Sample timestamps
    // must be relative to it, never the graph clock's absolute time
    // (system uptime) -- consumers schedule by stream time, so an
    // absolute stamp schedules the frame hours into the future and a
    // downstream Receive can sit on it until the graph tears down
    // (field: first Receive returned exactly at graph Stop, 4.000 s).
    REFERENCE_TIME run_start() const { return run_start_; }
    wp::FrameBus& bus() { return bus_; }
    CRITICAL_SECTION& lock() { return lock_; }
    CWirePlayPin* pin() { return pin_; }

    // The phone's geometry can change under us (rotation, or a device attached
    // after OBS connected). Nothing inside a source filter can change its own
    // output size; the graph has to be told so it rebuilds the connection.
    void NotifyGeometryChanged(int width, int height);

private:
    LONG ref_ = 1;
    FILTER_STATE state_ = State_Stopped;
    IReferenceClock* clock_ = nullptr;
    REFERENCE_TIME run_start_ = 0;
    CWirePlayPin* pin_ = nullptr;
    wp::FrameBus bus_;
    IFilterGraph* graph_ = nullptr;
    // IMediaEventSink is implemented by the Filter Graph Manager, not by us. It
    // is obtained from the graph when it hands itself over in JoinFilterGraph;
    // querying our own interfaces for it always returns E_NOINTERFACE.
    IMediaEventSink* event_sink_ = nullptr;
    WCHAR name_[128] = {};
    CRITICAL_SECTION lock_;
};

// ---------------------------------------------------------------------------
// The output pin. Delivers one frame per call from the frame bus.
// ---------------------------------------------------------------------------
class CWirePlayPin : public IPin, public IAMStreamConfig {
public:
    explicit CWirePlayPin(CWirePlayFilter* filter);
    ~CWirePlayPin();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** ppPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    // IAMStreamConfig: how OBS/Streamlabs negotiate with a capture pin.
    // Without it they enumerate the device, fail to query a single
    // capability, and silently drop it from the list.
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // Called by the filter while it is running.
    bool StartThread();
    void StopThread();
    void DeliverLoop();

    HANDLE thread_ = nullptr;
    volatile LONG stop_ = 0;

private:
    // `seq_out`, when given, receives the bus sequence of the frame delivered
    // (left untouched when a placeholder went out instead).
    HRESULT DeliverOne(uint64_t* seq_out = nullptr);
    // The geometry OBS should be offered: whatever the bus is publishing
    // right now, not the default the pin was constructed with. Connect-time
    // staleness here is what made OBS lock onto 1080x1920 forever.
    wp::FrameFormat CurrentFormat();
    HRESULT AgreeMediaType(IPin* peer, const AM_MEDIA_TYPE* pmt, AM_MEDIA_TYPE* agreed);
    HRESULT CompleteConnect(IPin* peer);
    HRESULT DecideAllocator(IMemInputPin* pInput, IMemAllocator** ppAlloc);
    void FillWaiting(IMediaSample* sample, size_t bytes);

    LONG ref_ = 1;
    CWirePlayFilter* filter_;
    IPin* peer_ = nullptr;
    IMemInputPin* input_ = nullptr;
    IMemAllocator* alloc_ = nullptr;
    AM_MEDIA_TYPE connected_ {};
    bool have_connected_ = false;
    wp::FrameFormat source_ {};
    int last_width_ = 0;
    int last_height_ = 0;
    uint64_t delivered_ = 0;
    uint64_t waiting_ = 0;
    std::vector<uint8_t> scratch_;
};

// ---------------------------------------------------------------------------
// One-type enumerator. The camera offers exactly one media type: the phone's
// geometry as 32-bit RGB. Consumers that want something else get refused and
// OBS falls back to converting, which it does well.
// ---------------------------------------------------------------------------
class COneTypeEnum : public IEnumMediaTypes {
public:
    COneTypeEnum(const AM_MEDIA_TYPE& type) : type_(type) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return ULONG(n);
    }
    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        ULONG fetched = 0;
        while (fetched < cMediaTypes && pos_ == 0) {
            AM_MEDIA_TYPE* out = static_cast<AM_MEDIA_TYPE*>(
                CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (!out) break;
            *out = type_;
            if (type_.cbFormat > 0 && type_.pbFormat) {
                out->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(type_.cbFormat));
                if (!out->pbFormat) {
                    CoTaskMemFree(out);
                    break;
                }
                memcpy(out->pbFormat, type_.pbFormat, type_.cbFormat);
            }
            ppMediaTypes[fetched++] = out;
            pos_ = 1;
        }
        if (pcFetched) *pcFetched = fetched;
        return fetched == cMediaTypes ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) override { pos_ += int(c); return pos_ > 1 ? S_FALSE : S_OK; }
    STDMETHODIMP Reset() override {
        pos_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        auto* e = new (std::nothrow) COneTypeEnum(type_);
        if (!e) return E_OUTOFMEMORY;
        e->pos_ = pos_;
        *ppEnum = e;
        return S_OK;
    }

private:
    AM_MEDIA_TYPE type_;
    LONG ref_ = 1;
    int pos_ = 0;
};

// ---------------------------------------------------------------------------
// Registration, without the baseclasses helper.
// ---------------------------------------------------------------------------
static HRESULT WriteRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value_name,
                                   const wchar_t* value) {
    HKEY key = nullptr;
    LONG r = RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    const DWORD bytes = DWORD((wcslen(value) + 1) * sizeof(wchar_t));
    r = RegSetValueExW(key, value_name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(r);
}

static HRESULT RegisterInprocServer(BOOL add) {
    wchar_t clsid[64] = {};
    StringFromGUID2(CLSID_WirePlayCamera, clsid, 64);

    std::wstring base = std::wstring(L"CLSID\\") + clsid;
    if (!add) {
        RegDeleteTreeW(HKEY_CLASSES_ROOT, base.c_str());
        return S_OK;
    }
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hInstance, path, MAX_PATH) == 0) return E_FAIL;

    HRESULT hr = WriteRegistryString(HKEY_CLASSES_ROOT, base.c_str(), nullptr, kFriendlyName);
    if (FAILED(hr)) return hr;
    const std::wstring inproc = base + L"\\InprocServer32";
    hr = WriteRegistryString(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr, path);
    if (FAILED(hr)) return hr;
    return WriteRegistryString(HKEY_CLASSES_ROOT, inproc.c_str(), L"ThreadingModel", L"Both");
}

STDAPI DllRegisterServer() {
    HRESULT hr = RegisterInprocServer(TRUE);
    if (FAILED(hr)) return hr;

    // Manual category instance. IFilterMapper2::RegisterFilter below should
    // create this too, but in practice several applications (Streamlabs
    // included) enumerate capture devices by walking
    // HKCR\CLSID\<VideoInputDeviceCategory>\Instance\<filter CLSID>, and the
    // mapper's entry alone left the camera invisible there. Writing the
    // instance explicitly -- FriendlyName + CLSID -- is what most virtual
    // cameras (DroidCam etc.) do.
    {
        wchar_t clsid[64] = {};
        StringFromGUID2(CLSID_WirePlayCamera, clsid, 64);
        wchar_t category[64] = {};
        StringFromGUID2(CLSID_VideoInputDeviceCategory, category, 64);
        const std::wstring inst = std::wstring(L"CLSID\\") + category + L"\\Instance\\" + clsid;
        hr = WriteRegistryString(HKEY_CLASSES_ROOT, inst.c_str(), L"FriendlyName", kFriendlyName);
        if (SUCCEEDED(hr))
            hr = WriteRegistryString(HKEY_CLASSES_ROOT, inst.c_str(), L"CLSID", clsid);
        if (FAILED(hr)) return hr;
    }

    // IFilterMapper2 puts the filter under CLSID_VideoInputDeviceCategory, which
    // is the key OBS reads when it builds its video-capture device list.
    IFilterMapper2* fm = nullptr;
    hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                          reinterpret_cast<void**>(&fm));
    if (FAILED(hr)) return hr;
    fm->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, CLSID_WirePlayCamera);
    // prf2 must NOT be null when a category is given: FilterMapper2 reads
    // the merit and pin list out of it to build the category entry, and
    // passing nullptr crashed regsvr32 with 0xC0000005 (field-proven,
    // 2026-10-22 -- the trace stopped right here). One output video pin is
    // all the mapper needs; the pin itself negotiates real media types.
    static const REGPINTYPES kPinTypes[] = {{&MEDIATYPE_Video, &GUID_NULL}};
    static const REGFILTERPINS kPins[] = {
        {const_cast<LPWSTR>(L"Video"), FALSE, TRUE, FALSE, FALSE,
         &GUID_NULL, nullptr, 1, kPinTypes},
    };
    REGFILTER2 rf2 = {};
    rf2.dwVersion = 1;
    rf2.dwMerit = 0x00800000;  // MERIT_DEFAULT
    rf2.cPins = 1;
    rf2.rgPins = kPins;
    hr = fm->RegisterFilter(CLSID_WirePlayCamera, kFriendlyName, nullptr,
                            &CLSID_VideoInputDeviceCategory, kFriendlyName, &rf2);
    fm->Release();
    return hr;
}

STDAPI DllUnregisterServer() {
    {
        wchar_t clsid[64] = {};
        StringFromGUID2(CLSID_WirePlayCamera, clsid, 64);
        wchar_t category[64] = {};
        StringFromGUID2(CLSID_VideoInputDeviceCategory, category, 64);
        const std::wstring inst = std::wstring(L"CLSID\\") + category + L"\\Instance\\" + clsid;
        RegDeleteTreeW(HKEY_CLASSES_ROOT, inst.c_str());
    }
    IFilterMapper2* fm = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFilterMapper2, reinterpret_cast<void**>(&fm)))) {
        fm->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, CLSID_WirePlayCamera);
        fm->Release();
    }
    return RegisterInprocServer(FALSE);
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_WirePlayCamera) return CLASS_E_CLASSNOTAVAILABLE;

    // Minimal class factory: one class, no locking ceremony beyond COM's own.
    class Factory : public IClassFactory {
    public:
        STDMETHODIMP QueryInterface(REFIID riid2, void** out) override {
            if (!out) return E_POINTER;
            if (riid2 == IID_IUnknown || riid2 == IID_IClassFactory) {
                *out = static_cast<IClassFactory*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
        STDMETHODIMP_(ULONG) Release() override {
            const LONG n = InterlockedDecrement(&ref_);
            if (n == 0) delete this;
            return ULONG(n);
        }
        STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid2, void** out) override {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (outer) return CLASS_E_NOAGGREGATION;
            auto* f = new (std::nothrow) CWirePlayFilter();
            if (!f) return E_OUTOFMEMORY;
            const HRESULT hr = f->QueryInterface(riid2, out);
            f->Release();
            return hr;
        }
        STDMETHODIMP LockServer(BOOL) override { return S_OK; }

    private:
        LONG ref_ = 1;
    };

    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_object_count == 0 ? S_OK : S_FALSE; }

// ---------------------------------------------------------------------------
// Media type construction. The geometry comes from the phone when we know it and
// from the advertised default before that.
// ---------------------------------------------------------------------------
static void BuildMediaType(AM_MEDIA_TYPE* pmt, const wp::FrameFormat& f) {
    FreeMediaType(pmt);
    ZeroMemory(pmt, sizeof(*pmt));
    pmt->majortype = MEDIATYPE_Video;
    pmt->subtype = MEDIASUBTYPE_RGB32;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->formattype = FORMAT_VideoInfo;
    pmt->lSampleSize = ULONG(wp::frameBytes(f));

    const wp::BitmapInfoHeader bih = wp::makeBitmapInfoHeader(f);
    VIDEOINFOHEADER vih {};
    // memcpy rather than a reinterpret_cast: the two structs have the same
    // layout, but punning between them is undefined behaviour and the compiler
    // is entitled to assume the objects do not alias.
    static_assert(sizeof(bih) == sizeof(vih.bmiHeader), "BITMAPINFOHEADER layout drifted");
    memcpy(&vih.bmiHeader, &bih, sizeof(vih.bmiHeader));
    vih.AvgTimePerFrame = wp::frameInterval100ns(f);
    if (f.fps_num > 0 && f.fps_den > 0) {
        vih.dwBitRate = DWORD(wp::frameBytes(f) * 8 * uint64_t(f.fps_num) / uint64_t(f.fps_den));
    }
    pmt->cbFormat = sizeof(vih);
    pmt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(vih)));
    if (pmt->pbFormat) memcpy(pmt->pbFormat, &vih, sizeof(vih));
}

static void CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
    FreeMediaType(dst);
    *dst = *src;
    dst->pbFormat = nullptr;
    if (src->cbFormat > 0 && src->pbFormat) {
        dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src->cbFormat));
        if (dst->pbFormat) memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    }
}

static void FreeMediaType(AM_MEDIA_TYPE* pmt) {
    if (pmt && pmt->pbFormat) {
        CoTaskMemFree(pmt->pbFormat);
        pmt->pbFormat = nullptr;
        pmt->cbFormat = 0;
    }
}

static wp::FrameFormat FormatFromMediaType(const AM_MEDIA_TYPE* pmt) {
    wp::FrameFormat f {};
    if (!pmt || pmt->formattype != FORMAT_VideoInfo || pmt->cbFormat < sizeof(VIDEOINFOHEADER)) {
        return f;
    }
    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
    const int h = vih->bmiHeader.biHeight;
    f.width = vih->bmiHeader.biWidth;
    f.height = h < 0 ? -h : h;
    if (vih->AvgTimePerFrame > 0) {
        f.fps_num = int(10000000000LL / vih->AvgTimePerFrame);
        f.fps_den = 1000;
    }
    return f;
}

// ---------------------------------------------------------------------------
// CWirePlayFilter
// ---------------------------------------------------------------------------
CWirePlayFilter::CWirePlayFilter() {
    InitializeCriticalSection(&lock_);
    InterlockedIncrement(&g_object_count);
    pin_ = new (std::nothrow) CWirePlayPin(this);
    FilterDbg("filter created");
}

CWirePlayFilter::~CWirePlayFilter() {
    if (pin_) pin_->Release();
    if (clock_) clock_->Release();
    if (event_sink_) event_sink_->Release();
    if (graph_) graph_->Release();
    bus_.close();
    DeleteCriticalSection(&lock_);
    InterlockedDecrement(&g_object_count);
}

STDMETHODIMP CWirePlayFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter ||
        riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CWirePlayFilter::AddRef() { return ULONG(InterlockedIncrement(&ref_)); }

STDMETHODIMP_(ULONG) CWirePlayFilter::Release() {
    const LONG n = InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return ULONG(n);
}

STDMETHODIMP CWirePlayFilter::GetClassID(CLSID* pClsid) {
    if (!pClsid) return E_POINTER;
    *pClsid = CLSID_WirePlayCamera;
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::Stop() {
    EnterCriticalSection(&lock_);
    state_ = State_Stopped;
    LeaveCriticalSection(&lock_);
    if (pin_) pin_->StopThread();
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::Pause() {
    EnterCriticalSection(&lock_);
    if (state_ == State_Stopped) state_ = State_Paused;
    LeaveCriticalSection(&lock_);
    // Start delivering from Pause, not only from Run. Field evidence: a
    // FilterGraph run left this filter Paused (its Run was never called)
    // while reporting itself Running -- the delivery thread never started
    // and every consumer showed black. Push sources conventionally start
    // their thread in Pause, once the allocator is committed; Run then
    // has nothing left to do. StartThread is idempotent.
    FilterDbg("filter Pause()");
    if (pin_) pin_->StartThread();
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::Run(REFERENCE_TIME tStart) {
    EnterCriticalSection(&lock_);
    state_ = State_Running;
    run_start_ = tStart;
    LeaveCriticalSection(&lock_);
    // A source has no input pin to be pulled by, so it needs its own delivery
    // thread. Run() itself must return immediately: blocking here would hang
    // the whole graph, and OBS would look frozen rather than broken.
    FilterDbg("filter Run()");
    if (pin_ && !pin_->StartThread()) { FilterDbg("StartThread FAILED"); return E_FAIL; }
    FilterDbg("delivery thread started");
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::GetState(DWORD, FILTER_STATE* pState) {
    if (!pState) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pState = state_;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::SetSyncSource(IReferenceClock* pClock) {
    EnterCriticalSection(&lock_);
    if (clock_) clock_->Release();
    clock_ = pClock;
    if (clock_) clock_->AddRef();
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::GetSyncSource(IReferenceClock** ppClock) {
    if (!ppClock) return E_POINTER;
    EnterCriticalSection(&lock_);
    *ppClock = clock_;
    if (clock_) clock_->AddRef();
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    // One output pin, enumerated by a tiny array-backed enumerator.
    class PinEnum : public IEnumPins {
    public:
        explicit PinEnum(IPin* p) : pin_(p) {
            if (pin_) pin_->AddRef();
        }
        // No `override`: IEnumPins derives from IUnknown, which has no virtual
        // destructor, so there is nothing to override. Release() below does
        // `delete this` with `this` statically typed PinEnum, which is
        // well-defined even through a base with a non-virtual destructor.
        ~PinEnum() {
            if (pin_) pin_->Release();
        }
        STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
            if (!out) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IEnumPins) {
                *out = static_cast<IEnumPins*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
        STDMETHODIMP_(ULONG) Release() override {
            const LONG n = InterlockedDecrement(&ref_);
            if (n == 0) delete this;
            return ULONG(n);
        }
        STDMETHODIMP Next(ULONG c, IPin** pins, ULONG* fetched) override {
            if (!pins) return E_POINTER;
            ULONG n = 0;
            while (n < c && pos_ == 0 && pin_) {
                pins[n++] = pin_;
                pin_->AddRef();
                pos_ = 1;
            }
            if (fetched) *fetched = n;
            return n == c ? S_OK : S_FALSE;
        }
        STDMETHODIMP Skip(ULONG c) override {
            pos_ += int(c);
            return pos_ > 1 ? S_FALSE : S_OK;
        }
        STDMETHODIMP Reset() override {
            pos_ = 0;
            return S_OK;
        }
        STDMETHODIMP Clone(IEnumPins** out) override {
            if (!out) return E_POINTER;
            auto* e = new (std::nothrow) PinEnum(pin_);
            if (!e) return E_OUTOFMEMORY;
            e->pos_ = pos_;
            *out = e;
            return S_OK;
        }

    private:
        IPin* pin_;
        LONG ref_ = 1;
        int pos_ = 0;
    };

    auto* e = new (std::nothrow) PinEnum(pin_);
    if (!e) return E_OUTOFMEMORY;
    *ppEnum = e;
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::FindPin(LPCWSTR id, IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    *ppPin = nullptr;
    if (!id || wcscmp(id, L"Output") != 0 || !pin_) return VFW_E_NOT_FOUND;
    *ppPin = pin_;
    pin_->AddRef();
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    wcsncpy_s(pInfo->achName, 128, kFriendlyName, _TRUNCATE);
    pInfo->pGraph = graph_;
    if (pInfo->pGraph) pInfo->pGraph->AddRef();
    return S_OK;
}

// The graph hands itself over here. We keep it only so that QueryFilterInfo can
// report it; the filter does not call back into the graph except through
// IMediaEventSink, which is queried on demand.
STDMETHODIMP CWirePlayFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    EnterCriticalSection(&lock_);
    if (event_sink_) {
        event_sink_->Release();
        event_sink_ = nullptr;
    }
    if (graph_) graph_->Release();
    graph_ = pGraph;
    if (graph_) {
        graph_->AddRef();
        // Not fatal if the graph does not expose it: we would lose the
        // rotation notification, nothing else.
        graph_->QueryInterface(IID_IMediaEventSink, reinterpret_cast<void**>(&event_sink_));
    }
    if (pName) wcsncpy_s(name_, 128, pName, _TRUNCATE);
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP CWirePlayFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    const wchar_t* vendor = L"WirePlay";
    const size_t bytes = (wcslen(vendor) + 1) * sizeof(wchar_t);
    auto* buf = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
    if (!buf) return E_OUTOFMEMORY;
    memcpy(buf, vendor, bytes);
    *pVendorInfo = buf;
    return S_OK;
}

void CWirePlayFilter::NotifyGeometryChanged(int width, int height) {
    // EC_VIDEO_SIZE_CHANGED is what tells the graph to reconnect at the new
    // size. Nothing in a source filter can resize its own output from inside
    // the delivery loop.
    EnterCriticalSection(&lock_);
    if (event_sink_) {
        // IMediaEventSink::Notify, not NotifyEvent: NotifyEvent is a helper on
        // the SDK's CBaseFilter, and this filter does not use the baseclasses.
        // See learn.microsoft.com/windows/win32/api/strmif/nf-strmif-imediaeventsink-notify
        event_sink_->Notify(EC_VIDEO_SIZE_CHANGED,
                            LONG_PTR(MAKELPARAM(width & 0xFFFF, height & 0xFFFF)), 0);
    }
    LeaveCriticalSection(&lock_);
}

// ---------------------------------------------------------------------------
// CWirePlayPin
// ---------------------------------------------------------------------------
CWirePlayPin::CWirePlayPin(CWirePlayFilter* filter) : filter_(filter) {
    InterlockedIncrement(&g_object_count);
    // The filter holds the only reference, and it is constructed in the
    // filter's constructor, so the pin starts alive without an AddRef dance.
}

CWirePlayPin::~CWirePlayPin() {
    StopThread();
    if (alloc_) alloc_->Release();
    if (input_) input_->Release();
    if (peer_) peer_->Release();
    FreeMediaType(&connected_);
    InterlockedDecrement(&g_object_count);
}

STDMETHODIMP CWirePlayPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CWirePlayPin::AddRef() { return ULONG(InterlockedIncrement(&ref_)); }

STDMETHODIMP_(ULONG) CWirePlayPin::Release() {
    const LONG n = InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return ULONG(n);
}

STDMETHODIMP CWirePlayPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    wcsncpy_s(pInfo->achName, 128, L"Output", _TRUNCATE);
    pInfo->dir = PINDIR_OUTPUT;
    pInfo->pFilter = filter_;
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    return S_OK;
}

STDMETHODIMP CWirePlayPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP CWirePlayPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    const wchar_t* id = L"Output";
    const size_t bytes = (wcslen(id) + 1) * sizeof(wchar_t);
    auto* buf = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
    if (!buf) return E_OUTOFMEMORY;
    memcpy(buf, id, bytes);
    *Id = buf;
    return S_OK;
}

wp::FrameFormat CWirePlayPin::CurrentFormat() {
    wp::FrameBus& bus = filter_->bus();
    if (!bus.valid()) bus.open(wp::defaultBusName());
    if (bus.valid() && bus.width() > 0 && bus.height() > 0) {
        source_ = {int(bus.width()), int(bus.height()),
                   int(bus.fpsNum() ? bus.fpsNum() : 60),
                   int(bus.fpsDen() ? bus.fpsDen() : 1)};
    }
    return (source_.width > 0 && source_.height > 0) ? source_
                                                     : wp::defaultCameraFormat();
}

STDMETHODIMP CWirePlayPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    const bool is_video = pmt->majortype == MEDIATYPE_Video;
    const bool is_rgb32 = pmt->subtype == MEDIASUBTYPE_RGB32;
    const wp::FrameFormat proposed = FormatFromMediaType(pmt);
    // kAcceptWithOurSize still answers "yes": QueryAccept is a yes/no question,
    // and the geometry we insist on is what GetMediaType hands back.
    return wp::judgeMediaType(is_video, is_rgb32, proposed, CurrentFormat()) ==
                   wp::kReject
               ? S_FALSE
               : S_OK;
}

STDMETHODIMP CWirePlayPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    AM_MEDIA_TYPE mt {};
    BuildMediaType(&mt, CurrentFormat());
    auto* e = new (std::nothrow) COneTypeEnum(mt);
    FreeMediaType(&mt);
    if (!e) return E_OUTOFMEMORY;
    *ppEnum = e;
    return S_OK;
}

STDMETHODIMP CWirePlayPin::ConnectedTo(IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    *ppPin = peer_;
    if (!peer_) return VFW_E_NOT_CONNECTED;
    peer_->AddRef();
    return S_OK;
}

STDMETHODIMP CWirePlayPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (!have_connected_) return VFW_E_NOT_CONNECTED;
    CopyMediaType(pmt, &connected_);
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAMStreamConfig. One capability: the phone's current geometry as RGB32.
// ---------------------------------------------------------------------------
STDMETHODIMP CWirePlayPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return S_OK;  // reset to the pin's own choice
    return QueryAccept(pmt) == S_OK ? S_OK : VFW_E_INVALIDMEDIATYPE;
}

STDMETHODIMP CWirePlayPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return E_OUTOFMEMORY;
    ZeroMemory(mt, sizeof(*mt));
    BuildMediaType(mt, CurrentFormat());
    *ppmt = mt;
    return S_OK;
}

STDMETHODIMP CWirePlayPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = 1;
    *piSize = int(sizeof(VIDEO_STREAM_CONFIG_CAPS));
    return S_OK;
}

STDMETHODIMP CWirePlayPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt) return E_POINTER;
    if (iIndex != 0) return S_FALSE;
    const HRESULT hr = GetFormat(ppmt);
    if (FAILED(hr)) return hr;
    if (pSCC) {
        auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(pSCC);
        ZeroMemory(caps, sizeof(*caps));
        const wp::FrameFormat f = FormatFromMediaType(*ppmt);
        caps->guid = FORMAT_VideoInfo;
        caps->InputSize = SIZE{f.width, f.height};
        caps->MinCroppingSize = caps->InputSize;
        caps->MaxCroppingSize = caps->InputSize;
        caps->CropGranularityX = 1;
        caps->CropGranularityY = 1;
        caps->MinOutputSize = caps->InputSize;
        caps->MaxOutputSize = caps->InputSize;
        caps->OutputGranularityX = 1;
        caps->OutputGranularityY = 1;
        const int64_t interval = wp::frameInterval100ns(f);
        caps->MinFrameInterval = interval;
        caps->MaxFrameInterval = interval;
        if (f.fps_num > 0 && f.fps_den > 0) {
            const long bps = long(wp::frameBytes(f) * 8 * uint64_t(f.fps_num) /
                                  uint64_t(f.fps_den));
            caps->MinBitsPerSecond = bps;
            caps->MaxBitsPerSecond = bps;
        }
    }
    return S_OK;
}

STDMETHODIMP CWirePlayPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) {
    return VFW_E_INVALID_DIRECTION;  // this pin only outputs
}

STDMETHODIMP CWirePlayPin::QueryInternalConnections(IPin**, ULONG* nPin) {
    if (nPin) *nPin = 0;
    return E_NOTIMPL;  // no partially-connected internal pins to report
}

STDMETHODIMP CWirePlayPin::EndOfStream() {
    return E_UNEXPECTED;  // end-of-stream arrives at input pins, not here
}

STDMETHODIMP CWirePlayPin::BeginFlush() {
    return E_UNEXPECTED;  // flushing is an input-pin operation
}

STDMETHODIMP CWirePlayPin::EndFlush() {
    return E_UNEXPECTED;
}

STDMETHODIMP CWirePlayPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) {
    return E_UNEXPECTED;  // segment notices go to input pins
}

STDMETHODIMP CWirePlayPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    if (peer_) return VFW_E_ALREADY_CONNECTED;

    AM_MEDIA_TYPE agreed {};
    HRESULT hr = AgreeMediaType(pReceivePin, pmt, &agreed);
    if (FAILED(hr)) {
        FreeMediaType(&agreed);
        return hr;
    }

    hr = pReceivePin->ReceiveConnection(static_cast<IPin*>(this), &agreed);
    if (FAILED(hr)) {
        FilterDbg("ReceiveConnection FAILED hr=0x%08lX", hr);
        FreeMediaType(&agreed);
        return hr;
    }
    CopyMediaType(&connected_, &agreed);
    FreeMediaType(&agreed);
    have_connected_ = true;
    FilterDbg("pin connected; negotiating allocator");

    peer_ = pReceivePin;
    peer_->AddRef();
    if (SUCCEEDED(peer_->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&input_))) &&
        input_) {
        hr = CompleteConnect(peer_);
    } else {
        hr = VFW_E_NO_TRANSPORT;
    }
    if (FAILED(hr)) Disconnect();
    return hr;
}

HRESULT CWirePlayPin::AgreeMediaType(IPin* peer, const AM_MEDIA_TYPE* pmt, AM_MEDIA_TYPE* agreed) {
    // If the caller proposed a type we accept, use it.
    const wp::FrameFormat f =
        (source_.width > 0 && source_.height > 0) ? source_ : wp::defaultCameraFormat();
    // A proposed type is only usable if it carries real geometry. The
    // Sample Grabber (and some graph builders) propose a FORMAT_VideoInfo
    // shell with no VIDEOINFOHEADER behind it; accepting that used to
    // "connect" the pin at 0x0, after which DeliverOne refused every
    // frame and consumers showed black forever.
    const wp::FrameFormat proposed_fmt = FormatFromMediaType(pmt);
    if (pmt && pmt->majortype != GUID_NULL && proposed_fmt.width > 0 &&
        proposed_fmt.height > 0 && S_OK == QueryAccept(pmt)) {
        CopyMediaType(agreed, pmt);
        return S_OK;
    }
    // Otherwise offer ours and let the peer say no.
    BuildMediaType(agreed, f);
    if (peer->QueryAccept(agreed) == S_OK) return S_OK;

    // Last resort: walk what the peer will take and see if any of it is ours.
    IEnumMediaTypes* penum = nullptr;
    if (FAILED(peer->EnumMediaTypes(&penum)) || !penum) return VFW_E_NO_ACCEPTABLE_TYPES;
    AM_MEDIA_TYPE* candidate = nullptr;
    ULONG fetched = 0;
    HRESULT hr = VFW_E_NO_ACCEPTABLE_TYPES;
    while (penum->Next(1, &candidate, &fetched) == S_OK && fetched == 1) {
        const wp::FrameFormat cand_fmt = FormatFromMediaType(candidate);
        if (candidate->majortype == MEDIATYPE_Video &&
            candidate->subtype == MEDIASUBTYPE_RGB32 && cand_fmt.width > 0 &&
            cand_fmt.height > 0) {
            CopyMediaType(agreed, candidate);
            hr = S_OK;
        }
        FreeMediaType(candidate);
        CoTaskMemFree(candidate);
        candidate = nullptr;
        if (hr == S_OK) break;
    }
    penum->Release();
    return hr;
}

HRESULT CWirePlayPin::DecideAllocator(IMemInputPin* pInput, IMemAllocator** ppAlloc) {
    // Ask the input pin first: renderers usually have a preference, and
    // honouring it avoids a copy on their side.
    ALLOCATOR_PROPERTIES prop {};
    // Use our OWN allocator, always. Borrowing the downstream pin's
    // allocator proved unreliable: the Sample Grabber lends the allocator
    // of its (unconnected) output side, and GetBuffer on it blocked for
    // the whole graph run despite a successful Commit (field log). An
    // allocator we create, size and commit ourselves has no such games.
    pInput->GetAllocatorRequirements(&prop);
    *ppAlloc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IMemAllocator, reinterpret_cast<void**>(ppAlloc));
    if (FAILED(hr)) return hr;
    if (prop.cBuffers < 4) prop.cBuffers = 4;
    prop.cbBuffer = LONG(connected_.lSampleSize);
    if (prop.cbAlign < 1) prop.cbAlign = 1;
    prop.cbPrefix = 0;

    ALLOCATOR_PROPERTIES actual {};
    hr = (*ppAlloc)->SetProperties(&prop, &actual);
    if (FAILED(hr)) return hr;
    if (actual.cbBuffer < LONG(connected_.lSampleSize)) return E_FAIL;
    FilterDbg("allocator sized: cBuffers=%ld cbBuffer=%ld", actual.cBuffers, actual.cbBuffer);
    return S_OK;
}

HRESULT CWirePlayPin::CompleteConnect(IPin* peer) {
    (void)peer;
    if (alloc_) {
        alloc_->Release();
        alloc_ = nullptr;
    }
    HRESULT hr = DecideAllocator(input_, &alloc_);
    if (FAILED(hr)) return hr;
    hr = input_->NotifyAllocator(alloc_, FALSE);
    if (FAILED(hr)) return hr;
    // Without Commit the allocator hands out nothing: GetBuffer waits for
    // a commit that never comes (field log: one frame at graph stop, then
    // VFW_E_NOT_COMMITTED 0x80040211 forever). Filters built on the
    // base classes get this from CBaseOutputPin::Active; this filter is
    // hand-rolled, so it has to ask explicitly.
    hr = alloc_->Commit();
    if (FAILED(hr)) { FilterDbg("allocator Commit FAILED hr=0x%08lX", hr); return hr; }
    FilterDbg("allocator committed (build with Commit fix)");

    const wp::FrameFormat f = FormatFromMediaType(&connected_);
    scratch_.resize(wp::frameBytes(f));
    last_width_ = last_height_ = 0;
    return S_OK;
}

STDMETHODIMP CWirePlayPin::Disconnect() {
    if (alloc_) {
        alloc_->Decommit();
        alloc_->Release();
        alloc_ = nullptr;
    }
    if (input_) {
        input_->Release();
        input_ = nullptr;
    }
    if (peer_) {
        peer_->Release();
        peer_ = nullptr;
    }
    FreeMediaType(&connected_);
    have_connected_ = false;
    return S_OK;
}
// ---------------------------------------------------------------------------
void CWirePlayPin::FillWaiting(IMediaSample* sample, size_t bytes) {
    BYTE* dst = nullptr;
    if (FAILED(sample->GetPointer(&dst)) || !dst) return;
    // Dark grey, not black: a black frame in OBS is indistinguishable from a
    // broken connection, which sends the user debugging the wrong thing.
    for (size_t i = 0; i + 3 < bytes; i += 4) {
        dst[i] = 24;
        dst[i + 1] = 24;
        dst[i + 2] = 28;
        dst[i + 3] = 255;
    }
    sample->SetActualDataLength(LONG(bytes));
    ++waiting_;
}

HRESULT CWirePlayPin::DeliverOne(uint64_t* seq_out) {
    static volatile LONG dbg_calls = 0;
    const long call_no = InterlockedIncrement(&dbg_calls);
    if (!input_ || !alloc_ || !have_connected_) {
        if (call_no <= 5)
            FilterDbg("DeliverOne early-out: input=%p alloc=%p connected=%d",
                      (void*)input_, (void*)alloc_, int(have_connected_));
        return S_FALSE;
    }

    const wp::FrameFormat f = FormatFromMediaType(&connected_);
    if (f.width <= 0 || f.height <= 0) return S_FALSE;
    const size_t want = wp::frameBytes(f);

    if (call_no <= 2) FilterDbg("D%ld: enter, before GetBuffer", call_no);
    IMediaSample* sample = nullptr;
    HRESULT hr = alloc_->GetBuffer(&sample, nullptr, nullptr, 0);
    if (call_no <= 2) FilterDbg("D%ld: GetBuffer returned hr=0x%08lX", call_no, hr);
    if (hr == VFW_E_NOT_COMMITTED) {
        // The downstream pin can decommit "our" allocator after
        // NotifyAllocator; re-commit and retry once instead of waiting
        // forever for buffers that will never exist.
        FilterDbg("GetBuffer NOT_COMMITTED; re-committing");
        if (SUCCEEDED(alloc_->Commit()))
            hr = alloc_->GetBuffer(&sample, nullptr, nullptr, 0);
    }
    if (FAILED(hr) || !sample) {
        if (call_no <= 5) FilterDbg("GetBuffer failed hr=0x%08lX", hr);
        return S_FALSE;
    }

    // Timestamps are stream time: graph clock minus the tStart Run()
    // received. The absolute clock is machine uptime; stamping samples
    // with it schedules every frame ~uptime into the future, and
    // consumers that honour timestamps wait inside Receive until
    // teardown. While merely Paused (preroll before Run) leave the
    // sample unstamped -- renderers treat that as "show now".
    if (filter_->state() == State_Running && filter_->clock()) {
        REFERENCE_TIME now = 0;
        if (SUCCEEDED(filter_->clock()->GetTime(&now))) {
            REFERENCE_TIME start = now - filter_->run_start();
            if (start < 0) start = 0;
            REFERENCE_TIME stop = start + wp::frameInterval100ns(f);
            sample->SetTime(&start, &stop);  // SetTime takes non-const pointers
            sample->SetMediaTime(nullptr, nullptr);
        }
    }
    sample->SetSyncPoint(TRUE);
    sample->SetPreroll(FALSE);
    sample->SetDiscontinuity(FALSE);

    BYTE* dst = nullptr;
    if (FAILED(sample->GetPointer(&dst)) || !dst || size_t(sample->GetSize()) < want) {
        FillWaiting(sample, size_t(sample->GetSize() > 0 ? sample->GetSize() : 0));
        input_->Receive(sample);
        sample->Release();  // GetBuffer's reference is ours; Receive keeps its own
        return S_OK;
    }

    wp::FrameBus& bus = filter_->bus();
    if (!bus.valid() && !bus.open(wp::defaultBusName())) {
        // WirePlay is not running. Keep delivering at full rate so the graph
        // stays healthy and the user sees "waiting" instead of a frozen image.
        FillWaiting(sample, want);
        input_->Receive(sample);
        sample->Release();  // GetBuffer's reference is ours; Receive keeps its own
        return S_OK;
    }

    // Geometry can change under us: rotation, or a device attached after OBS
    // connected. Raise the event so the graph reconnects at the new size.
    const int w = int(bus.width()), h = int(bus.height());
    if (w > 0 && h > 0 && (w != last_width_ || h != last_height_)) {
        last_width_ = w;
        last_height_ = h;
        source_ = {w, h, int(bus.fpsNum() ? bus.fpsNum() : 60),
                   int(bus.fpsDen() ? bus.fpsDen() : 1)};
        filter_->NotifyGeometryChanged(w, h);
    }

    if (bus.stale(1500)) {
        FillWaiting(sample, want);
        input_->Receive(sample);
        sample->Release();  // GetBuffer's reference is ours; Receive keeps its own
        return S_OK;
    }

    if (scratch_.size() < want) scratch_.resize(want);
    int ow = 0, oh = 0, os = 0;
    if (call_no <= 2) FilterDbg("D%ld: got buffer, filling from bus", call_no);
    const unsigned long consume_t0 = GetTickCount();
    uint64_t consumed_seq = 0;
    const bool consumed = bus.consumeLatest(scratch_.data(), &ow, &oh, &os, &consumed_seq);
    if (call_no <= 5)
        FilterDbg("D%ld: consumeLatest ok=%d took=%llu ms", call_no, int(consumed),
                  (unsigned long long)(GetTickCount() - consume_t0));
    if (!consumed || ow != f.width || oh != f.height) {
        // Either nothing published yet, or the size moved between the check
        // above and the copy. The reconnect is already on its way.
        FillWaiting(sample, want);
        input_->Receive(sample);
        sample->Release();  // GetBuffer's reference is ours; Receive keeps its own
        return S_OK;
    }

    wp::copyTopDownToBottomUp(dst, scratch_.data(), ow, oh, size_t(os));
    sample->SetActualDataLength(LONG(want));
    ++delivered_;
    if (seq_out) *seq_out = consumed_seq;
    const unsigned long receive_t0 = GetTickCount();
    const HRESULT rcv = input_->Receive(sample);
    if (call_no <= 5)
        FilterDbg("D%ld: Receive took=%llu ms", call_no,
                  (unsigned long long)(GetTickCount() - receive_t0));
    sample->Release();  // GetBuffer's reference is ours; Receive keeps its own
    if (call_no <= 5 || delivered_ % 300 == 0)
        FilterDbg("delivered=%llu waiting=%llu receive hr=0x%08lX",
                  (unsigned long long)delivered_, (unsigned long long)waiting_, rcv);
    return S_OK;
}

static DWORD WINAPI DeliverThreadProc(LPVOID param) {
    static_cast<CWirePlayPin*>(param)->DeliverLoop();
    return 0;
}

bool CWirePlayPin::StartThread() {
    if (thread_) return true;
    InterlockedExchange(&stop_, 0);
    thread_ = CreateThread(nullptr, 0, DeliverThreadProc, this, 0, nullptr);
    return thread_ != nullptr;
}

void CWirePlayPin::StopThread() {
    InterlockedExchange(&stop_, 1);
    if (thread_) {
        // The loop wakes at least every 8 ms, so this cannot hang for long.
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
}

void CWirePlayPin::DeliverLoop() {
    FilterDbg("DeliverLoop enter: state=%d input=%p connected=%d",
              int(filter_->state()), (void*)input_, int(have_connected_));
    const wp::FrameFormat f = FormatFromMediaType(&connected_);
    REFERENCE_TIME interval = wp::frameInterval100ns(f);
    if (interval <= 0) interval = 166666;  // 60 fps
    const LONGLONG interval_us = interval / 10;

    LARGE_INTEGER freq {}, t0 {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    auto nowUs = [&]() -> LONGLONG {
        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);
        return (now.QuadPart - t0.QuadPart) * 1000000LL / (freq.QuadPart ? freq.QuadPart : 1);
    };

    // A live camera hands over a frame when the phone has produced one. It
    // used to re-sample the stream on a fixed wall-clock grid at the advertised
    // rate (59 fps in the field), repeating or skipping frames to fit: a
    // variable-rate source against a fixed grid beats, and the beat is a
    // periodic duplicated frame or a skipped one, a hitch every second or so.
    // Now the loop sleeps on the bus's frame event and delivers each new frame
    // as it lands. If the phone goes quiet, the newest frame is repeated at a
    // slow keep-alive so consumers that treat a silent device as dead (some
    // conferencing apps) keep it; a stall shorter than that is shown as the
    // stall it is, not papered over.
    const LONGLONG keepalive_us = 250 * 1000;
    uint64_t seen = 0;
    LONGLONG last_deliver_us = nowUs();
    LONGLONG next_tick_us = last_deliver_us;  // nominal-rate pacing for placeholders
    uint64_t new_frames = 0, keepalives = 0, placeholders = 0;
    LONGLONG window_start_us = last_deliver_us, worst_gap_us = 0;

    while (stop_ == 0 && filter_->state() != State_Stopped && input_ && have_connected_) {
        wp::FrameBus& bus = filter_->bus();
        if (bus.valid() && !bus.stale(1500)) {
            // Short slices, so Stop() never waits long for this thread.
            const uint64_t got = bus.waitForFrame(seen, 40);
            const LONGLONG t = nowUs();
            if (got != seen) {
                uint64_t used = 0;
                DeliverOne(&used);
                // Whatever happened to that copy, do not spin on the same
                // counter: the next frame is only a frame away.
                seen = used > got ? used : got;
                const LONGLONG gap = nowUs() - last_deliver_us;
                if (gap > worst_gap_us) worst_gap_us = gap;
                last_deliver_us = nowUs();
                ++new_frames;
            } else if (t - last_deliver_us >= keepalive_us) {
                DeliverOne();  // keep-alive: the newest frame again
                last_deliver_us = nowUs();
                ++keepalives;
            }
        } else {
            // No producer, or a silent one: deliver the "waiting" picture at
            // the nominal rate so the graph stays healthy.
            DeliverOne();
            ++placeholders;
            next_tick_us += interval_us;
            for (;;) {
                const LONGLONG t = nowUs();
                if (t >= next_tick_us || stop_ != 0) break;
                const DWORD ms = DWORD((next_tick_us - t) / 1000);
                Sleep(ms > 8 ? 8 : 1);
            }
            if (nowUs() - next_tick_us > 4 * interval_us) next_tick_us = nowUs();  // do not burst
            last_deliver_us = nowUs();
        }

        const LONGLONG t = nowUs();
        if (t - window_start_us >= 1000000) {
            FilterDbg("cadence: new=%llu keepalive=%llu waiting=%llu worst_gap=%.1f ms",
                      (unsigned long long)new_frames, (unsigned long long)keepalives,
                      (unsigned long long)placeholders, double(worst_gap_us) / 1000.0);
            new_frames = keepalives = placeholders = 0;
            worst_gap_us = 0;
            window_start_us = t;
        }
    }
    FilterDbg("DeliverLoop exit: stop=%ld state=%d input=%p connected=%d delivered=%llu",
              stop_, int(filter_->state()), (void*)input_, int(have_connected_),
              (unsigned long long)delivered_);
}

// ---------------------------------------------------------------------------
// DLL entry
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInstance = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}
