// Exercises production callbacks and queued DirectShow samples without a decoder.
#include <streams.h>
#include "DsHandler.h"
#include "MmtTlvOutputPin.h"
#include "SubtitleFileIndex.h"
#define private public
#include "MmtTlvSplitter.h"
#undef private
#include "../src/MmtTlvSplitter.cpp"
#include <iostream>
#include <mutex>
#include <thread>

static unsigned checks = 0;
static void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static bool waitFor(const std::function<bool()>& condition) {
    const ULONGLONG end = GetTickCount64() + 3000;
    do { if (condition()) return true; Sleep(1); } while (GetTickCount64() < end);
    return false;
}
struct Captured { REFERENCE_TIME pts; bool preroll; bool discontinuity; };
class CapturePin : public CBaseInputPin {
public:
    CapturePin(CBaseFilter* filter, CCritSec* lock, HRESULT* hr)
        : CBaseInputPin(NAME("Capture input"), filter, lock, hr, L"Input") {}
    HRESULT CheckMediaType(const CMediaType*) override { return S_OK; }
    std::atomic<unsigned> begins{0};
    STDMETHODIMP BeginFlush() override { ++begins; return CBaseInputPin::BeginFlush(); }
    STDMETHODIMP Receive(IMediaSample* sample) override {
        REFERENCE_TIME start, stop;
        const HRESULT hr = sample->GetTime(&start, &stop);
        if (FAILED(hr)) return hr;
        std::lock_guard<std::mutex> guard(mutex);
        samples.push_back({start, sample->IsPreroll() == S_OK, sample->IsDiscontinuity() == S_OK});
        return S_OK;
    }
    std::vector<Captured> snapshot() {
        std::lock_guard<std::mutex> guard(mutex); return samples;
    }
private:
    std::mutex mutex;
    std::vector<Captured> samples;
};
class CaptureFilter : public CBaseFilter {
public:
    DECLARE_IUNKNOWN;
    explicit CaptureFilter(HRESULT* hr) : CBaseFilter(L"Capture", nullptr, &lock, GUID_NULL) {
        pin = new CapturePin(this, &lock, hr);
    }
    ~CaptureFilter() { delete pin; }
    int GetPinCount() override { return 1; }
    CBasePin* GetPin(int n) override { return n == 0 ? pin : nullptr; }
    CapturePin* pin;
private:
    CCritSec lock;
};
struct Fixture {
    CMmtTlvSplitter* filter;
    CaptureFilter* sink;
    CaptureFilter* audioSink;
    CMmtTlvOutputPin* audio;
    CMmtTlvOutputPin* video;
    Fixture() {
        HRESULT hr = S_OK;
        filter = new CMmtTlvSplitter(nullptr, &hr); filter->NonDelegatingAddRef();
        sink = new CaptureFilter(&hr); sink->NonDelegatingAddRef();
        require(SUCCEEDED(hr), "construct filters");
        filter->m_handler.setKnownVideoStreams({{0, 0x100, 0, 3840, 2160, true},
                                               {2, 0x102, 1, 1920, 1080, true}});
        filter->m_handler.setKnownAudioStreams({{1, 0x101, 0x10, 48000, 2, false, {}},
                                               {3, 0x103, 0x11, 48000, 2, false, {}}});
        filter->CreatePins(); video = filter->m_pins.front();
        audio = filter->m_pins[1];
        audioSink = new CaptureFilter(&hr); audioSink->NonDelegatingAddRef();
        require(SUCCEEDED(audio->Connect(audioSink->pin, nullptr)), "connect audio branch");
        require(SUCCEEDED(audio->Active()), "activate queued audio");
        require(SUCCEEDED(video->Connect(sink->pin, nullptr)), "connect real output pin");
        IMemAllocator* allocator = nullptr;
        require(SUCCEEDED(sink->pin->GetAllocator(&allocator)), "get negotiated allocator");
        ALLOCATOR_PROPERTIES request{8, 4096, 1, 0}, actual{};
        hr = allocator->SetProperties(&request, &actual); allocator->Release();
        require(SUCCEEDED(hr), "use small fixture buffers");
        require(SUCCEEDED(video->Active()), "activate queued output");
    }
    ~Fixture() {
        filter->Stop(); video->Disconnect(); sink->pin->Disconnect();
        audio->Disconnect(); audioSink->pin->Disconnect();
        sink->NonDelegatingRelease(); audioSink->NonDelegatingRelease(); filter->NonDelegatingRelease();
    }
    void feed(double pts, double dts, bool rap) {
        MmtTlv::MmtStream stream(0x100);
        MmtTlv::MfuData mfu{};
        mfu.pts = static_cast<uint64_t>(pts * 90000);
        mfu.dts = static_cast<uint64_t>(dts * 90000);
        mfu.keyframe = false; // Exercise the pin's IRAP NAL detection too.
        mfu.isFirstFragment = mfu.isLastFragment = true;
        mfu.data = {0, 0, 1, static_cast<uint8_t>(rap ? 0x26 : 0x02), 1, 0x80};
        filter->m_handler.onVideoData(stream, mfu);
    }
    void count(size_t expected) {
        require(waitFor([&] { return sink->pin->snapshot().size() == expected; }), "queued sample delivery");
    }
};
static void makeMap(const std::wstring& media) {
    std::ofstream file(media, std::ios::binary);
    const std::string bytes(100000, '\0'); file.write(bytes.data(), bytes.size()); file.close();
    std::ofstream map(SidecarMapPathFor(media), std::ios::binary);
    auto pod = [&](auto value) { map.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
    map.write("MMTSMAP3", 8); pod(uint32_t{3}); pod(uint32_t{0}); pod(uint64_t{100000});
    pod(int64_t{90000}); pod(int64_t{1000000}); pod(int64_t{1090000});
    pod(uint32_t{0}); pod(uint32_t{0}); pod(uint32_t{3}); pod(uint32_t{0});
    for (int i = 0; i < 3; ++i) { pod(int64_t{1000000} + i * 30000); pod(uint64_t{1000} + i * 30000); }
}
struct WorkerState { HANDLE stop; std::atomic<bool> exited{false}; };
static DWORD WINAPI waitingWorker(LPVOID context) {
    auto& state = *static_cast<WorkerState*>(context);
    WaitForSingleObject(state.stop, INFINITE); state.exited = true; return 0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "Pass the fixture output directory\n"; return 2; }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        require(SUCCEEDED(com), "COM startup");
        constexpr REFERENCE_TIME sec = 10000000;
        {
            HRESULT hr = S_OK; CMmtTlvSplitter filter(nullptr, &hr);
            filter.m_filename = std::wstring(argv[1]) + L"\\playback_fixture.mmts";
            makeMap(filter.m_filename); filter.m_fileSize = 100000;
            filter.LoadSidecarMap(); filter.m_origFirstPts = filter.m_firstPts = 1000 * sec;
            filter.m_editSegments = {{10 * sec, 40 * sec}, {50 * sec, 80 * sec}};
            filter.m_duration = 60 * sec; filter.m_sourceDuration = 90 * sec;
            filter.m_seekTarget = 20 * sec; filter.m_isSeeking = true;
            filter.DemuxLoop();
            require(filter.m_demuxByteOffset == 31000, "EDL startup seeks the normalized 30s RAP");
            filter.m_seekTarget = 35 * sec; filter.DemuxLoop();
            require(filter.m_demuxByteOffset == 31000 && filter.m_curSegment == 1,
                    "seek inside second segment queries source-relative 55s");
            filter.m_editSegments.clear(); filter.m_virtualStart = 10 * sec;
            filter.m_seekTarget = 20 * sec; filter.DemuxLoop();
            require(filter.m_demuxByteOffset == 31000, "single trim retains source-relative lookup");
        }
        {
            Fixture fixture; auto& filter = *fixture.filter;
            filter.m_origFirstPts = filter.m_firstPts = 1000 * sec;
            filter.m_editSegments = {{10 * sec, 40 * sec}, {50 * sec, 80 * sec}};
            filter.m_curSegment = 0; filter.m_curSegmentBase = 0;
            filter.m_waitingForVideoRap = true; fixture.video->SetWaitForVideoRap(true);
            fixture.feed(1009, 1008, true); fixture.feed(1010, 1009, false); fixture.count(2);
            auto captured = fixture.sink->pin->snapshot();
            require(captured[0].preroll && !captured[1].preroll && captured[1].pts == 0,
                    "pre-cut RAP decoded, exact cut frame displayed");
            require(filter.m_currentPts == 0, "pre-cut RAP cannot advance playback position");
            REFERENCE_TIME mapped;
            require(!filter.EditMapPts(1040 * sec, mapped) && filter.m_pendingSegmentJump == -1,
                    "leading audio cannot request a jump");
            fixture.feed(1040.1, 1039.8, false); fixture.feed(1039.9, 1039.9, false); fixture.count(4);
            captured = fixture.sink->pin->snapshot();
            require(captured[2].preroll && !captured[3].preroll,
                    "post-cut reference decoded before in-range B frame");
            require(filter.m_pendingSegmentJump == -1, "reference PTS cannot request a jump");
            fixture.feed(1040.2, 1040, false);
            require(filter.m_pendingSegmentJump == 1, "video DTS requests the next segment at the cut");
            filter.m_pendingSegmentJump = -1; filter.m_curSegment = 1; filter.m_curSegmentBase = 30 * sec;
            filter.m_waitingForVideoRap = true; fixture.video->ResetForSeek();
            fixture.feed(1049, 1048, true); fixture.feed(1050, 1049, false); fixture.count(6);
            captured = fixture.sink->pin->snapshot();
            require(captured[4].preroll && captured[4].discontinuity, "next segment starts with decoder preroll/discontinuity");
            require(!captured[5].preroll && captured[5].pts == 30 * sec && filter.GetSegmentTimeOffset() == 0,
                    "second segment preserves cumulative 30s output time");
            filter.m_segmentStart = 35 * sec;
            bool preroll;
            require(filter.EditMapPts(1054 * sec, mapped, true, &preroll, 1053 * sec) && preroll,
                    "RAP before seek target within a segment is preroll");
            require(!filter.EditMapPts(1054 * sec, mapped), "audio before seek target is not displayed at zero");
            require(filter.EditMapPts(1055 * sec, mapped) && mapped == 35 * sec,
                    "audio at requested seek target retains its program time");
            require(filter.EditMapPts(1079 * sec, mapped, true, &preroll, 1078 * sec) && !preroll,
                    "video before final decode boundary remains playable");
            require(!filter.EditMapPts(1080 * sec, mapped, true, &preroll, 1080 * sec) && filter.m_pendingSegmentJump == -2,
                    "final video boundary requests EOS");
            filter.m_editSegments.clear(); filter.m_waitingForVideoRap = true;
            filter.NotifyVideoRap(sec);
            require(filter.GetSegmentTimeOffset() == sec, "ordinary playback RAP recovery unchanged");
        }
        {
            Fixture fixture; auto& filter = *fixture.filter;
            WorkerState worker{filter.m_hStop};
            filter.m_active = true;
            filter.m_hThread = CreateThread(nullptr, 0, waitingWorker, &worker, 0, nullptr);
            require(filter.m_hThread != nullptr, "start track-change worker fixture");
            require(filter.Enable(1, AMSTREAMSELECTENABLE_ENABLE) == S_OK, "change selected video");
            require(worker.exited && filter.m_hThread == nullptr && filter.m_handler.getSelectedVideoStreamIndex() == 2,
                    "track selection joins producer before resetting pins");
            require(fixture.audioSink->pin->begins != 0, "video selection flushes connected audio too");
            const unsigned flushedVideo = fixture.sink->pin->begins;
            worker.exited = false; filter.m_active = true;
            filter.m_hThread = CreateThread(nullptr, 0, waitingWorker, &worker, 0, nullptr);
            require(filter.Enable(3, AMSTREAMSELECTENABLE_ENABLE) == S_OK, "change selected audio");
            require(worker.exited && filter.m_hThread == nullptr && filter.m_handler.getSelectedAudioStreamIndex() == 3,
                    "audio selection joins producer before resetting pins");
            require(fixture.sink->pin->begins > flushedVideo, "audio selection flushes connected video too");
            require(filter.Enable(99, AMSTREAMSELECTENABLE_ENABLE) == E_INVALIDARG, "invalid selection rejected");
        }
        CoUninitialize(); std::cout << "Playback state tests passed: " << checks << " checks\n"; return 0;
    } catch (const std::exception& e) {
        if (SUCCEEDED(com)) CoUninitialize(); std::cerr << e.what() << '\n'; return 1;
    }
}
