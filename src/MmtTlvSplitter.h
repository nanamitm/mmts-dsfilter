#pragma once
#include <streams.h>
#include <string>
#include <vector>
#include <atomic>
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

private:
    void CreatePins();
    void StartThread();
    void StopThread();
    void SeekTo(REFERENCE_TIME pos);
    void PreScanFile();
    static void ExtractHevcParamSets(const std::vector<uint8_t>& annexb,
                                     std::vector<uint8_t>& out,
                                     int* width = nullptr,
                                     int* height = nullptr);
    static bool ParseHevcSpsSize(const std::vector<uint8_t>& sps,
                                 int& width, int& height);
    static DWORD WINAPI ThreadProc(LPVOID pv);
    void DemuxLoop();

    CCritSec m_pinLock;
    std::vector<CMmtTlvOutputPin*> m_pins;

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
    REFERENCE_TIME m_stopPos{_I64_MAX};
    std::atomic<REFERENCE_TIME> m_currentPts{0};  // updated from video callback
    std::streamsize m_fileSize{0};
    bool m_audioUnsupported{false};
    double m_rate{1.0};
    REFERENCE_TIME m_seekTarget{0};    // position to seek to (relative, 0 = start)
    REFERENCE_TIME m_segmentStart{0};  // media-time start of the active segment
    std::atomic<bool> m_waitingForVideoRap{false};
    std::atomic<REFERENCE_TIME> m_segmentTimeOffset{0};
    std::atomic<REFERENCE_TIME> m_subtitleTimeOffset{-1};
};
