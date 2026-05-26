#include "MmtTlvOutputPin.h"
#include "MmtTlvSplitter.h"
#include "Guids.h"
#include <dvdmedia.h>   // VIDEOINFOHEADER2
#include <strsafe.h>

static void LogPinMsg(const WCHAR* format, ...)
{
    WCHAR buf[512];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), format, args);
    va_end(args);
    OutputDebugStringW(buf);
}

static bool IsHevcRapNal(uint8_t nalType)
{
    // HEVC IRAP NAL units: BLA(16-18), IDR(19-20), CRA(21).
    return nalType >= 16 && nalType <= 21;
}

static REFERENCE_TIME SubtractTimeOffset(REFERENCE_TIME rt, REFERENCE_TIME offset)
{
    if (rt < 0)
        return rt;
    rt -= offset;
    return rt < 0 ? 0 : rt;
}

CMmtTlvOutputPin::CMmtTlvOutputPin(bool isVideo, HRESULT* phr,
                                   CBaseFilter* pFilter, CCritSec* pLock, LPCWSTR pName,
                                   int audioStreamIndex)
    : CBaseOutputPin(NAME("MmtTlvOutputPin"), pFilter, pLock, phr, pName)
    , m_isVideo(isVideo)
    , m_audioStreamIndex(audioStreamIndex)
{
}

CMmtTlvOutputPin::~CMmtTlvOutputPin()
{
    delete m_pQueue;
}

STDMETHODIMP CMmtTlvOutputPin::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    // Expose IMediaSeeking on both video and audio pins (forwarding to filter)
    // so that the filter graph manager recognizes seeking is supported on all branches.
    if (riid == IID_IMediaSeeking)
        return GetInterface(static_cast<IMediaSeeking*>(this), ppv);
    if (riid == IID_IAMStreamSelect) {
        LogPinMsg(L"MMT/TLV %s pin QI for IAMStreamSelect\n", m_isVideo ? L"Video" : L"Audio");
        return GetInterface(static_cast<IAMStreamSelect*>(this), ppv);
    }
    return CBaseOutputPin::NonDelegatingQueryInterface(riid, ppv);
}

// ---------------------------------------------------------------------------
// IMediaSeeking – forward every call to the filter's implementation
// ---------------------------------------------------------------------------
#define FWD(call) return Filter()->call

STDMETHODIMP CMmtTlvOutputPin::GetCapabilities(DWORD* p)
    { FWD(GetCapabilities(p)); }
STDMETHODIMP CMmtTlvOutputPin::CheckCapabilities(DWORD* p)
    { FWD(CheckCapabilities(p)); }
STDMETHODIMP CMmtTlvOutputPin::IsFormatSupported(const GUID* p)
    { FWD(IsFormatSupported(p)); }
STDMETHODIMP CMmtTlvOutputPin::QueryPreferredFormat(GUID* p)
    { FWD(QueryPreferredFormat(p)); }
STDMETHODIMP CMmtTlvOutputPin::GetTimeFormat(GUID* p)
    { FWD(GetTimeFormat(p)); }
STDMETHODIMP CMmtTlvOutputPin::IsUsingTimeFormat(const GUID* p)
    { FWD(IsUsingTimeFormat(p)); }
STDMETHODIMP CMmtTlvOutputPin::SetTimeFormat(const GUID* p)
    { FWD(SetTimeFormat(p)); }
STDMETHODIMP CMmtTlvOutputPin::GetDuration(LONGLONG* p)
    { FWD(GetDuration(p)); }
STDMETHODIMP CMmtTlvOutputPin::GetStopPosition(LONGLONG* p)
    { FWD(GetStopPosition(p)); }
STDMETHODIMP CMmtTlvOutputPin::GetCurrentPosition(LONGLONG* p)
    { FWD(GetCurrentPosition(p)); }
STDMETHODIMP CMmtTlvOutputPin::ConvertTimeFormat(LONGLONG* pT, const GUID* pTF,
                                                  LONGLONG S, const GUID* pSF)
    { FWD(ConvertTimeFormat(pT, pTF, S, pSF)); }
