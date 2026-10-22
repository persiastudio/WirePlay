// DirectShow camera diagnostic (Windows only).
//
// Stands in for OBS/Streamlabs and walks the exact chain those apps walk:
//   1. enumerate CLSID_VideoInputDeviceCategory the way the OBS device
//      list does (ICreateDevEnum + IEnumMoniker, FriendlyName/CLSID);
//   2. instantiate the WirePlay filter by CLSID (CoCreateInstance);
//   3. ask its output pin for media types;
//   4. connect it to a Sample Grabber + Null Renderer, run the graph for
//      a few seconds and count delivered frames (needs wireplay.exe
//      running and mirroring, exactly like real use).
//
// Every stage prints PASS/FAIL with the HRESULT, so a failure says which
// link is broken instead of "the camera does not show up somewhere".

#include <windows.h>
#include <dshow.h>
#include <uuids.h>
#include <initguid.h>  // DEFINE_GUID below must define, not just declare

// qedit.h (Sample Grabber) no longer ships with current Windows SDKs, so
// the two interfaces and CLSID are declared here directly -- the GUIDs are
// the published DirectShow constants.
DEFINE_GUID(MyCLSID_SampleGrabber, 0xc1f400a4, 0x3f08, 0x11d3, 0x9f, 0x0b, 0x00, 0x60,
            0x08, 0x03, 0x9e, 0x37);

// Likewise not declared by name in current SDK headers.
DEFINE_GUID(MyCLSID_NullRenderer, 0x36a5f770, 0xfe4c, 0x11ce, 0xa8, 0xed, 0x00, 0xaa,
            0x00, 0x2f, 0xea, 0xb5);

struct __declspec(uuid("0579154a-2b53-4994-b0d0-e773148eff85")) ISampleGrabberCB
    : public IUnknown {
    STDMETHOD(SampleCB)(double, IMediaSample*) PURE;
    STDMETHOD(BufferCB)(double, BYTE*, long) PURE;
};

struct __declspec(uuid("6b652fff-11fe-4fce-92ad-0266b5d7c78f")) ISampleGrabber
    : public IUnknown {
    STDMETHOD(SetOneShot)(BOOL) PURE;
    STDMETHOD(SetMediaType)(const AM_MEDIA_TYPE*) PURE;
    STDMETHOD(GetConnectedMediaType)(AM_MEDIA_TYPE*) PURE;
    STDMETHOD(SetBufferSamples)(BOOL) PURE;
    STDMETHOD(GetCurrentBuffer)(long*, long*) PURE;
    STDMETHOD(GetCurrentSample)(IMediaSample**) PURE;
    STDMETHOD(SetCallback)(ISampleGrabberCB*, long) PURE;
};

#include <algorithm>
#include <cstdio>
#include <string>


DEFINE_GUID(CLSID_WirePlayCamera, 0x8e14549a, 0xdb61, 0x4309, 0xaf, 0xa1, 0x3a, 0x3b,
            0x5c, 0x6e, 0x7f, 0x2c);

namespace {

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(std::max(0, n - 1), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

void PrintHr(const char* what, HRESULT hr) {
    std::printf("  %-52s hr=0x%08lX %s\n", what, unsigned long(hr),
                SUCCEEDED(hr) ? "OK" : "FAIL");
    std::fflush(stdout);
}

void Step(const char* what) {
    std::printf("    .. %s\n", what);
    std::fflush(stdout);
}

std::wstring ReadFriendlyName(IMoniker* moniker) {
    IPropertyBag* bag = nullptr;
    if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) || !bag)
        return {};
    VARIANT v;
    VariantInit(&v);
    std::wstring name;
    if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR)
        name = v.bstrVal ? v.bstrVal : L"";
    VariantClear(&v);
    bag->Release();
    return name;
}

