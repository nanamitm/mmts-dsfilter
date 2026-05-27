#include "MmtTlvOutputPin.h"
#include "MmtTlvSplitter.h"
#include "Guids.h"
#include <dvdmedia.h>   // VIDEOINFOHEADER2
#include <strsafe.h>

struct SUBTITLEINFO {
    DWORD dwOffset;
    CHAR IsoLang[4];
    WCHAR TrackName[256];
};

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

static const WCHAR* PinKindName(MmtTlvPinKind kind)
{
    switch (kind) {
    case MmtTlvPinKind::Video:
        return L"Video";
    case MmtTlvPinKind::Audio:
        return L"Audio";
    case MmtTlvPinKind::Subtitle:
        return L"Subtitle";
    default:
        return L"Unknown";
    }
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
    , m_kind(isVideo ? MmtTlvPinKind::Video : MmtTlvPinKind::Audio)
    , m_isVideo(isVideo)
    , m_audioStreamIndex(audioStreamIndex)
{
}

CMmtTlvOutputPin::CMmtTlvOutputPin(MmtTlvPinKind kind, HRESULT* phr,
                                   CBaseFilter* pFilter, CCritSec* pLock, LPCWSTR pName,
                                   int streamIndex)
    : CBaseOutputPin(NAME("MmtTlvOutputPin"), pFilter, pLock, phr, pName)
    , m_kind(kind)
    , m_isVideo(kind == MmtTlvPinKind::Video)
    , m_audioStreamIndex(streamIndex)
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
    m_droppedBeforeSegment = 0;
    if (m_isVideo) {
        m_waitForVideoRap = true;
        m_droppedUntilRap = 0;
        m_videoFirstWallMs = 0;
        m_videoFirstPts = -1;
        m_videoLastPts = -1;
    }
    LogPinMsg(L"MMT/TLV %s ResetForSeek\n", m_isVideo ? L"Video" : L"Audio");
}

void CMmtTlvOutputPin::SetWaitForVideoRap(bool wait)
{
    if (!m_isVideo)
        return;
    m_waitForVideoRap = wait;
    m_droppedUntilRap = 0;
    LogPinMsg(L"MMT/TLV Video wait for RAP %s\n", wait ? L"enabled" : L"disabled");
}

HRESULT CMmtTlvOutputPin::GetMediaType(int iPosition, CMediaType* pmt)
{
    if (iPosition < 0) return E_INVALIDARG;
    if (iPosition > 0) return VFW_S_NO_MORE_ITEMS;

    pmt->InitMediaType();

    if (IsVideo()) {
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
    } else if (IsAudio()) {
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
    } else {
        pmt->SetType(&MEDIATYPE_Subtitle);
        pmt->SetSubtype(&MEDIASUBTYPE_ASS);
        pmt->SetFormatType(&FORMAT_SubtitleInfo);

        static const char kAssHeader[] =
            "[Script Info]\r\n"
            "ScriptType: v4.00+\r\n"
            "WrapStyle: 2\r\n"
            "ScaledBorderAndShadow: yes\r\n"
            "PlayResX: 1920\r\n"
            "PlayResY: 1080\r\n"
            "\r\n"
            "[V4+ Styles]\r\n"
            "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
            "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, "
            "Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\r\n"
            "Style: Default,Noto Sans JP,96,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
            "0,0,0,0,100,100,0,0,1,3,1,2,20,20,20,1\r\n"
            "\r\n"
            "[Events]\r\n"
            "Format: ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n";

        const ULONG headerSize = sizeof(kAssHeader) - 1;
        const ULONG fmtSize = sizeof(SUBTITLEINFO) + headerSize;
        auto* info = reinterpret_cast<SUBTITLEINFO*>(pmt->AllocFormatBuffer(fmtSize));
        if (!info) return E_OUTOFMEMORY;
        ZeroMemory(info, fmtSize);
        info->dwOffset = sizeof(SUBTITLEINFO);
        info->IsoLang[0] = 'j';
        info->IsoLang[1] = 'p';
        info->IsoLang[2] = 'n';
        StringCchCopyW(info->TrackName, ARRAYSIZE(info->TrackName),
                       m_trackName.empty() ? L"MMT/TLV Subtitle" : m_trackName.c_str());
        memcpy(reinterpret_cast<BYTE*>(info) + sizeof(SUBTITLEINFO), kAssHeader, headerSize);
    }

    pmt->SetTemporalCompression(IsVideo() ? TRUE : FALSE);
    pmt->SetVariableSize();
    return S_OK;
}

