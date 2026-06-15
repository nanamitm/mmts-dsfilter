#pragma once
#include <streams.h>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include "mmtTlvDemuxer.h"
#include "DsHandler.h"
#include "MmtTlvOutputPin.h"

class CMmtTlvSplitter : public CBaseFilter
                      , public IFileSourceFilter
                      , public IAMFilterMiscFlags
                      , public IMediaSeeking
                      , public IAMStreamSelect {
public:
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT* phr);

    CMmtTlvSplitter(LPUNKNOWN pUnk, HRESULT* phr);
    ~CMmtTlvSplitter();

    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;

    // IFileSourceFilter
    STDMETHODIMP Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetCurFile(LPOLESTR* ppszFileName, AM_MEDIA_TYPE* pmt) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

    // CBaseFilter
    int GetPinCount() override;
    CBasePin* GetPin(int n) override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Stop() override;

    // IMediaSeeking
    STDMETHODIMP GetCapabilities(DWORD* pCaps) override;
    STDMETHODIMP CheckCapabilities(DWORD* pCaps) override;
    STDMETHODIMP IsFormatSupported(const GUID* pFormat) override;
    STDMETHODIMP QueryPreferredFormat(GUID* pFormat) override;
    STDMETHODIMP GetTimeFormat(GUID* pFormat) override;
    STDMETHODIMP IsUsingTimeFormat(const GUID* pFormat) override;
    STDMETHODIMP SetTimeFormat(const GUID* pFormat) override;
    STDMETHODIMP GetDuration(LONGLONG* pDuration) override;
    STDMETHODIMP GetStopPosition(LONGLONG* pStop) override;
    STDMETHODIMP GetCurrentPosition(LONGLONG* pCurrent) override;
    STDMETHODIMP ConvertTimeFormat(LONGLONG* pTarget, const GUID* pTargetFormat,
                                   LONGLONG Source, const GUID* pSourceFormat) override;
    STDMETHODIMP SetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags,
                              LONGLONG* pStop, DWORD dwStopFlags) override;
    STDMETHODIMP GetPositions(LONGLONG* pCurrent, LONGLONG* pStop) override;
    STDMETHODIMP GetAvailable(LONGLONG* pEarliest, LONGLONG* pLatest) override;
    STDMETHODIMP SetRate(double dRate) override;
    STDMETHODIMP GetRate(double* pdRate) override;
    STDMETHODIMP GetPreroll(LONGLONG* pllPreroll) override;

    // IAMStreamSelect
    STDMETHODIMP Count(DWORD* pcStreams) override;
    STDMETHODIMP Info(long lIndex, AM_MEDIA_TYPE** ppmt, DWORD* pdwFlags,
                      LCID* plcid, DWORD* pdwGroup, LPWSTR* ppszName,
                      IUnknown** ppObject, IUnknown** ppUnk) override;
    STDMETHODIMP Enable(long lIndex, DWORD dwFlags) override;

    bool IsWaitingForVideoRap() const;
    void NotifyVideoRap(REFERENCE_TIME segmentTime);
    REFERENCE_TIME GetSegmentTimeOffset() const;
    REFERENCE_TIME GetSegmentStart() const { return m_segmentStart; }

