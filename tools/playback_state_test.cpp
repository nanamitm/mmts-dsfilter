// Exercises production callbacks and queued DirectShow samples without a decoder.
#include <streams.h>
#include <dvdmedia.h>
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
struct Captured {
    REFERENCE_TIME pts; bool preroll; bool discontinuity;
    REFERENCE_TIME stop; REFERENCE_TIME framePeriod; DWORD aspectX; DWORD aspectY;
    WORD channels; // from an attached WAVEFORMATEX, 0 when the sample carried none
};
class CapturePin : public CBaseInputPin {
public:
    CapturePin(CBaseFilter* filter, CCritSec* lock, HRESULT* hr)
        : CBaseInputPin(NAME("Capture input"), filter, lock, hr, L"Input") {}
    HRESULT CheckMediaType(const CMediaType*) override { return S_OK; }
    std::atomic<unsigned> begins{0};
    std::atomic<bool> failReceive{false}; // Stand in for a connected pin nothing plays.
    STDMETHODIMP BeginFlush() override { ++begins; return CBaseInputPin::BeginFlush(); }
    STDMETHODIMP Receive(IMediaSample* sample) override {
        if (failReceive) return E_FAIL;
        REFERENCE_TIME start, stop;
        const HRESULT hr = sample->GetTime(&start, &stop);
        if (FAILED(hr)) return hr;
        std::lock_guard<std::mutex> guard(mutex);
        REFERENCE_TIME period=0; DWORD aspectX=0, aspectY=0; WORD channels=0;
        AM_MEDIA_TYPE* updated=nullptr;
        if (sample->GetMediaType(&updated)==S_OK && updated) {
            if (updated->formattype==FORMAT_VideoInfo2 && updated->cbFormat>=sizeof(VIDEOINFOHEADER2)) {
                const auto* video=reinterpret_cast<const VIDEOINFOHEADER2*>(updated->pbFormat);
                period=video->AvgTimePerFrame; aspectX=video->dwPictAspectRatioX; aspectY=video->dwPictAspectRatioY;
            } else if (updated->formattype==FORMAT_WaveFormatEx && updated->cbFormat>=sizeof(WAVEFORMATEX)) {
                channels=reinterpret_cast<const WAVEFORMATEX*>(updated->pbFormat)->nChannels;
            }
            DeleteMediaType(updated);
        }
        samples.push_back({start, sample->IsPreroll() == S_OK, sample->IsDiscontinuity() == S_OK,
                           stop, period, aspectX, aspectY, channels});
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
// A superimpose is an overlay over the running captions, not an alternative to
// them, but a text subtitle track is exclusive downstream. These lock in that
// its cues reach the caption pin rather than a track of their own.
static void testSuperimposeRouting() {
    require(IsCaptionComponentTag(0x30) && IsCaptionComponentTag(0x37),
            "0x30-0x37 is the caption range");
    require(!IsCaptionComponentTag(0x2F) && !IsCaptionComponentTag(0x38),
            "the caption range stops at 0x37");
    require(IsSuperimposeComponentTag(0x38) && IsSuperimposeComponentTag(0x3F),
            "0x38-0x3F is the superimpose range");
    require(!IsSuperimposeComponentTag(0x37) && !IsSuperimposeComponentTag(0x40),
            "the superimpose range is 0x38-0x3F only");

    // Merged: the superimpose cue lands on the caption pin and nowhere else.
    require(SubtitleCueMatchesPin(0x38, 3, 0x30, 2, true),
            "a merged superimpose cue goes to the caption pin");
    require(!SubtitleCueMatchesPin(0x38, 3, 0x38, 3, true),
            "a merged superimpose cue does not also go to a superimpose pin");

    // Not merged: the tag identifies the track, both ways.
    require(SubtitleCueMatchesPin(0x38, 3, 0x38, 3, false),
            "an unmerged superimpose cue keeps its own pin");
    require(!SubtitleCueMatchesPin(0x38, 3, 0x30, 2, false),
            "an unmerged superimpose cue stays off the caption pin");
    require(SubtitleCueMatchesPin(0x30, 2, 0x30, 2, false),
            "a caption cue goes to the caption pin");
    require(!SubtitleCueMatchesPin(0x30, 2, 0x38, 3, false),
            "a caption cue never goes to the superimpose pin");

    // Merging never diverts a caption cue, whatever the setting.
    require(SubtitleCueMatchesPin(0x30, 2, 0x30, 2, false) &&
            !SubtitleCueMatchesPin(0x30, 2, 0x38, 3, false),
            "merging leaves caption routing alone");

    // No tag on either side: fall back to the stream index.
    require(SubtitleCueMatchesPin(-1, 5, -1, 5, false),
            "an untagged cue falls back to the stream index");
    require(!SubtitleCueMatchesPin(-1, 5, -1, 6, false),
            "the index fallback still distinguishes streams");
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "Pass the fixture output directory\n"; return 2; }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        require(SUCCEEDED(com), "COM startup");
        constexpr REFERENCE_TIME sec = 10000000;
        {
            // A 4:3/50fps service must carry its attributes through connection
            // negotiation and real queued IMediaSample delivery.
            HRESULT hr=S_OK; CMmtTlvSplitter metadata(nullptr,&hr);
            CFilterDemuxerHandler::VideoStreamInfo info{0,0x100,0,1440,1080,true,50,1,4,3};
            metadata.m_videoWidth=1440; metadata.m_videoHeight=1080;
            metadata.m_handler.setKnownVideoStreams({info}); metadata.CreatePins(); CMediaType mt;
            require(SUCCEEDED(metadata.m_pins[0]->GetMediaType(0,&mt)),"get descriptor-based video format");
            const auto* format=reinterpret_cast<const VIDEOINFOHEADER2*>(mt.Format());
            require(format->AvgTimePerFrame==200000 && format->dwPictAspectRatioX==4 && format->dwPictAspectRatioY==3,
                    "initial connection must advertise 50fps and 4:3");
            Fixture fixture; fixture.filter->m_firstPts=0; fixture.filter->m_videoWidth=1440; fixture.filter->m_videoHeight=1080;
            fixture.filter->m_handler.setKnownVideoStreams({info}); fixture.feed(0,0,true); fixture.feed(.02,.02,false); fixture.count(2);
            auto delivered=fixture.sink->pin->snapshot();
            require(delivered[1].pts-delivered[0].pts==200000 && delivered[0].stop-delivered[0].pts==200000 &&
                    delivered[1].stop-delivered[1].pts==200000,"50fps sample duration must agree with PTS spacing");
            require(delivered[0].framePeriod==200000 && delivered[0].aspectX==4 && delivered[0].aspectY==3,
                    "connected pin must notify changed attributes on the sample");
            info.frameRateNumerator=60000; info.frameRateDenominator=1001; info.aspectX=16; info.aspectY=9;
            fixture.filter->m_handler.setKnownVideoStreams({info}); fixture.feed(.04,.04,false); fixture.count(3);
            delivered=fixture.sink->pin->snapshot();
            require(delivered[2].stop-delivered[2].pts==166833 && delivered[2].framePeriod==166833 &&
                    delivered[2].aspectX==16 && delivered[2].aspectY==9,"59.94fps/16:9 update must reach downstream");
            info.frameRateNumerator=24000; fixture.filter->m_handler.setKnownVideoStreams({info});
            fixture.feed(.08,.08,false); fixture.count(4); delivered=fixture.sink->pin->snapshot();
            require(delivered[3].stop-delivered[3].pts==417083,"23.976fps must retain the /1001 denominator");
        }
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
        {
            // A connected audio pin whose downstream rejects every sample must
            // stop producing, so the 22.2ch LATM decode never runs for it.
            Fixture fixture;
            IMemAllocator* audioAllocator = nullptr;
            ALLOCATOR_PROPERTIES audioProps{};
            require(SUCCEEDED(fixture.audioSink->pin->GetAllocator(&audioAllocator)),
                    "get the negotiated audio allocator");
            const HRESULT propsHr = audioAllocator->GetProperties(&audioProps);
            audioAllocator->Release();
            require(SUCCEEDED(propsHr), "read the audio allocator properties");
            // 64 AAC frames, about 1.4s. Deliberately not deeper: a downstream
            // decoder that holds its filter lock across Deliver() holds it for
            // as long as this lead, and a deeper queue bought nothing when it
            // was measured.
            require(audioProps.cBuffers == 64, "audio pin keeps its 1.4s of lead");
            // A channel boundary can change an asset's format under a connected
            // pin. The new media type must reach the next sample.
            const uint8_t adts[] = {0xFF, 0xF1, 0x4C, 0x80, 0x00, 0x1F, 0xFC};
            fixture.audio->DeliverSample(true, 0, 0, true, true, adts, sizeof(adts));
            require(waitFor([&] { return fixture.audioSink->pin->snapshot().size() == 1; }),
                    "first audio sample delivered");
            require(fixture.audioSink->pin->snapshot()[0].channels == 0,
                    "an unchanged format attaches no media type");
            fixture.audio->SetAudioInfo(48000, 6);
            fixture.audio->DeliverSample(true, 213333, 213333, true, true, adts, sizeof(adts));
            require(waitFor([&] { return fixture.audioSink->pin->snapshot().size() == 2; }),
                    "second audio sample delivered");
            require(fixture.audioSink->pin->snapshot()[1].channels == 6,
                    "the changed channel count reaches the decoder");
            fixture.audio->SetAudioInfo(48000, 6);
            fixture.audio->DeliverSample(true, 426666, 426666, true, true, adts, sizeof(adts));
            require(waitFor([&] { return fixture.audioSink->pin->snapshot().size() == 3; }),
                    "third audio sample delivered");
            require(fixture.audioSink->pin->snapshot()[2].channels == 0,
                    "setting the same format again attaches nothing");

            fixture.audioSink->pin->failReceive = true;
            const uint8_t frame[] = {0xFF, 0xF1, 0x4C, 0x80, 0x00, 0x1F, 0xFC};
            // The queue only reports the failure once its worker has tried the
            // sample, so give it a slot between deliveries to catch up.
            bool suppressed = false;
            for (int i = 0; i < 400 && !suppressed; ++i) {
                fixture.audio->DeliverSample(true, i * 213333LL, i * 213333LL, true, true,
                                             frame, sizeof(frame));
                suppressed = !fixture.audio->AcceptsSamples();
                if (!suppressed) Sleep(1);
            }
            require(suppressed, "audio pin stops producing once its downstream keeps failing");
            require(fixture.audio->DeliverSample(true, 0, 0, true, true, frame, sizeof(frame)) == S_FALSE,
                    "suppressed audio pin takes no further samples");
            require(fixture.video->AcceptsSamples(), "video pin is never suppressed");
            fixture.audioSink->pin->failReceive = false;
            // EndFlush waits for the queue's flush, so it needs its BeginFlush.
            require(SUCCEEDED(fixture.audio->DeliverBeginFlush()), "begin flush the audio branch");
            require(SUCCEEDED(fixture.audio->DeliverEndFlush()), "flush the audio branch");
            require(fixture.audio->AcceptsSamples(), "a flush lets the audio pin try again");
        }
        testSuperimposeRouting();
        CoUninitialize(); std::cout << "Playback state tests passed: " << checks << " checks\n"; return 0;
    } catch (const std::exception& e) {
        if (SUCCEEDED(com)) CoUninitialize(); std::cerr << e.what() << '\n'; return 1;
    }
}
