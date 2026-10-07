// mmts_pin_probe.cpp
//
// Plays an MMTS file through the real mmts-dsfilter in a DirectShow graph and
// reports how many samples each output pin delivered, with the first and last
// timestamps. This exercises the filter as a player does - PreScan, pin
// creation, the locked stream lists, seeking - which a demuxer-only probe
// cannot: the handler behaves differently once pins exist, so routing bugs
// (a caption arriving on the wrong subtitle pin, an audio pin going silent at
// an MPT change) only show up here.
//
// The video pin goes to a Null Renderer. Every other pin goes to a Sample
// Grabber whose output is left unconnected: a renderer on a pin that has no
// data yet (a commentary track that starts mid-recording) would hold the graph
// in its pause transition, and nothing would play.
//
// The graph runs on the default clock, so the run time is real time.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dshow.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// From src/Guids.h and qedit.h (the latter is no longer in the Windows SDK;
// the Sample Grabber and Null Renderer themselves still ship with Windows).
const GUID kMediaTypeSubtitle = {0xE487EB08, 0x6B26, 0x4BE9, {0x9D, 0xD3, 0x99, 0x34, 0x34, 0xD3, 0x13, 0xFD}};
const CLSID kClsidMmtsSplitter = {0x8B7D1A60, 0x3C4E, 0x4F2A, {0x9B, 0x8E, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB}};
const CLSID kClsidSampleGrabber = {0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
const CLSID kClsidNullRenderer = {0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
const IID kIidSampleGrabber = {0x6B652FFF, 0x11FE, 0x4fce, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
const IID kIidSampleGrabberCB = {0x0579154A, 0x2B53, 0x4994, {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};

struct ISampleGrabberCB : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long size) = 0;
};

struct ISampleGrabber : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* size, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback, long whichMethod) = 0;
};

// Lives for the whole run; reference counting is not needed.
class PinCounter : public ISampleGrabberCB {
public:
    std::string name;
    bool subtitle = false;
    std::atomic<long> samples{0};
    std::atomic<LONGLONG> firstMs{-1};
    std::atomic<LONGLONG> lastMs{-1};

    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == kIidSampleGrabberCB) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample* sample) override
    {
        REFERENCE_TIME start = -1, stop = -1;
        sample->GetTime(&start, &stop);
        if (++samples == 1)
            firstMs = start / 10000;
        lastMs = start / 10000;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) override { return S_OK; }
};

IPin* FindPin(IBaseFilter* filter, PIN_DIRECTION direction)
{
    IEnumPins* pins = nullptr;
    if (FAILED(filter->EnumPins(&pins)))
        return nullptr;
    IPin* pin = nullptr;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION pinDirection;
        if (SUCCEEDED(pin->QueryDirection(&pinDirection)) && pinDirection == direction) {
            pins->Release();
            return pin;
        }
        pin->Release();
    }
    pins->Release();
    return nullptr;
}

HRESULT CreateSplitter(const wchar_t* axPath, IBaseFilter** splitter)
{
    if (!axPath) {
        return CoCreateInstance(kClsidMmtsSplitter, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IBaseFilter, reinterpret_cast<void**>(splitter));
    }

    // Load an unregistered build directly, so a fresh build can be checked
    // without touching the installed filter. It reads the .ini next to it.
    HMODULE module = LoadLibraryW(axPath);
    if (!module)
        return HRESULT_FROM_WIN32(GetLastError());
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
    if (!getClassObject)
        return E_NOINTERFACE;
    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(kClsidMmtsSplitter, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr))
        return hr;
    hr = factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(splitter));
    factory->Release();
    return hr;
}

std::string ToUtf8(const wchar_t* text)
{
    char buffer[512];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer), nullptr, nullptr);
    return buffer;
}

