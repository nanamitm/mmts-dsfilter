#pragma once
#include <streams.h>
#include <vector>
#include <cstdint>

class CMmtTlvSplitter;
class COutputQueue;

// The video output pin also implements IMediaSeeking (forwarding to the filter)
// so that MPC-BE's graph manager can enable seeking without a COM identity
// violation.  The audio pin returns E_NOINTERFACE for IMediaSeeking.
class CMmtTlvOutputPin : public CBaseOutputPin, public IMediaSeeking, public IAMStreamSelect {
public:
    CMmtTlvOutputPin(bool isVideo, HRESULT* phr, CBaseFilter* pFilter,
                     CCritSec* pLock, LPCWSTR pName, int audioStreamIndex = -1);
    ~CMmtTlvOutputPin();

    // IUnknown (exposes IMediaSeeking only on the video pin)
    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;

    // CBaseOutputPin
    HRESULT CheckMediaType(const CMediaType* pmt) override;
    HRESULT GetMediaType(int iPosition, CMediaType* pmt) override;
    HRESULT DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pprop) override;
    HRESULT Active() override;
    HRESULT Inactive() override;
    HRESULT DeliverBeginFlush() override;
    HRESULT DeliverEndFlush() override;
    HRESULT DeliverNewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    // IQualityControl
    STDMETHODIMP Notify(IBaseFilter* pSender, Quality q) override;

    // Deliver one ES fragment. Returns S_OK or an error.
    HRESULT DeliverSample(bool keyframe, REFERENCE_TIME pts, REFERENCE_TIME dts,
                          bool isFirstFragment, bool isLastFragment,
                          const uint8_t* data, size_t size);
    HRESULT DeliverEOS();

    // Called by the filter to set stream metadata before connection.
    void SetVideoInfo(int width, int height) { m_width = width; m_height = height; }
    void SetAudioInfo(int sampleRate, int channels, int bitdepth = 16)
        { m_sampleRate = sampleRate; m_channels = channels; m_bitdepth = bitdepth; }
    void SetHevcExtradata(const std::vector<uint8_t>& extra) { m_hevcExtradata = extra; }

    bool IsVideo() const { return m_isVideo; }
    int AudioStreamIndex() const { return m_audioStreamIndex; }

    // Reset accumulation state after a seek (called by SeekTo)
    void ResetForSeek();

    // IMediaSeeking – all methods forward to the filter's implementation.
    // Only the video pin exposes this; audio pin returns E_NOINTERFACE.
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

    // IAMStreamSelect - forwards to the filter implementation.
    STDMETHODIMP Count(DWORD* pcStreams) override;
    STDMETHODIMP Info(long lIndex, AM_MEDIA_TYPE** ppmt, DWORD* pdwFlags,
                      LCID* plcid, DWORD* pdwGroup, LPWSTR* ppszName,
                      IUnknown** ppObject, IUnknown** ppUnk) override;
    STDMETHODIMP Enable(long lIndex, DWORD dwFlags) override;

private:
    CMmtTlvSplitter* Filter() const
        { return reinterpret_cast<CMmtTlvSplitter*>(m_pFilter); }

    bool m_isVideo;
    int m_audioStreamIndex{-1};

    // Video
    int m_width{3840};
    int m_height{2160};
    std::vector<uint8_t> m_hevcExtradata;

    // Audio
    int m_sampleRate{48000};
    int m_channels{2};
    int m_bitdepth{16};

    // Fragment accumulation buffer
    std::vector<uint8_t> m_accum;
    REFERENCE_TIME m_accumPts{-1};
    REFERENCE_TIME m_accumDts{-1};
    bool m_accumKey{false};
    bool m_firstSample{true};
    bool m_logNextSample{false};
    bool m_waitForVideoRap{false};
    LONG m_droppedUntilRap{0};

    COutputQueue* m_pQueue{nullptr};
};