private:
    struct LatmPcmDecoder;

    void CreatePins();
    void StartThread();
    void StopThread();
    void SeekTo(REFERENCE_TIME pos);
    void PreScanFile();
    void LoadSidecarEdit();
    void LoadSidecarMap();
    void ApplySidecarIndex();
    void ApplySidecarMap();
    void ApplySidecarMapTracks(REFERENCE_TIME sourceTarget);
    bool FindSidecarMapSeekOffset(REFERENCE_TIME sourceTarget, long long& byteOffset) const;

    // EDL (multi-segment) helpers. Active only when m_editSegments.size() > 1.
    bool IsMultiSegment() const { return m_editSegments.size() > 1; }
    REFERENCE_TIME EditTotalDuration() const;       // sum of segment durations
    REFERENCE_TIME SegmentBase(int index) const;    // program time at start of segment
    int SegmentAtProgram(REFERENCE_TIME programRT, REFERENCE_TIME& srcRelOut) const;
    // Maps an absolute source PTS to program time for the current segment.
    // Returns false (drop the sample) when the PTS is past the segment end (also
    // requests the next jump) or before the segment start.
    bool EditMapPts(REFERENCE_TIME absPts, REFERENCE_TIME& programOut);
    static void ExtractHevcParamSets(const std::vector<uint8_t>& annexb,
                                     std::vector<uint8_t>& out,
                                     int* width = nullptr,
                                     int* height = nullptr);
    static bool ParseHevcSpsSize(const std::vector<uint8_t>& sps,
                                 int& width, int& height);
    static DWORD WINAPI ThreadProc(LPVOID pv);
    void DemuxLoop();
    bool FindNextSubtitleBegin(int streamIndex, REFERENCE_TIME currentBegin,
                               long long startOffset, REFERENCE_TIME& nextBegin) const;
    void ClearPendingSubtitleCues();
    void FlushPendingSubtitleCue(int streamIndex, REFERENCE_TIME stopTime);
    void FlushAllPendingSubtitleCues(REFERENCE_TIME stopTime);
    void PumpPendingSubtitleChunks(REFERENCE_TIME currentTime);
    void DeliverSubtitleCue(int streamIndex, REFERENCE_TIME start, REFERENCE_TIME stop,
                            const std::vector<std::string>& assEvents,
                            const std::string& assText);
    void ProcessDeferredSubtitleSamples();
    // Maps a TTML begin time to the media (normalized, pre-segment) timeline.
    // Subtitles have no PTS, so the offset is anchored once per TTML timeline
    // to the video position where that timeline is first observed.
    REFERENCE_TIME ResolveSubtitleOffset(REFERENCE_TIME ttmlBegin);
    REFERENCE_TIME SubtitleTimingAnchor() const;

    CCritSec m_pinLock;
    std::vector<CMmtTlvOutputPin*> m_pins;
    std::vector<std::unique_ptr<LatmPcmDecoder>> m_latmPcmDecoders;

    MmtTlv::MmtTlvDemuxer m_demuxer;
    CFilterDemuxerHandler  m_handler;

    std::wstring m_filename;
    std::vector<uint8_t> m_hevcExtradata;
    int m_videoWidth{3840};
    int m_videoHeight{2160};

    HANDLE m_hThread{NULL};
    HANDLE m_hStop{NULL};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_isSeeking{false};

    // Seeking state
    REFERENCE_TIME m_duration{0};      // total file duration (100ns)
    REFERENCE_TIME m_firstPts{-1};     // absolute PTS of first video frame (100ns)
    REFERENCE_TIME m_sourceDuration{0};
    REFERENCE_TIME m_virtualStart{0};
    REFERENCE_TIME m_virtualEnd{0};

    // EDL: ordered source cut segments concatenated into one program timeline.
    // Each is source-relative [start,end) in 100ns. Multi-segment mode is active
    // only when m_editSegments has >1 entry; one or zero segments keep the
    // original single-range (m_virtualStart/m_virtualEnd) path untouched.
    struct EditSeg { REFERENCE_TIME start{0}; REFERENCE_TIME end{0}; };
    std::vector<EditSeg> m_editSegments;
    REFERENCE_TIME m_origFirstPts{-1};       // first video PTS (absolute), pre-offset
    int m_curSegment{0};                     // segment currently being delivered
    REFERENCE_TIME m_curSegmentBase{0};      // program time at start of m_curSegment
    std::atomic<int> m_pendingSegmentJump{-1}; // demux loop: jump to seg N, -2 = EOS, -1 = none

    bool m_hasSidecarIndex{false};
    bool m_hasSidecarMap{false};
    REFERENCE_TIME m_mapDuration{0};
    REFERENCE_TIME m_mapFirstVideoPts{-1};
    REFERENCE_TIME m_stopPos{_I64_MAX};
    std::atomic<REFERENCE_TIME> m_currentPts{0};  // updated from video callback
    std::streamsize m_fileSize{0};
    bool m_audioUnsupported{false};
    bool m_limitAudioToSelected{false};
    double m_rate{1.0};
    REFERENCE_TIME m_seekTarget{0};    // position to seek to (relative, 0 = start)
    REFERENCE_TIME m_segmentStart{0};  // media-time start of the active segment
    std::atomic<bool> m_waitingForVideoRap{false};
    std::atomic<REFERENCE_TIME> m_segmentTimeOffset{0};
    std::atomic<REFERENCE_TIME> m_subtitleTimeOffset{0};
    std::atomic<REFERENCE_TIME> m_lastSubtitleTtmlBegin{-1};
    // Whether m_subtitleTimeOffset holds a calibrated value. A separate flag is
    // required because a legitimate offset can be negative (TTML begin < media PTS),
    // so the previous "< 0 means unset" sentinel mis-fired on every cue.
    std::atomic<bool> m_subtitleOffsetValid{false};
    std::atomic<long long> m_demuxByteOffset{0};

    struct PendingSubtitleCue {
        int streamIndex = -1;
        REFERENCE_TIME start = 0;
        REFERENCE_TIME nextChunkStart = 0;
        std::vector<std::string> assEvents;
        std::string assText;
    };
    std::vector<PendingSubtitleCue> m_pendingSubtitleCues;

    struct DeferredSubtitleSample {
        int streamIndex = -1;
        long long pts = -1;
        LONG callbackNo = 0;
        std::vector<uint8_t> data;
    };
    std::vector<DeferredSubtitleSample> m_deferredSubtitleSamples;

    struct SidecarMapTrack {
        std::string type;
        int streamIndex{-1};
        uint16_t packetId{0};
        int componentTag{-1};
        uint32_t samplingRate{0};
        bool latm{false};
        uint8_t audioMode{0};
        uint16_t channels{0};
    };
    struct SidecarMapPoint {
        REFERENCE_TIME time{0};
        long long offset{0};
    };
    struct SidecarMapMptChange {
        REFERENCE_TIME time{0};
        long long offset{0};
        std::vector<SidecarMapTrack> tracks;
    };
    const SidecarMapMptChange* FindSidecarMapMptChange(REFERENCE_TIME sourceTarget) const;
    std::wstring AudioTimelineLabel(const CFilterDemuxerHandler::AudioStreamInfo& info) const;
    std::vector<SidecarMapTrack> m_sidecarMapTracks;
    std::vector<SidecarMapMptChange> m_sidecarMapMptChanges;
    std::vector<SidecarMapPoint> m_sidecarMapRapPoints;
    std::vector<SidecarMapPoint> m_sidecarMapSeekPoints;
};