HRESULT CMmtTlvOutputPin::CheckMediaType(const CMediaType* pmt)
{
    if (IsVideo()) {
        if (*pmt->Type()    != MEDIATYPE_Video)    return VFW_E_TYPE_NOT_ACCEPTED;
        if (*pmt->Subtype() != MEDIASUBTYPE_HEVC)  return VFW_E_TYPE_NOT_ACCEPTED;
    } else if (IsAudio()) {
        if (*pmt->Type()    != MEDIATYPE_Audio)       return VFW_E_TYPE_NOT_ACCEPTED;
        if (*pmt->Subtype() != MEDIASUBTYPE_RAW_AAC1) return VFW_E_TYPE_NOT_ACCEPTED;
    } else {
        if (*pmt->Type()    != MEDIATYPE_Subtitle) return VFW_E_TYPE_NOT_ACCEPTED;
        if (*pmt->Subtype() != MEDIASUBTYPE_ASS)   return VFW_E_TYPE_NOT_ACCEPTED;
    }
    return S_OK;
}

HRESULT CMmtTlvOutputPin::DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pprop)
{
    ALLOCATOR_PROPERTIES actual;
    const bool isVideo = IsVideo();
    const bool isSubtitle = IsSubtitle();
    pprop->cbBuffer = isVideo
        ? 16 * 1024 * 1024
        : (isSubtitle ? 256 * 1024 : 64 * 1024);
    pprop->cBuffers  = isVideo ? 32 : 64;
    pprop->cbAlign   = 1;
    pprop->cbPrefix  = 0;
    HRESULT hr = pAlloc->SetProperties(pprop, &actual);
    LogPinMsg(L"MMT/TLV %s DecideBufferSize: hr=0x%08X requested=%ldx%ld actual=%ldx%ld\n",
              IsVideo() ? L"Video" : (IsAudio() ? L"Audio" : L"Subtitle"),
              hr,
              pprop->cBuffers,
              pprop->cbBuffer,
              actual.cBuffers,
              actual.cbBuffer);
    return hr;
}