void PrintUsage()
{
    std::fwprintf(stderr,
        L"usage: mmts_pin_probe <input.mmts> [--seek SEC] [--run SEC] [--ax PATH]\n"
        L"                      [--expect-main-subtitle] [--expect-pin NAME]...\n"
        L"  --seek SEC              start playback at SEC seconds (default 0)\n"
        L"  --run SEC               play for SEC seconds of real time (default 60)\n"
        L"  --ax PATH               load this mmts-dsfilter.ax instead of the registered one\n"
        L"  --expect-main-subtitle  fail unless the first subtitle pin (the main\n"
        L"                          caption) received samples and no other subtitle\n"
        L"                          pin did\n"
        L"  --expect-pin NAME       fail unless a pin whose name starts with NAME\n"
        L"                          received samples (repeatable)\n"
        L"exit code: 0 = ran (and every expectation held), 1 = an expectation\n"
        L"failed or the graph could not be built, 2 = bad arguments\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* input = nullptr;
    const wchar_t* axPath = nullptr;
    double seekSec = 0;
    double runSec = 60;
    bool expectMainSubtitle = false;
    std::vector<std::string> expectedPins;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--expect-main-subtitle") == 0) {
            expectMainSubtitle = true;
        } else if (std::wcscmp(argv[i], L"--expect-pin") == 0 && i + 1 < argc) {
            expectedPins.push_back(ToUtf8(argv[++i]));
        } else if (std::wcscmp(argv[i], L"--seek") == 0 && i + 1 < argc) {
            seekSec = _wtof(argv[++i]);
        } else if (std::wcscmp(argv[i], L"--run") == 0 && i + 1 < argc) {
            runSec = _wtof(argv[++i]);
        } else if (std::wcscmp(argv[i], L"--ax") == 0 && i + 1 < argc) {
            axPath = argv[++i];
        } else if (!input && argv[i][0] != L'-') {
            input = argv[i];
        } else {
            PrintUsage();
            return 2;
        }
    }
    if (!input || runSec <= 0) {
        PrintUsage();
        return 2;
    }

    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IBaseFilter* splitter = nullptr;
    HRESULT hr = CreateSplitter(axPath, &splitter);
    if (FAILED(hr)) {
        std::printf("cannot create mmts-dsfilter: 0x%08lX\n", hr);
        return 1;
    }

    IGraphBuilder* graph = nullptr;
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, reinterpret_cast<void**>(&graph));
    if (FAILED(hr)) {
        std::printf("cannot create filter graph: 0x%08lX\n", hr);
        return 1;
    }
    graph->AddFilter(splitter, L"mmts-dsfilter");

    IFileSourceFilter* source = nullptr;
    splitter->QueryInterface(IID_IFileSourceFilter, reinterpret_cast<void**>(&source));
    if (!source || FAILED(hr = source->Load(input, nullptr))) {
        std::printf("Load failed: 0x%08lX\n", hr);
        return 1;
    }

    std::vector<PinCounter*> counters;
    IEnumPins* pins = nullptr;
    splitter->EnumPins(&pins);
    IPin* pin = nullptr;
    int pinIndex = 0;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_INFO info{};
        pin->QueryPinInfo(&info);
        if (info.pFilter)
            info.pFilter->Release();

        bool isVideo = false;
        bool isSubtitle = false;
        IEnumMediaTypes* types = nullptr;
        AM_MEDIA_TYPE* type = nullptr;
        if (SUCCEEDED(pin->EnumMediaTypes(&types)) && types->Next(1, &type, nullptr) == S_OK) {
            isVideo = type->majortype == MEDIATYPE_Video;
            isSubtitle = type->majortype == kMediaTypeSubtitle;
            CoTaskMemFree(type->pbFormat);
            CoTaskMemFree(type);
        }
        if (types)
            types->Release();

        const std::string name = ToUtf8(info.achName);
        wchar_t filterName[32];
        std::swprintf(filterName, 32, L"sink%d", pinIndex);
        if (isVideo) {
            IBaseFilter* renderer = nullptr;
            hr = CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IBaseFilter, reinterpret_cast<void**>(&renderer));
            if (SUCCEEDED(hr)) {
                graph->AddFilter(renderer, filterName);
                hr = graph->ConnectDirect(pin, FindPin(renderer, PINDIR_INPUT), nullptr);
            }
        } else {
            IBaseFilter* grabber = nullptr;
            ISampleGrabber* grabberControl = nullptr;
            hr = CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IBaseFilter, reinterpret_cast<void**>(&grabber));
            if (SUCCEEDED(hr))
                hr = grabber->QueryInterface(kIidSampleGrabber, reinterpret_cast<void**>(&grabberControl));
            if (SUCCEEDED(hr)) {
                auto* counter = new PinCounter;
                counter->name = name;
                counter->subtitle = isSubtitle;
                counters.push_back(counter);
                grabberControl->SetCallback(counter, 0);
                graph->AddFilter(grabber, filterName);
                hr = graph->ConnectDirect(pin, FindPin(grabber, PINDIR_INPUT), nullptr);
            }
        }
        std::printf("pin %d: %s%s%s\n", pinIndex, name.c_str(),
                    isSubtitle ? " (subtitle)" : "",
                    SUCCEEDED(hr) ? "" : " (not connected)");
        pin->Release();
        ++pinIndex;
    }
    pins->Release();

    if (seekSec > 0) {
        IMediaSeeking* seeking = nullptr;
        graph->QueryInterface(IID_IMediaSeeking, reinterpret_cast<void**>(&seeking));
        LONGLONG position = static_cast<LONGLONG>(seekSec * 1e7);
        hr = seeking ? seeking->SetPositions(&position, AM_SEEKING_AbsolutePositioning,
                                             nullptr, AM_SEEKING_NoPositioning)
                     : E_NOINTERFACE;
        if (FAILED(hr)) {
            std::printf("seek to %.1f s failed: 0x%08lX\n", seekSec, hr);
            return 1;
        }
    }

    IMediaControl* control = nullptr;
    graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&control));
    std::printf("playing from %.1f s for %.1f s...\n", seekSec, runSec);
    control->Run();
    Sleep(static_cast<DWORD>(runSec * 1000));
    control->Stop();

    std::printf("\n%-40s %10s %12s %12s\n", "pin", "samples", "first ms", "last ms");
    for (const auto* counter : counters) {
        std::printf("%-40s %10ld %12lld %12lld\n", counter->name.c_str(), counter->samples.load(),
                    counter->firstMs.load(), counter->lastMs.load());
    }

    // The splitter creates the main caption pin first among its subtitle pins.
    std::vector<std::string> failures;
    if (expectMainSubtitle) {
        bool first = true;
        bool any = false;
        for (const auto* counter : counters) {
            if (!counter->subtitle)
                continue;
            any = true;
            const long samples = counter->samples.load();
            if (first && samples == 0)
                failures.push_back("main subtitle pin '" + counter->name + "' received no samples");
            if (!first && samples > 0)
                failures.push_back("subtitle pin '" + counter->name + "' received " +
                                   std::to_string(samples) + " samples (expected only the main one)");
            first = false;
        }
        if (!any)
            failures.push_back("no subtitle pin");
    }
    for (const auto& expected : expectedPins) {
        const PinCounter* match = nullptr;
        for (const auto* counter : counters) {
            if (counter->name.compare(0, expected.size(), expected) == 0) {
                match = counter;
                break;
            }
        }
        if (!match)
            failures.push_back("no counted pin named '" + expected + "*' (the video pin is not counted)");
        else if (match->samples.load() == 0)
            failures.push_back("pin '" + match->name + "' received no samples");
    }

    if (expectMainSubtitle || !expectedPins.empty()) {
        std::printf("\n");
        for (const auto& failure : failures)
            std::printf("FAIL: %s\n", failure.c_str());
        std::printf("RESULT: %s\n", failures.empty() ? "PASS" : "FAIL");
        if (!failures.empty())
            return 1;
    }
    return 0;
}