STDMETHODIMP CMmtTlvOutputPin::SetPositions(LONGLONG* pC, DWORD fC,
                                             LONGLONG* pS, DWORD fS)
    { FWD(SetPositions(pC, fC, pS, fS)); }
STDMETHODIMP CMmtTlvOutputPin::GetPositions(LONGLONG* pC, LONGLONG* pS)
    { FWD(GetPositions(pC, pS)); }
STDMETHODIMP CMmtTlvOutputPin::GetAvailable(LONGLONG* pE, LONGLONG* pL)
    { FWD(GetAvailable(pE, pL)); }
STDMETHODIMP CMmtTlvOutputPin::SetRate(double d)
    { FWD(SetRate(d)); }
STDMETHODIMP CMmtTlvOutputPin::GetRate(double* p)
    { FWD(GetRate(p)); }
STDMETHODIMP CMmtTlvOutputPin::GetPreroll(LONGLONG* p)
    { FWD(GetPreroll(p)); }

STDMETHODIMP CMmtTlvOutputPin::Count(DWORD* pcStreams)
    { FWD(Count(pcStreams)); }
STDMETHODIMP CMmtTlvOutputPin::Info(long lIndex, AM_MEDIA_TYPE** ppmt, DWORD* pdwFlags,
                                    LCID* plcid, DWORD* pdwGroup, LPWSTR* ppszName,
                                    IUnknown** ppObject, IUnknown** ppUnk)
    { FWD(Info(lIndex, ppmt, pdwFlags, plcid, pdwGroup, ppszName, ppObject, ppUnk)); }
STDMETHODIMP CMmtTlvOutputPin::Enable(long lIndex, DWORD dwFlags)
    { FWD(Enable(lIndex, dwFlags)); }

#undef FWD

void CMmtTlvOutputPin::ResetForSeek()
{
    m_accum.clear();
    m_accumPts  = -1;
    m_accumDts  = -1;
    m_accumKey  = false;
    m_firstSample = true;
    m_logNextSample = true;
    if (m_isVideo) {
        m_waitForVideoRap = true;
        m_droppedUntilRap = 0;
    }
    LogPinMsg(L"MMT/TLV %s ResetForSeek\n", m_isVideo ? L"Video" : L"Audio");
}

HRESULT CMmtTlvOutputPin::GetMediaType(int iPosition, CMediaType* pmt)
{
    if (iPosition < 0) return E_INVALIDARG;
    if (iPosition > 0) return VFW_S_NO_MORE_ITEMS;

    pmt->InitMediaType();

    if (m_isVideo) {
        pmt->SetType(&MEDIATYPE_Video);
        pmt->SetSubtype(&MEDIASUBTYPE_HEVC);
        pmt->SetFormatType(&FORMAT_VideoInfo2);

        ULONG fmtSize = sizeof(VIDEOINFOHEADER2) + static_cast<ULONG>(m_hevcExtradata.size());
        VIDEOINFOHEADER2* vih = reinterpret_cast<VIDEOINFOHEADER2*>(
            pmt->AllocFormatBuffer(fmtSize));
        if (!vih) return E_OUTOFMEMORY;
        ZeroMemory(vih, fmtSize);

        vih->AvgTimePerFrame         = 166667; // default 60fps in 100ns units
        vih->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        vih->bmiHeader.biWidth       = m_width;
        vih->bmiHeader.biHeight      = m_height;
        vih->bmiHeader.biPlanes      = 1;
        vih->bmiHeader.biBitCount    = 24;
        vih->bmiHeader.biCompression = MAKEFOURCC('H','E','V','C');
        vih->bmiHeader.biSizeImage   = m_width * m_height * 3 / 2;
        vih->dwPictAspectRatioX      = 16;
        vih->dwPictAspectRatioY      = 9;
        SetRect(&vih->rcSource, 0, 0, m_width, m_height);
        SetRect(&vih->rcTarget, 0, 0, m_width, m_height);

        // Append VPS+SPS+PPS extradata required by MPC-BE internal HEVC decoder
        if (!m_hevcExtradata.empty()) {
            memcpy(reinterpret_cast<BYTE*>(vih) + sizeof(VIDEOINFOHEADER2),
                   m_hevcExtradata.data(), m_hevcExtradata.size());
        }
    } else {
        pmt->SetType(&MEDIATYPE_Audio);
        pmt->SetSubtype(&MEDIASUBTYPE_RAW_AAC1);
        pmt->SetFormatType(&FORMAT_WaveFormatEx);

        WAVEFORMATEX* wf = reinterpret_cast<WAVEFORMATEX*>(
            pmt->AllocFormatBuffer(sizeof(WAVEFORMATEX)));
        if (!wf) return E_OUTOFMEMORY;
        ZeroMemory(wf, sizeof(WAVEFORMATEX));

        wf->wFormatTag      = 0x00FF;  // WAVE_FORMAT_RAW_AAC1
        wf->nChannels       = static_cast<WORD>(m_channels);
        wf->nSamplesPerSec  = static_cast<DWORD>(m_sampleRate);
        wf->wBitsPerSample  = static_cast<WORD>(m_bitdepth);
        wf->nBlockAlign     = 1;
        wf->nAvgBytesPerSec = 0;
        wf->cbSize          = 0;
    }

    pmt->SetTemporalCompression(m_isVideo ? TRUE : FALSE);
    pmt->SetVariableSize();
    return S_OK;
}