bool RegistryInstancePresent() {
    wchar_t clsid[64] = {};
    StringFromGUID2(CLSID_WirePlayCamera, clsid, 64);
    wchar_t category[64] = {};
    StringFromGUID2(CLSID_VideoInputDeviceCategory, category, 64);
    const std::wstring sub =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + category + L"\\Instance\\" + clsid;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

class GrabberCB : public ISampleGrabberCB {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
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
    STDMETHODIMP SampleCB(double, IMediaSample* sample) override {
        ++frames;
        if (sample) bytes += sample->GetActualDataLength();
        return S_OK;
    }
    STDMETHODIMP BufferCB(double, BYTE*, long size) override {
        ++frames;
        bytes += size;
        return S_OK;
    }
    volatile LONG frames = 0;
    volatile LONG bytes = 0;

private:
    LONG ref_ = 1;
};

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 4;
    std::printf("dshow_probe: DirectShow virtual-camera diagnostic\n");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        PrintHr("CoInitializeEx", hr);
        return 1;
    }

    std::printf("[registry] category Instance key for our CLSID: %s\n",
                RegistryInstancePresent() ? "PRESENT" : "MISSING");

    // ---- Stage 1: enumerate the video-input category --------------------
    std::printf("[stage 1] enumerate video capture devices\n");
    ICreateDevEnum* dev_enum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&dev_enum));
    PrintHr("CoCreateInstance(SystemDeviceEnum)", hr);
    bool listed = false;
    IBaseFilter* filter = nullptr;
    if (SUCCEEDED(hr)) {
        IEnumMoniker* enum_mon = nullptr;
        hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_mon, 0);
        PrintHr("CreateClassEnumerator(VideoInputDeviceCategory)", hr);
        if (SUCCEEDED(hr) && enum_mon) {
            IMoniker* moniker = nullptr;
            while (enum_mon->Next(1, &moniker, nullptr) == S_OK) {
                const std::wstring name = ReadFriendlyName(moniker);
                std::printf("    device: %s\n", WideToUtf8(name.c_str()).c_str());
                if (name == L"WirePlay iPhone") {
                    listed = true;
                    hr = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter));
                    PrintHr("BindToObject(WirePlay iPhone)", hr);
                }
                moniker->Release();
            }
            enum_mon->Release();
        }
        dev_enum->Release();
    }
    std::printf("  -> WirePlay iPhone %s\n", listed ? "IS listed" : "is NOT listed");

    // ---- Stage 2: instantiate by CLSID if the moniker path failed -------
    if (!filter) {
        std::printf("[stage 2] CoCreateInstance(CLSID_WirePlayCamera)\n");
        hr = CoCreateInstance(CLSID_WirePlayCamera, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&filter));
        PrintHr("CoCreateInstance", hr);
    }

    if (!filter) {
        std::printf("RESULT: filter cannot be instantiated at all.\n");
        CoUninitialize();
        return 2;
    }

    // ---- Stage 3: media types on the output pin --------------------------
    std::printf("[stage 3] output pin media types\n");
    IEnumPins* enum_pins = nullptr;
    hr = filter->EnumPins(&enum_pins);
    PrintHr("EnumPins", hr);
    IPin* out_pin = nullptr;
    if (SUCCEEDED(hr) && enum_pins) {
        IPin* pin = nullptr;
        while (enum_pins->Next(1, &pin, nullptr) == S_OK) {
            PIN_DIRECTION dir = PINDIR_INPUT;
            pin->QueryDirection(&dir);
            if (dir == PINDIR_OUTPUT) {
                out_pin = pin;
                break;
            }
            pin->Release();
        }
        enum_pins->Release();
    }
    if (out_pin) {
        IEnumMediaTypes* enum_mt = nullptr;
        hr = out_pin->EnumMediaTypes(&enum_mt);
        PrintHr("EnumMediaTypes", hr);
        if (SUCCEEDED(hr) && enum_mt) {
            AM_MEDIA_TYPE* mt = nullptr;
            while (enum_mt->Next(1, &mt, nullptr) == S_OK) {
                if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
                    std::printf("    type: %ldx%ld RGB32, avg frame %lld00 ns\n",
                                long(vih->bmiHeader.biWidth), long(vih->bmiHeader.biHeight),
                                long long(vih->AvgTimePerFrame));
                }
                if (mt->pbFormat) CoTaskMemFree(mt->pbFormat);
                CoTaskMemFree(mt);
            }
            enum_mt->Release();
        }
    }

    // ---- Stage 4: run the graph against a null renderer ------------------
    std::printf("[stage 4] run graph %d s (wireplay.exe must be mirroring)\n", seconds);
    IGraphBuilder* graph = nullptr;
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&graph));
    PrintHr("CoCreateInstance(FilterGraph)", hr);
    long long delivered = 0;
    if (SUCCEEDED(hr) && graph) {
        Step("AddFilter(WirePlay)");
        graph->AddFilter(filter, L"WirePlay iPhone");
        IBaseFilter* grabber = nullptr;
        IBaseFilter* null_renderer = nullptr;
        hr = CoCreateInstance(MyCLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&grabber));
        PrintHr("CoCreateInstance(SampleGrabber)", hr);
        if (SUCCEEDED(hr)) {
            ISampleGrabber* sg = nullptr;
            grabber->QueryInterface(IID_PPV_ARGS(&sg));
            AM_MEDIA_TYPE mt {};
            mt.majortype = MEDIATYPE_Video;
            mt.subtype = MEDIASUBTYPE_RGB32;
            mt.formattype = FORMAT_VideoInfo;
            if (sg) sg->SetMediaType(&mt);
            auto* cb = new GrabberCB();
            if (sg) {
                sg->SetCallback(cb, 1);  // 1 = BufferCB (buffered copies)
                sg->SetOneShot(FALSE);
                // Buffer mode + BufferCB: with buffer samples disabled the
                // grabber passes samples through without ever invoking a
                // callback (the trap this probe sat in: the filter logged
                // 233 delivered, we counted 0).
                sg->SetBufferSamples(TRUE);
            }
            Step("SetMediaType/SetCallback done; AddFilter(Grabber)");
            graph->AddFilter(grabber, L"Grabber");
            // Connect the pins directly, the way OBS's capture code does
            // (RenderStream would demand a pin category we do not owe it).
            auto find_pin = [](IBaseFilter* f2, PIN_DIRECTION want) -> IPin* {
                IEnumPins* ep = nullptr;
                IPin* found = nullptr;
                if (FAILED(f2->EnumPins(&ep)) || !ep) return nullptr;
                IPin* pin2 = nullptr;
                while (ep->Next(1, &pin2, nullptr) == S_OK) {
                    PIN_DIRECTION d = PINDIR_INPUT;
                    pin2->QueryDirection(&d);
                    if (d == want) { found = pin2; break; }
                    pin2->Release();
                }
                ep->Release();
                return found;
            };
            // No NullRenderer: the grabber fires its callback from
            // Receive() whether or not its output is connected, and the
            // extra filter only added a place to hang.
            Step("finding pins (grabber in)");
            IPin* grab_in = find_pin(grabber, PINDIR_INPUT);
            Step("Connect(WirePlay -> SampleGrabber)...");
            hr = (out_pin && grab_in) ? graph->Connect(out_pin, grab_in) : E_POINTER;
            PrintHr("Connect(WirePlay -> SampleGrabber)", hr);
            if (SUCCEEDED(hr)) {
                IMediaControl* control = nullptr;
                graph->QueryInterface(IID_PPV_ARGS(&control));
                if (control) {
                    Step("IMediaControl::Run...");
                    hr = control->Run();
                    PrintHr("IMediaControl::Run", hr);
                    Step("graph running; waiting");
                    {
                        OAFilterState gst = 0;
                        const HRESULT ghr = control->GetState(500, &gst);
                        FILTER_STATE fst = State_Stopped;
                        const HRESULT fhr = filter->GetState(0, &fst);
                        std::printf("    graph state hr=0x%08lX state=%ld; "
                                    "filter hr=0x%08lX state=%d\n",
                                    ghr, long(gst), fhr, int(fst));
                        std::fflush(stdout);
                    }
                    Sleep(DWORD(seconds) * 1000);
                    delivered = cb->frames;
                    std::printf("    delivered %lld frames, %lld bytes total\n",
                                (long long)cb->frames, (long long)cb->bytes);
                    std::fflush(stdout);
                    control->Stop();
                    control->Release();
                }
            }
            if (grab_in) grab_in->Release();
            if (sg) sg->Release();
            cb->Release();
        }
        if (grabber) grabber->Release();
        if (null_renderer) null_renderer->Release();
        graph->Release();
    }

    if (out_pin) out_pin->Release();
    filter->Release();
    CoUninitialize();

    if (delivered > 0) {
        std::printf("RESULT: camera works end to end over DirectShow.\n");
        return 0;
    }
    std::printf("RESULT: chain reached the graph but no frames arrived "
                "(is wireplay.exe running and mirroring?).\n");
    return listed ? 3 : 2;
}