HRESULT CMmtTlvOutputPin::Active()
{
    m_accum.clear();
    m_accumPts = -1;
    m_accumDts = -1;
    m_accumKey = false;
    m_firstSample = true;
    m_logNextSample = false;
    m_waitForVideoRap = false;
    m_droppedUntilRap = 0;
    m_droppedBeforeSegment = 0;
    m_videoFirstWallMs = 0;
    m_videoFirstPts = -1;
    m_videoLastPts = -1;

    HRESULT hr = CBaseOutputPin::Active();
    if (FAILED(hr)) return hr;

    delete m_pQueue;
    m_pQueue = nullptr;

    if (m_Connected && !IsSubtitle()) {
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
              PinKindName(m_kind),
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
        if (dropped <= 5 || (dropped % 10) == 0) {
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
        if (m_accumPts >= 0 && m_accumPts < timeOffset) {
            LONG dropped = ++m_droppedBeforeSegment;
            if (dropped <= 5 || (dropped % 30) == 0) {
                LogPinMsg(L"MMT/TLV %s dropping pre-segment sample #%ld: pts=%I64d ms, offset=%I64d ms, size=%zu\n",
                          IsVideo() ? L"Video" : (IsAudio() ? L"Audio" : L"Subtitle"),
                          dropped,
                          m_accumPts / 10000,
                          timeOffset / 10000,
                          m_accum.size());
            }
            m_accum.clear();
            m_accumPts = -1;
            m_accumDts = -1;
            m_accumKey = false;
            return S_OK;
        }
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
        LogPinMsg(L"MMT/TLV %s sample too large: size=%zu, buffer=%ld, dropping pts=%I64d ms\n",
                  IsVideo() ? L"Video" : (IsAudio() ? L"Audio" : L"Subtitle"),
                  m_accum.size(),
                  bufLen,
                  m_accumPts / 10000);
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
    LONGLONG videoFrameStepMs = 0;
    LONGLONG videoDriftMs = 0;
    if (m_isVideo && m_accumPts >= 0) {
        if (m_videoFirstWallMs == 0) {
            m_videoFirstWallMs = tDeliverEnd;
            m_videoFirstPts = m_accumPts;
        }
        if (m_videoLastPts >= 0)
            videoFrameStepMs = (m_accumPts - m_videoLastPts) / 10000;
        m_videoLastPts = m_accumPts;

        const LONGLONG wallElapsedMs = static_cast<LONGLONG>(tDeliverEnd - m_videoFirstWallMs);
        const LONGLONG mediaElapsedMs = (m_accumPts - m_videoFirstPts) / 10000;
        videoDriftMs = wallElapsedMs - mediaElapsedMs;
    }
    const bool forceLog = m_logNextSample;
    m_logNextSample = false;
    if (forceLog || sampleNo <= 10 || (sampleNo % 100) == 0 || bufferMs >= 20 || deliverMs >= 20 || FAILED(hr)) {
        if (m_isVideo) {
            LogPinMsg(L"MMT/TLV Video DeliverSample #%ld: hr=0x%08X, size=%zu, pts=%I64d ms, dts=%I64d ms, key=%d, disc=%d, getbuf=%I64u ms, deliver=%I64u ms, frameStep=%I64d ms, drift=%I64d ms\n",
                      sampleNo,
                      hr,
                      m_accum.size(),
                      m_accumPts / 10000,
                      m_accumDts / 10000,
                      m_accumKey,
                      wasFirstSample,
                      bufferMs,
                      deliverMs,
                      videoFrameStepMs,
                      videoDriftMs);
        } else {
            LogPinMsg(L"MMT/TLV Audio DeliverSample #%ld: hr=0x%08X, size=%zu, pts=%I64d ms, dts=%I64d ms, key=%d, disc=%d, getbuf=%I64u ms, deliver=%I64u ms\n",
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
    }

    m_accum.clear();
    m_accumPts = -1;
    m_accumDts = -1;
    m_accumKey = false;

    return hr;
}

HRESULT CMmtTlvOutputPin::DeliverTextSample(REFERENCE_TIME start, REFERENCE_TIME stop,
                                            const char* text, size_t size)
{
    if (!IsConnected() || !IsSubtitle() || !text || size == 0)
        return S_OK;

    static volatile LONG s_subtitleSamples = 0;
    LONG sampleNo = InterlockedIncrement(&s_subtitleSamples);

    IMediaSample* pSample = nullptr;
    HRESULT hr = GetDeliveryBuffer(&pSample, nullptr, nullptr, 0);
    if (FAILED(hr))
        return hr;

    BYTE* pBuf = nullptr;
    pSample->GetPointer(&pBuf);
    LONG bufLen = pSample->GetSize();
    if (bufLen < static_cast<LONG>(size)) {
        pSample->Release();
        LogPinMsg(L"MMT/TLV Subtitle sample too large: size=%zu, buffer=%ld\n", size, bufLen);
        return S_OK;
    }

    memcpy(pBuf, text, size);
    pSample->SetActualDataLength(static_cast<LONG>(size));
    pSample->SetTime(&start, &stop);
    pSample->SetSyncPoint(TRUE);
    pSample->SetPreroll(FALSE);
    pSample->SetDiscontinuity(sampleNo == 1 ? TRUE : FALSE);

    if (m_pQueue) {
        hr = m_pQueue->Receive(pSample);
    } else {
        hr = Deliver(pSample);
        pSample->Release();
    }

    if (sampleNo <= 20 || FAILED(hr)) {
        int chars = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(size), nullptr, 0);
        std::wstring preview;
        if (chars > 0) {
            preview.resize(static_cast<size_t>(chars));
            MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(size), preview.data(), chars);
            if (preview.size() > 64)
                preview.resize(64);
        } else {
            preview = L"<utf8-convert-failed>";
        }
        LogPinMsg(L"MMT/TLV Subtitle DeliverTextSample #%ld: hr=0x%08X, size=%zu, start=%I64d ms, stop=%I64d ms, text=\"%s\"\n",
                  sampleNo, hr, size, start / 10000, stop / 10000, preview.c_str());
    }
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