HRESULT CMmtTlvOutputPin::CheckMediaType(const CMediaType* pmt)
{
    if (m_isVideo) {
        if (*pmt->Type()    != MEDIATYPE_Video)    return VFW_E_TYPE_NOT_ACCEPTED;
        if (*pmt->Subtype() != MEDIASUBTYPE_HEVC)  return VFW_E_TYPE_NOT_ACCEPTED;
    } else {
        if (*pmt->Type()    != MEDIATYPE_Audio)       return VFW_E_TYPE_NOT_ACCEPTED;
        if (*pmt->Subtype() != MEDIASUBTYPE_RAW_AAC1) return VFW_E_TYPE_NOT_ACCEPTED;
    }
    return S_OK;
}

HRESULT CMmtTlvOutputPin::DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pprop)
{
    ALLOCATOR_PROPERTIES actual;
    // 4K HEVC I-frame can be 3-10MB; audio frames are small
    pprop->cbBuffer = m_isVideo ? 16 * 1024 * 1024 : 64 * 1024;
    pprop->cBuffers  = m_isVideo ? 32 : 64;
    pprop->cbAlign   = 1;
    pprop->cbPrefix  = 0;
    return pAlloc->SetProperties(pprop, &actual);
}

HRESULT CMmtTlvOutputPin::Active()
{
    m_accum.clear();
    m_accumPts = -1;
    m_accumDts = -1;
    m_accumKey = false;
    m_firstSample = true;
    m_logNextSample = false;
    m_waitForVideoRap = m_isVideo;
    m_droppedUntilRap = 0;

    HRESULT hr = CBaseOutputPin::Active();
    if (FAILED(hr)) return hr;

    delete m_pQueue;
    m_pQueue = nullptr;

    if (m_Connected) {
        m_pQueue = new COutputQueue(
            m_Connected,
            &hr,
            FALSE,  // force queued delivery; downstream Receive() can block for clock/decoder pacing
            TRUE,
            1,
            FALSE,
            m_isVideo ? 256 : 512,
            THREAD_PRIORITY_NORMAL);
        if (FAILED(hr)) {
            delete m_pQueue;
            m_pQueue = nullptr;
            return hr;
        }
    }

    LogPinMsg(L"MMT/TLV %s pin Active: queued delivery %s\n",
              m_isVideo ? L"Video" : L"Audio",
              m_pQueue ? L"enabled" : L"disabled");
    return S_OK;
}

HRESULT CMmtTlvOutputPin::Inactive()
{
    delete m_pQueue;
    m_pQueue = nullptr;
    m_accum.clear();
    return CBaseOutputPin::Inactive();
}

HRESULT CMmtTlvOutputPin::DeliverBeginFlush()
{
    if (m_pQueue) {
        m_pQueue->BeginFlush();
        return S_OK;
    }
    return CBaseOutputPin::DeliverBeginFlush();
}

HRESULT CMmtTlvOutputPin::DeliverEndFlush()
{
    if (m_pQueue) {
        m_pQueue->EndFlush();
        return S_OK;
    }
    return CBaseOutputPin::DeliverEndFlush();
}

HRESULT CMmtTlvOutputPin::DeliverNewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate)
{
    if (m_pQueue) {
        m_pQueue->NewSegment(tStart, tStop, dRate);
        return S_OK;
    }
    return CBaseOutputPin::DeliverNewSegment(tStart, tStop, dRate);
}

HRESULT CMmtTlvOutputPin::DeliverSample(
    bool keyframe, REFERENCE_TIME pts, REFERENCE_TIME dts,
    bool isFirstFragment, bool isLastFragment,
    const uint8_t* data, size_t size)
{
    if (!IsConnected()) return S_FALSE;

    static volatile LONG s_videoSamples = 0;
    static volatile LONG s_audioSamples = 0;

    if (isFirstFragment) {
        m_accum.clear();
        m_accumPts = pts;
        m_accumDts = dts;
        m_accumKey = keyframe;
    }

    m_accum.insert(m_accum.end(), data, data + size);

    if (!isLastFragment)
        return S_OK;

    // --- complete AU: allocate and deliver ---
    if (m_accum.empty()) return S_OK;

    // dantto4k may report only some RAP frames as keyframes; detect all HEVC
    // IRAP NAL units so seek recovery starts from a decodable access point.
    if (m_isVideo && !m_accumKey) {
        const uint8_t* p = m_accum.data();
        size_t sz = m_accum.size();
        for (size_t i = 0; i + 4 <= sz; ) {
            if (p[i] == 0 && p[i+1] == 0) {
                size_t naluStart = 0;
                if (p[i+2] == 1) {
                    naluStart = i + 3;
                } else if (i + 5 <= sz && p[i+2] == 0 && p[i+3] == 1) {
                    naluStart = i + 4;
                }
                if (naluStart > 0 && naluStart < sz) {
                    uint8_t nalType = (p[naluStart] >> 1) & 0x3F;
                    if (IsHevcRapNal(nalType)) {
                        m_accumKey = true;
                        break;
                    }
                    i = naluStart + 1;
                    continue;
                }
            }
            i++;
        }
    }

    if (m_isVideo && m_waitForVideoRap && !m_accumKey) {
        LONG dropped = ++m_droppedUntilRap;
        if (dropped <= 5 || (dropped % 30) == 0) {
            LogPinMsg(L"MMT/TLV Video dropping non-RAP while waiting for RAP #%ld: size=%zu, pts=%I64d ms, dts=%I64d ms\n",
                      dropped, m_accum.size(), m_accumPts / 10000, m_accumDts / 10000);
        }
        m_accum.clear();
        m_accumPts = -1;
        m_accumDts = -1;
        m_accumKey = false;
        return S_OK;
    }

    if (m_isVideo && m_waitForVideoRap && m_accumKey) {
        LogPinMsg(L"MMT/TLV Video RAP reached: dropped=%ld, pts=%I64d ms, dts=%I64d ms\n",
                  m_droppedUntilRap, m_accumPts / 10000, m_accumDts / 10000);
        m_waitForVideoRap = false;
        Filter()->NotifyVideoRap(m_accumPts);
    }

    if (!m_isVideo && Filter()->IsWaitingForVideoRap()) {
        LONG dropped = ++m_droppedUntilRap;
        if (dropped <= 5 || (dropped % 30) == 0) {
            LogPinMsg(L"MMT/TLV Audio dropping while waiting for video RAP #%ld: size=%zu, pts=%I64d ms, dts=%I64d ms\n",
                      dropped, m_accum.size(), m_accumPts / 10000, m_accumDts / 10000);
        }
        m_accum.clear();
        m_accumPts = -1;
        m_accumDts = -1;
        m_accumKey = false;
        return S_OK;
    }

    const REFERENCE_TIME timeOffset = Filter()->GetSegmentTimeOffset();
    if (timeOffset > 0) {
        m_accumPts = SubtractTimeOffset(m_accumPts, timeOffset);
        m_accumDts = SubtractTimeOffset(m_accumDts, timeOffset);
    }

    LONG sampleNo = InterlockedIncrement(m_isVideo ? &s_videoSamples : &s_audioSamples);
    const ULONGLONG tBufferStart = GetTickCount64();

    IMediaSample* pSample = nullptr;
    HRESULT hr = GetDeliveryBuffer(&pSample, nullptr, nullptr, 0);
    const ULONGLONG tBufferEnd = GetTickCount64();
    if (FAILED(hr)) return hr;

    BYTE* pBuf = nullptr;
    pSample->GetPointer(&pBuf);
    LONG bufLen = pSample->GetSize();

    if (bufLen < static_cast<LONG>(m_accum.size())) {
        // Frame too large for allocated buffer - skip and continue
        DbgLog((LOG_ERROR, 1, TEXT("MmtTlvOutputPin: frame %zu > buffer %ld, dropping"),
                m_accum.size(), bufLen));
        pSample->Release();
        m_accum.clear();
        return S_OK;
    }

    memcpy(pBuf, m_accum.data(), m_accum.size());
    pSample->SetActualDataLength(static_cast<LONG>(m_accum.size()));

    if (m_accumPts != -1) {
        REFERENCE_TIME rtStart = m_accumPts;
        REFERENCE_TIME rtEnd;
        if (m_isVideo) {
            rtEnd = rtStart + 166667; // 60fps duration in 100ns units
        } else {
            // Calculate correct audio frame duration (1024 samples at m_sampleRate) to avoid gaps
            REFERENCE_TIME rtDuration = (m_sampleRate > 0)
                ? (1024 * 10000000LL / m_sampleRate) : 213333;
            rtEnd = rtStart + rtDuration;
        }
        pSample->SetTime(&rtStart, &rtEnd);
    }

    const bool wasFirstSample = m_firstSample;
    pSample->SetSyncPoint(m_isVideo
        ? ((m_accumKey || wasFirstSample) ? TRUE : FALSE)
        : TRUE);
    pSample->SetPreroll(FALSE);
    pSample->SetDiscontinuity(wasFirstSample ? TRUE : FALSE);
    m_firstSample = false;

    const ULONGLONG tDeliverStart = GetTickCount64();
    if (m_pQueue) {
        hr = m_pQueue->Receive(pSample);
    } else {
        hr = Deliver(pSample);
        pSample->Release();
    }
    const ULONGLONG tDeliverEnd = GetTickCount64();

    const ULONGLONG bufferMs = tBufferEnd - tBufferStart;
    const ULONGLONG deliverMs = tDeliverEnd - tDeliverStart;
    const bool forceLog = m_logNextSample;
    m_logNextSample = false;
    if (forceLog || sampleNo <= 10 || (sampleNo % 100) == 0 || bufferMs >= 20 || deliverMs >= 20 || FAILED(hr)) {
        LogPinMsg(L"MMT/TLV %s DeliverSample #%ld: hr=0x%08X, size=%zu, pts=%I64d ms, dts=%I64d ms, key=%d, disc=%d, getbuf=%I64u ms, deliver=%I64u ms\n",
                  m_isVideo ? L"Video" : L"Audio",
                  sampleNo,
                  hr,
                  m_accum.size(),
                  m_accumPts / 10000,
                  m_accumDts / 10000,
                  m_accumKey,
                  wasFirstSample,
                  bufferMs,
                  deliverMs);
    }

    m_accum.clear();
    m_accumPts = -1;
    m_accumDts = -1;
    m_accumKey = false;

    return hr;
}

HRESULT CMmtTlvOutputPin::DeliverEOS()
{
    if (!IsConnected()) return S_OK;
    if (m_pQueue) {
        m_pQueue->EOS();
        return S_OK;
    }
    return DeliverEndOfStream();
}

STDMETHODIMP CMmtTlvOutputPin::Notify(IBaseFilter* pSender, Quality q)
{
    UNREFERENCED_PARAMETER(pSender);
    UNREFERENCED_PARAMETER(q);
    return E_NOTIMPL;
}
