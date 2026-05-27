#include "MmtTlvSplitter.h"
#include "Guids.h"
#include "stream.h"     // MmtTlv::Common::ReadStream
#include "ttml.h"
#include <fstream>
#include <strmif.h>
#include <cwchar>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

static const WCHAR kFilterName[] = L"MMT/TLV Splitter";

#include <strsafe.h>
static void LogMsg(const WCHAR* format, ...)
{
    WCHAR buf[512];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), format, args);
    va_end(args);
    OutputDebugStringW(buf);
}

static const WCHAR* PositioningName(DWORD flags)
{
    switch (flags & AM_SEEKING_PositioningBitsMask) {
    case AM_SEEKING_NoPositioning:          return L"none";
    case AM_SEEKING_AbsolutePositioning:    return L"absolute";
    case AM_SEEKING_RelativePositioning:    return L"relative";
    case AM_SEEKING_IncrementalPositioning: return L"incremental";
    default:                                return L"unknown";
    }
}

static REFERENCE_TIME ToSegmentTime(REFERENCE_TIME rt, REFERENCE_TIME segmentStart)
{
    if (rt < 0)
        return rt;
    rt -= segmentStart;
    return rt < 0 ? 0 : rt;
}

static LPWSTR AllocStreamName(const WCHAR* format, int listIndex, const CFilterDemuxerHandler::AudioStreamInfo& info)
{
    WCHAR buf[128];
    StringCchPrintfW(buf, ARRAYSIZE(buf), format, listIndex, info.streamIndex, info.componentTag);
    size_t chars = wcslen(buf) + 1;
    LPWSTR name = static_cast<LPWSTR>(CoTaskMemAlloc(chars * sizeof(WCHAR)));
    if (name)
        StringCchCopyW(name, chars, buf);
    return name;
}

static std::string ExtractTtmlPlainText(const uint8_t* data, size_t size)
{
    if (!data || size == 0)
        return {};

    std::string xml(reinterpret_cast<const char*>(data), size);
    TTML ttml = TTMLPaser::parse(xml);
    std::ostringstream text;

    for (const auto& div : ttml.divTags) {
        for (const auto& p : div.pTags) {
            bool wroteLine = false;
            for (const auto& span : p.spanTags) {
                if (!span.text.empty()) {
                    text << span.text;
                    wroteLine = true;
                }
            }
            if (wroteLine)
                text << "\n";
        }
    }

    std::string out = text.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

struct TtmlDebugStats {
    size_t divs = 0;
    size_t paragraphs = 0;
    size_t spans = 0;
    size_t textBytes = 0;
};

struct TtmlTextCue {
    std::string text;
    std::string assText;
    bool hasBegin = false;
    bool hasEnd = false;
    REFERENCE_TIME begin = 0;
    REFERENCE_TIME end = 0;
};

static std::string EscapeAssText(const std::string& text);

static bool TryGetLength(const TTMLCssValue& value, float& length)
{
    try {
        TTMLCssValueLength v = value.getValue<TTMLCssValueLength>();
        if (v.unit == "px") {
            length = v.value;
            return true;
        }
    } catch (const std::bad_variant_access&) {
    }
    return false;
}

static bool TryGetLengthPair(const std::optional<TTMLCssValuePair>& pair, float& first, float& second)
{
    if (!pair.has_value())
        return false;
    return TryGetLength(pair->first, first) && TryGetLength(pair->second, second);
}

static std::string FormatAssTag(const char* format, int value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, value);
    return std::string(buf);
}

static std::string FormatAssColorTag(const TTMLCssValueColor& color)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\\c&H%02X%02X%02X&",
                  color.b, color.g, color.r);
    std::string tag(buf);
    if (color.a != 255) {
        const int assAlpha = 255 - color.a;
        std::snprintf(buf, sizeof(buf), "\\alpha&H%02X&", assAlpha);
        tag += buf;
    }
    return tag;
}

static std::string BuildAssStyleTags(const TTMLSpanTag* span)
{
    std::string tags;
    if (!span)
        return tags;

    float fontWidth = 0;
    float fontHeight = 0;
    if (TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight) && fontHeight > 0) {
        const int assFontSize = (std::max)(1, static_cast<int>(std::lround(fontHeight * 1080.0 / 2160.0)));
        tags += FormatAssTag("\\fs%d", assFontSize);
    }

    if (span->style.color.has_value()) {
        try {
            tags += FormatAssColorTag(span->style.color->getValue<TTMLCssValueColor>());
        } catch (const std::bad_variant_access&) {
        }
    }

    return tags;
}

static std::string BuildAssPositionTags(const TTMLPTag& p, bool lowerByOneLine)
{
    float originX = 0;
    float originY = 0;
    if (TryGetLengthPair(p.region.origin, originX, originY)) {
        float extentX = 0;
        float extentY = 0;
        const bool hasExtent = TryGetLengthPair(p.region.extent, extentX, extentY);
        const double anchorX = hasExtent ? (originX + extentX / 2.0f) : originX;
        const int x = static_cast<int>(std::lround(anchorX * 1920.0 / 3840.0));
        float yOffset = 0;
        if (lowerByOneLine && !p.spanTags.empty()) {
            const auto& style = p.spanTags.begin()->style;
            if (style.lineHeight.has_value()) {
                TryGetLength(*style.lineHeight, yOffset);
            }
            if (yOffset <= 0) {
                float fontWidth = 0;
                float fontHeight = 0;
                if (TryGetLengthPair(style.fontSize, fontWidth, fontHeight))
                    yOffset = fontHeight;
            }
        }
        const int y = static_cast<int>(std::lround((originY + yOffset) * 1080.0 / 2160.0));
        return std::string(hasExtent ? "\\an8" : "\\an7") +
               FormatAssTag("\\pos(%d", x) + FormatAssTag(",%d)", y);
    }

    return "\\an2\\pos(960,980)";
}

static size_t CountSubtitleLines(const TTMLDivTag& div)
{
    size_t lines = 0;
    for (const auto& p : div.pTags) {
        bool hasText = false;
        for (const auto& span : p.spanTags) {
            if (!span.text.empty()) {
                hasText = true;
                lines += static_cast<size_t>(std::count(span.text.begin(), span.text.end(), '\n'));
            }
        }
        if (hasText)
            ++lines;
    }
    return lines;
}

static TtmlTextCue ExtractTtmlPlainText(const uint8_t* data, size_t size, TtmlDebugStats& stats)
{
    stats = {};
    TtmlTextCue cue;
    if (!data || size == 0)
        return cue;

    std::string xml(reinterpret_cast<const char*>(data), size);
    TTML ttml = TTMLPaser::parse(xml);
    std::ostringstream text;
    std::ostringstream ass;
    bool wroteAssParagraph = false;

    for (const auto& div : ttml.divTags) {
        ++stats.divs;
        const bool lowerByOneLine = CountSubtitleLines(div) > 1;
        if (div.begin.has_value()) {
            const REFERENCE_TIME begin = static_cast<REFERENCE_TIME>(*div.begin) * 10000;
            cue.begin = cue.hasBegin ? (std::min)(cue.begin, begin) : begin;
            cue.hasBegin = true;
        }
        if (div.end.has_value()) {
            const REFERENCE_TIME end = static_cast<REFERENCE_TIME>(*div.end) * 10000;
            cue.end = cue.hasEnd ? (std::max)(cue.end, end) : end;
            cue.hasEnd = true;
        }
        for (const auto& p : div.pTags) {
            ++stats.paragraphs;
            bool wroteLine = false;
            bool wroteAssLine = false;
            const TTMLSpanTag* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
            for (const auto& span : p.spanTags) {
                ++stats.spans;
                if (!span.text.empty()) {
                    text << span.text;
                    if (!wroteAssParagraph) {
                        ass << "0,0,Default,,0,0,0,,{"
                            << BuildAssPositionTags(p, lowerByOneLine)
                            << BuildAssStyleTags(firstSpan)
                            << "}";
                        wroteAssParagraph = true;
                    } else if (!wroteAssLine) {
                        ass << "\\N";
                    }
                    std::string spanTags = BuildAssStyleTags(&span);
                    if (!spanTags.empty())
                        ass << "{" << spanTags << "}";
                    ass << EscapeAssText(span.text);
                    stats.textBytes += span.text.size();
                    wroteLine = true;
                    wroteAssLine = true;
                }
            }
            if (wroteLine)
                text << "\n";
        }
    }

    std::string out = text.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    cue.text = std::move(out);
    cue.assText = ass.str();
    if (cue.assText.empty() && !cue.text.empty())
        cue.assText = "0,0,Default,,0,0,0,,{\\an2\\pos(960,980)}" + EscapeAssText(cue.text);
    return cue;
}

static std::wstring Utf8Preview(const std::string& text, size_t maxBytes = 96)
{
    std::string clipped = text.substr(0, maxBytes);
    int chars = MultiByteToWideChar(CP_UTF8, 0, clipped.c_str(), static_cast<int>(clipped.size()), nullptr, 0);
    if (chars <= 0)
        return L"<utf8-convert-failed>";

    std::wstring wide(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, clipped.c_str(), static_cast<int>(clipped.size()),
                        wide.data(), chars);
    return wide;
}

static std::string EscapeAssText(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '{':
            escaped += "\\{";
            break;
        case '}':
            escaped += "\\}";
            break;
        case '\r':
            break;
        case '\n':
            escaped += "\\N";
            break;
        default:
            escaped.push_back(c);
            break;
        }
    }
    return escaped;
}

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& data) : m_data(data) {}

    uint32_t ReadBits(int bits)
    {
        uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            if (m_bitPos >= m_data.size() * 8)
                throw std::out_of_range("bitstream exhausted");
            value <<= 1;
            value |= (m_data[m_bitPos / 8] >> (7 - (m_bitPos % 8))) & 1;
            ++m_bitPos;
        }
        return value;
    }

    uint32_t ReadBit()
    {
        return ReadBits(1);
    }

    uint32_t ReadUE()
    {
        int zeros = 0;
        while (ReadBit() == 0) {
            ++zeros;
            if (zeros > 31)
                throw std::out_of_range("invalid Exp-Golomb code");
        }
        uint32_t suffix = zeros ? ReadBits(zeros) : 0;
        return (1u << zeros) - 1 + suffix;
    }

private:
    const std::vector<uint8_t>& m_data;
    size_t m_bitPos = 0;
};

static std::vector<uint8_t> RemoveHevcEmulationPrevention(const uint8_t* data, size_t size)
{
    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        if (i + 2 < size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x03) {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2;
            continue;
        }
        rbsp.push_back(data[i]);
    }
    return rbsp;
}

static void SkipHevcProfileTierLevel(BitReader& br, int maxSubLayersMinus1)
{
    br.ReadBits(2);   // general_profile_space
    br.ReadBit();     // general_tier_flag
    br.ReadBits(5);   // general_profile_idc
    br.ReadBits(32);  // general_profile_compatibility_flags
    br.ReadBits(4);   // general constraint flags
    br.ReadBits(32);
    br.ReadBits(12);  // remaining constraint flags
    br.ReadBits(8);   // general_level_idc

    bool subLayerProfilePresent[8]{};
    bool subLayerLevelPresent[8]{};
    for (int i = 0; i < maxSubLayersMinus1; ++i) {
        subLayerProfilePresent[i] = br.ReadBit() != 0;
        subLayerLevelPresent[i] = br.ReadBit() != 0;
    }
    if (maxSubLayersMinus1 > 0) {
        for (int i = maxSubLayersMinus1; i < 8; ++i)
            br.ReadBits(2);
    }
    for (int i = 0; i < maxSubLayersMinus1; ++i) {
        if (subLayerProfilePresent[i]) {
            br.ReadBits(2);
            br.ReadBit();
            br.ReadBits(5);
            br.ReadBits(32);
            br.ReadBits(4);
            br.ReadBits(32);
            br.ReadBits(12);
        }
        if (subLayerLevelPresent[i])
            br.ReadBits(8);
    }
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
CUnknown* WINAPI CMmtTlvSplitter::CreateInstance(LPUNKNOWN pUnk, HRESULT* phr)
{
    return new(std::nothrow) CMmtTlvSplitter(pUnk, phr);
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
CMmtTlvSplitter::CMmtTlvSplitter(LPUNKNOWN pUnk, HRESULT* phr)
    : CBaseFilter(kFilterName, pUnk, &m_pinLock, CLSID_MmtTlvSplitter)
{
    m_hStop = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!m_hStop && phr)
        *phr = E_OUTOFMEMORY;
}

CMmtTlvSplitter::~CMmtTlvSplitter()
{
    StopThread();
    for (auto* p : m_pins) delete p;
    if (m_hStop) CloseHandle(m_hStop);
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------
STDMETHODIMP CMmtTlvSplitter::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    if (riid == IID_IFileSourceFilter) {
        LogMsg(L"MMT/TLV Splitter: QI for IFileSourceFilter\n");
        return GetInterface(static_cast<IFileSourceFilter*>(this), ppv);
    }
    if (riid == IID_IAMFilterMiscFlags) {
        LogMsg(L"MMT/TLV Splitter: QI for IAMFilterMiscFlags\n");
        return GetInterface(static_cast<IAMFilterMiscFlags*>(this), ppv);
    }
    if (riid == IID_IMediaSeeking) {
        LogMsg(L"MMT/TLV Splitter: QI for IMediaSeeking\n");
        return GetInterface(static_cast<IMediaSeeking*>(this), ppv);
    }
    if (riid == IID_IAMStreamSelect) {
        LogMsg(L"MMT/TLV Splitter: QI for IAMStreamSelect\n");
        return GetInterface(static_cast<IAMStreamSelect*>(this), ppv);
    }
    return CBaseFilter::NonDelegatingQueryInterface(riid, ppv);
}

// ---------------------------------------------------------------------------
// IFileSourceFilter
// ---------------------------------------------------------------------------
STDMETHODIMP CMmtTlvSplitter::Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE*)
{
    if (!pszFileName) return E_POINTER;
    CAutoLock lock(&m_pinLock);
    m_filename = pszFileName;
    m_handler.reset();
    m_handler.resetAudioSelection();
    m_videoWidth = 3840;
    m_videoHeight = 2160;
    m_audioUnsupported = false;
    m_seekTarget = 0;
    m_currentPts = 0;
    m_segmentStart = 0;
    m_segmentTimeOffset.store(0, std::memory_order_release);
    m_firstSubtitleTime.store(-1, std::memory_order_release);
    m_waitingForVideoRap.store(false, std::memory_order_release);

    LogMsg(L"MMT/TLV Splitter: Load called for %s\n", pszFileName);

    // Get file size
    std::ifstream tmp(m_filename, std::ios::binary | std::ios::ate);
    bool openOk = tmp.is_open();
    if (openOk) {
        m_fileSize = static_cast<std::streamsize>(tmp.tellg());
        tmp.close();
    }
    LogMsg(L"MMT/TLV Splitter: File open status = %d, size = %I64d bytes\n", openOk, m_fileSize);

    PreScanFile();   // sets m_hevcExtradata, m_firstPts, m_duration
    CreatePins();
    return S_OK;
}

// ---------------------------------------------------------------------------
// Pre-scan: two-phase scan using the same demuxer so MPT knowledge is shared.
//
// Phase 1 – read from the beginning until the first complete video AU:
//   • extract VPS+SPS+PPS extradata
//   • capture m_firstPts
//
// Phase 2 – seek to the last ~20 MB and continue with the same demuxer
//   (which already knows the stream IDs from Phase 1's MPT) to find the
//   last video PTS and compute m_duration.
// ---------------------------------------------------------------------------
void CMmtTlvSplitter::PreScanFile()
{
    m_hevcExtradata.clear();
    m_videoWidth = 3840;
    m_videoHeight = 2160;
    m_firstPts  = -1;
    m_duration  = 0;

    LogMsg(L"MMT/TLV Splitter: PreScanFile starting...\n");

    std::ifstream ifs(m_filename, std::ios::binary);
    if (!ifs.is_open()) {
        LogMsg(L"MMT/TLV Splitter: PreScanFile failed to open stream for read!\n");
        return;
    }

    MmtTlv::MmtTlvDemuxer demuxer;
    CFilterDemuxerHandler  handler;

    // ---- Phase 1: beginning of file ----------------------------------------
    bool phase1Done = false;
    std::vector<uint8_t> accumVideo;
    long long minPts = -1;

    handler.setVideoCallback(
        [&](int, bool, long long pts, long long, bool isFirst, bool isLast,
            const uint8_t* data, size_t size)
        {
            if (phase1Done) return;
            if (isFirst) {
                accumVideo.clear();
            }
            if (pts >= 0) {
                if (minPts < 0 || pts < minPts) {
                    minPts = pts;
                }
            }
            accumVideo.insert(accumVideo.end(), data, data + size);
            if (isLast && !accumVideo.empty()) {
                if (m_hevcExtradata.empty()) {
                    ExtractHevcParamSets(accumVideo, m_hevcExtradata, &m_videoWidth, &m_videoHeight);
                    if (!m_hevcExtradata.empty()) {
                        LogMsg(L"MMT/TLV Splitter: PreScanFile successfully extracted HEVC Extradata, video=%dx%d\n",
                               m_videoWidth, m_videoHeight);
                    }
                }
            }
        });

    handler.setAudioCallback(
        [&](int, bool, long long pts, long long, bool, bool, const uint8_t*, size_t)
        {
            if (phase1Done) return;
            if (pts >= 0) {
                if (minPts < 0 || pts < minPts) {
                    minPts = pts;
                }
            }
        });

    demuxer.setDemuxerHandler(handler);

    constexpr size_t kChunk   = 65536;
    constexpr size_t kMaxScan = 20 * 1024 * 1024;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    size_t totalRead = 0;

    while (totalRead < kMaxScan) {
        if (!m_hevcExtradata.empty() && minPts >= 0 && totalRead >= 4 * 1024 * 1024) {
            phase1Done = true;
            break;
        }

        size_t oldSz = buf.size();
        buf.resize(oldSz + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
        size_t got = static_cast<size_t>(ifs.gcount());
        buf.resize(oldSz + got);
        if (got == 0) break;
        totalRead += got;

        MmtTlv::Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            auto status = demuxer.demux(stream);
            if (status == MmtTlv::DemuxStatus::NotEnoughBuffer) break;
        }
        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) buf.erase(buf.begin(), buf.begin() + consumed);
    }

    if (minPts >= 0) {
        m_firstPts = minPts;
    }
    {
        auto discoveredAudioStreams = handler.getAudioStreams();
        auto convertibleAudioStreams = handler.getAdtsConvertibleAudioStreams();
        if (!discoveredAudioStreams.empty() && convertibleAudioStreams.empty()) {
            m_audioUnsupported = true;
            m_handler.setKnownAudioStreams({});
            m_handler.setRequireAdtsConvertibleAudio(true);
            LogMsg(L"MMT/TLV Splitter: audio streams found but no ADTS-convertible stream; audio pins disabled\n");
        } else if (!convertibleAudioStreams.empty()) {
            m_handler.setKnownAudioStreams(convertibleAudioStreams);
            m_handler.setRequireAdtsConvertibleAudio(true);
        } else {
            m_handler.setKnownAudioStreams(discoveredAudioStreams);
            m_handler.setRequireAdtsConvertibleAudio(false);
        }
        m_handler.setAudioStreamListLocked(true);
    }
    m_handler.setKnownSubtitleStreams(handler.getSubtitleStreams());
    {
        auto streams = m_handler.getAudioStreams();
        LogMsg(L"MMT/TLV Splitter: PreScan audio stream count=%zu\n", streams.size());
        for (size_t i = 0; i < streams.size(); ++i) {
            const auto& info = streams[i];
            LogMsg(L"MMT/TLV Splitter: PreScan audio[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, samplingRate=%u\n",
                   i, info.streamIndex, info.packetId, info.componentTag, info.samplingRate);
        }
    }
    {
        auto streams = m_handler.getSubtitleStreams();
        LogMsg(L"MMT/TLV Splitter: PreScan subtitle stream count=%zu\n", streams.size());
        for (size_t i = 0; i < streams.size(); ++i) {
            const auto& info = streams[i];
            LogMsg(L"MMT/TLV Splitter: PreScan subtitle[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d\n",
                   i, info.streamIndex, info.packetId, info.componentTag);
        }
    }

    LogMsg(L"MMT/TLV Splitter: Phase 1 finished. minPts=%I64d, phase1Done=%d, totalRead=%d\n",
           m_firstPts, phase1Done, totalRead);

    if (m_firstPts < 0 || m_fileSize <= 0) {
        LogMsg(L"MMT/TLV Splitter: PreScanFile early abort due to negative firstPts or 0 fileSize!\n");
        return;
    }

    // ---- Phase 2: tail scan (same demuxer – MPT/stream IDs still known) ----
    constexpr std::streamsize kTailSize = 20 * 1024 * 1024;
    std::streamsize tailOffset = m_fileSize - kTailSize;
    if (tailOffset < 0) tailOffset = 0;

    LogMsg(L"MMT/TLV Splitter: PreScanFile Phase 2 starting from tail offset %I64d...\n", tailOffset);

    ifs.clear();
    ifs.seekg(tailOffset);
    if (!ifs.good()) {
        LogMsg(L"MMT/TLV Splitter: PreScanFile Phase 2 seek failed!\n");
        return;
    }

    // Reset per-stream processing state without losing stream registration
    demuxer.resetStreams();

    REFERENCE_TIME lastPts = m_firstPts;

    handler.setVideoCallback(
        [&](int, bool, long long pts, long long, bool, bool isLast,
            const uint8_t*, size_t)
        {
            if (isLast && pts > lastPts) {
                lastPts = pts;
            }
        });

    buf.clear();
    buf.reserve(kChunk * 2);

    while (true) {
        size_t oldSz = buf.size();
        buf.resize(oldSz + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
        size_t got = static_cast<size_t>(ifs.gcount());
        buf.resize(oldSz + got);
        if (got == 0) break;

        MmtTlv::Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            auto status = demuxer.demux(stream);
            if (status == MmtTlv::DemuxStatus::NotEnoughBuffer) break;
        }
        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) buf.erase(buf.begin(), buf.begin() + consumed);
    }

    if (lastPts > m_firstPts) {
        m_duration = lastPts - m_firstPts;
        LogMsg(L"MMT/TLV Splitter: PreScanFile succeeded. firstPts=%I64d ms, lastPts=%I64d ms, duration=%I64d ms\n",
               m_firstPts / 10000, lastPts / 10000, m_duration / 10000);
    } else {
        LogMsg(L"MMT/TLV Splitter: PreScanFile failed or duration is 0. firstPts=%I64d, lastPts=%I64d\n",
               m_firstPts, lastPts);
    }
}

// Extract VPS(32)+SPS(33)+PPS(34) from AnnexB stream and build
// 2-byte length-prefix extradata [len_hi][len_lo][NALU] for each parameter set.
void CMmtTlvSplitter::ExtractHevcParamSets(const std::vector<uint8_t>& annexb,
                                           std::vector<uint8_t>& out,
                                           int* width,
                                           int* height)
{
    const uint8_t* p = annexb.data();
    size_t sz = annexb.size();

    auto nextSC = [&](size_t from) -> size_t {
        for (size_t i = from; i + 3 <= sz; i++) {
            if (p[i] == 0 && p[i+1] == 0) {
                if (p[i+2] == 1) return i;
                if (i + 4 <= sz && p[i+2] == 0 && p[i+3] == 1) return i;
            }
        }
        return sz;
    };

    std::vector<uint8_t> vps, sps, pps;
    size_t pos = 0;
    while (pos < sz) {
        size_t sc = nextSC(pos);
        if (sc >= sz) break;
        int scLen = (p[sc+2] == 0) ? 4 : 3;
        size_t naluStart = sc + scLen;
        if (naluStart >= sz) break;

        uint8_t nalType = (p[naluStart] >> 1) & 0x3F;
        size_t naluEnd = nextSC(naluStart + 2);

        if      (nalType == 32 && vps.empty()) vps.assign(p + naluStart, p + naluEnd);
        else if (nalType == 33 && sps.empty()) sps.assign(p + naluStart, p + naluEnd);
        else if (nalType == 34 && pps.empty()) pps.assign(p + naluStart, p + naluEnd);

        if (!vps.empty() && !sps.empty() && !pps.empty()) break;
        pos = naluEnd;
    }

    if (vps.empty() || sps.empty() || pps.empty()) return;

    int parsedWidth = 0;
    int parsedHeight = 0;
    if (ParseHevcSpsSize(sps, parsedWidth, parsedHeight)) {
        if (width)
            *width = parsedWidth;
        if (height)
            *height = parsedHeight;
    }

    out.clear();
    const uint8_t startCode[] = {0, 0, 0, 1};
    for (auto* nalu : {&vps, &sps, &pps}) {
        out.insert(out.end(), std::begin(startCode), std::end(startCode));
        out.insert(out.end(), nalu->begin(), nalu->end());
    }
}

bool CMmtTlvSplitter::ParseHevcSpsSize(const std::vector<uint8_t>& sps,
                                       int& width, int& height)
{
    if (sps.size() < 4)
        return false;

    try {
        std::vector<uint8_t> rbsp = RemoveHevcEmulationPrevention(sps.data() + 2, sps.size() - 2);
        BitReader br(rbsp);

        br.ReadBits(4); // sps_video_parameter_set_id
        const int maxSubLayersMinus1 = static_cast<int>(br.ReadBits(3));
        br.ReadBit(); // sps_temporal_id_nesting_flag
        SkipHevcProfileTierLevel(br, maxSubLayersMinus1);

        br.ReadUE(); // sps_seq_parameter_set_id
        const uint32_t chromaFormatIdc = br.ReadUE();
        bool separateColourPlane = false;
        if (chromaFormatIdc == 3)
            separateColourPlane = br.ReadBit() != 0;

        uint32_t picWidth = br.ReadUE();
        uint32_t picHeight = br.ReadUE();

        uint32_t confWinLeft = 0;
        uint32_t confWinRight = 0;
        uint32_t confWinTop = 0;
        uint32_t confWinBottom = 0;
        if (br.ReadBit()) {
            confWinLeft = br.ReadUE();
            confWinRight = br.ReadUE();
            confWinTop = br.ReadUE();
            confWinBottom = br.ReadUE();
        }

        uint32_t subWidthC = 1;
        uint32_t subHeightC = 1;
        if (!separateColourPlane) {
            if (chromaFormatIdc == 1) {
                subWidthC = 2;
                subHeightC = 2;
            } else if (chromaFormatIdc == 2) {
                subWidthC = 2;
                subHeightC = 1;
            }
        }

        const uint32_t cropW = subWidthC * (confWinLeft + confWinRight);
        const uint32_t cropH = subHeightC * (confWinTop + confWinBottom);
        if (picWidth <= cropW || picHeight <= cropH)
            return false;

        width = static_cast<int>(picWidth - cropW);
        height = static_cast<int>(picHeight - cropH);
        return width > 0 && height > 0;
    } catch (const std::exception&) {
        return false;
    }
}

STDMETHODIMP CMmtTlvSplitter::GetCurFile(LPOLESTR* ppszFileName, AM_MEDIA_TYPE* pmt)
{
    if (!ppszFileName) return E_POINTER;
    *ppszFileName = static_cast<LPOLESTR>(
        CoTaskMemAlloc((m_filename.size() + 1) * sizeof(WCHAR)));
    if (!*ppszFileName) return E_OUTOFMEMORY;
    wcscpy_s(*ppszFileName, m_filename.size() + 1, m_filename.c_str());
    if (pmt) ZeroMemory(pmt, sizeof(*pmt));
    return S_OK;
}

// ---------------------------------------------------------------------------
// Pin management
// ---------------------------------------------------------------------------
void CMmtTlvSplitter::CreatePins()
{
    for (auto* p : m_pins) delete p;
    m_pins.clear();

    HRESULT hr = S_OK;
    m_pins.push_back(new CMmtTlvOutputPin(true,  &hr, this, &m_pinLock, L"Video"));
    auto audioStreams = m_handler.getAudioStreams();
    if (audioStreams.empty()) {
        if (!m_audioUnsupported) {
            m_pins.push_back(new CMmtTlvOutputPin(false, &hr, this, &m_pinLock, L"Audio", -1));
            LogMsg(L"MMT/TLV Splitter: CreatePins created fallback Audio pin\n");
        } else {
            LogMsg(L"MMT/TLV Splitter: CreatePins skipped Audio pins because audio is unsupported\n");
        }
    } else {
        for (size_t i = 0; i < audioStreams.size(); ++i) {
            WCHAR pinName[64];
            StringCchPrintfW(pinName, ARRAYSIZE(pinName), L"Audio %zu", i + 1);
            auto* pin = new CMmtTlvOutputPin(false, &hr, this, &m_pinLock,
                                             pinName, audioStreams[i].streamIndex);
            if (audioStreams[i].samplingRate > 0)
                pin->SetAudioInfo(static_cast<int>(audioStreams[i].samplingRate), 2);
            m_pins.push_back(pin);
            LogMsg(L"MMT/TLV Splitter: CreatePins created %s for streamIndex=%d, packetId=0x%04X, componentTag=%d\n",
                   pinName,
                   audioStreams[i].streamIndex,
                   audioStreams[i].packetId,
                   audioStreams[i].componentTag);
        }
    }
    constexpr bool kEnableSubtitlePins = true;
    auto subtitleStreams = m_handler.getSubtitleStreams();
    if (kEnableSubtitlePins) {
        for (size_t i = 0; i < subtitleStreams.size(); ++i) {
            WCHAR pinName[64];
            StringCchPrintfW(pinName, ARRAYSIZE(pinName), L"Subtitle %zu", i + 1);
            auto* pin = new CMmtTlvOutputPin(MmtTlvPinKind::Subtitle, &hr, this, &m_pinLock,
                                             pinName, subtitleStreams[i].streamIndex);
            m_pins.push_back(pin);
            LogMsg(L"MMT/TLV Splitter: CreatePins created %s for streamIndex=%d, packetId=0x%04X, componentTag=%d\n",
                   pinName,
                   subtitleStreams[i].streamIndex,
                   subtitleStreams[i].packetId,
                   subtitleStreams[i].componentTag);
        }
    } else if (!subtitleStreams.empty()) {
        LogMsg(L"MMT/TLV Splitter: subtitle pins disabled for playback timing test, streams=%zu\n",
               subtitleStreams.size());
    }

    auto videoPin = m_pins[0];

    videoPin->SetVideoInfo(m_videoWidth, m_videoHeight);
    if (!m_hevcExtradata.empty())
        videoPin->SetHevcExtradata(m_hevcExtradata);

    m_handler.setVideoCallback(
        [videoPin, this](int, bool key, long long pts, long long dts,
                         bool first, bool last, const uint8_t* d, size_t sz) {
            REFERENCE_TIME normPts = (pts >= 0 && m_firstPts >= 0) ? pts - m_firstPts : pts;
            REFERENCE_TIME normDts = (dts >= 0 && m_firstPts >= 0) ? dts - m_firstPts : dts;
            if (last && normPts > 0)
                m_currentPts.store(normPts, std::memory_order_relaxed);
            REFERENCE_TIME samplePts = ToSegmentTime(normPts, m_segmentStart);
            REFERENCE_TIME sampleDts = ToSegmentTime(normDts, m_segmentStart);

            videoPin->DeliverSample(key, samplePts, sampleDts, first, last, d, sz);
        });

    m_handler.setAudioCallback(
        [this](int streamIndex, bool key, long long pts, long long dts,
                         bool first, bool last, const uint8_t* d, size_t sz) {
            REFERENCE_TIME normPts = (pts >= 0 && m_firstPts >= 0) ? pts - m_firstPts : pts;
            REFERENCE_TIME normDts = (dts >= 0 && m_firstPts >= 0) ? dts - m_firstPts : dts;
            REFERENCE_TIME samplePts = ToSegmentTime(normPts, m_segmentStart);
            REFERENCE_TIME sampleDts = ToSegmentTime(normDts, m_segmentStart);

            bool delivered = false;
            for (auto* pin : m_pins) {
                if (pin->IsAudio() &&
                    (pin->AudioStreamIndex() == -1 || pin->AudioStreamIndex() == streamIndex)) {
                    pin->DeliverSample(key, samplePts, sampleDts, first, last, d, sz);
                    delivered = true;
                }
            }
            if (!delivered && last) {
                LogMsg(L"AUDIO CALLBACK: no pin for streamIndex=%d, pts=%I64d ms\n",
                       streamIndex, normPts / 10000);
            }
        });

    m_handler.setSubtitleCallback(
        [this](int streamIndex, bool, long long pts, long long,
               bool, bool, const uint8_t* d, size_t sz) {
            static volatile LONG s_subtitleCallbacks = 0;
            LONG callbackNo = InterlockedIncrement(&s_subtitleCallbacks);

            TtmlDebugStats stats;
            TtmlTextCue cue = ExtractTtmlPlainText(d, sz, stats);

            REFERENCE_TIME normPts = (pts >= 0 && m_firstPts >= 0) ? pts - m_firstPts : pts;

            if (callbackNo <= 20 || cue.text.empty()) {
                std::wstring preview = Utf8Preview(cue.text);
                LogMsg(L"SUBTITLE CALLBACK #%ld: streamIndex=%d, pts=%I64d ms, ttmlBegin=%s%I64d ms, ttmlEnd=%s%I64d ms, size=%zu, divs=%zu, p=%zu, spans=%zu, textBytes=%zu, text=\"%s\"\n",
                       callbackNo,
                       streamIndex,
                       normPts / 10000,
                       cue.hasBegin ? L"" : L"none/",
                       cue.begin / 10000,
                       cue.hasEnd ? L"" : L"none/",
                       cue.end / 10000,
                       sz,
                       stats.divs,
                       stats.paragraphs,
                       stats.spans,
                       stats.textBytes,
                       preview.c_str());
            }

            if (cue.text.empty()) {
                size_t previewSize = sz < 16 ? sz : 16;
                WCHAR hex[96]{};
                WCHAR* cursor = hex;
                size_t remaining = ARRAYSIZE(hex);
                for (size_t i = 0; i < previewSize && remaining > 4; ++i) {
                    HRESULT hr = StringCchPrintfW(cursor, remaining, L"%02X ", d[i]);
                    if (FAILED(hr))
                        break;
                    size_t used = wcslen(cursor);
                    cursor += used;
                    remaining -= used;
                }
                LogMsg(L"SUBTITLE CALLBACK empty text #%ld: streamIndex=%d, firstBytes=%s\n",
                       callbackNo, streamIndex, hex);
                return;
            }

            std::string sampleText = cue.assText;

            REFERENCE_TIME sampleStart;
            REFERENCE_TIME sampleStop;
            if (cue.hasBegin) {
                REFERENCE_TIME firstSubtitle = m_firstSubtitleTime.load(std::memory_order_acquire);
                if (firstSubtitle < 0) {
                    REFERENCE_TIME expected = -1;
                    if (m_firstSubtitleTime.compare_exchange_strong(expected, cue.begin, std::memory_order_acq_rel)) {
                        firstSubtitle = cue.begin;
                    } else {
                        firstSubtitle = expected;
                    }
                    LogMsg(L"SUBTITLE timing base set: firstTtmlBegin=%I64d ms\n", firstSubtitle / 10000);
                }
                sampleStart = cue.begin - firstSubtitle;
                sampleStop = cue.hasEnd ? (cue.end - firstSubtitle) : (sampleStart + 5 * 10000000LL);
                sampleStart = ToSegmentTime(sampleStart, m_segmentStart);
                sampleStop = ToSegmentTime(sampleStop, m_segmentStart);
            } else {
                sampleStart = ToSegmentTime(normPts, m_segmentStart);
                sampleStop = sampleStart + 5 * 10000000LL;
            }
            if (sampleStop <= sampleStart)
                sampleStop = sampleStart + 5 * 10000000LL;

            bool delivered = false;
            for (auto* pin : m_pins) {
                if (pin->IsSubtitle() && pin->StreamIndex() == streamIndex) {
                    pin->DeliverTextSample(sampleStart, sampleStop, sampleText.c_str(), sampleText.size());
                    delivered = true;
                }
            }
            if (!delivered) {
                LogMsg(L"SUBTITLE CALLBACK: no pin for streamIndex=%d, pts=%I64d ms\n",
                       streamIndex, normPts / 10000);
            }
        });

    m_demuxer.setDemuxerHandler(m_handler);
}

bool CMmtTlvSplitter::IsWaitingForVideoRap() const
{
    return m_waitingForVideoRap.load(std::memory_order_acquire);
}

void CMmtTlvSplitter::NotifyVideoRap(REFERENCE_TIME segmentTime)
{
    bool expected = true;
    if (m_waitingForVideoRap.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        if (segmentTime < 0)
            segmentTime = 0;
        m_segmentTimeOffset.store(segmentTime, std::memory_order_release);
        LogMsg(L"MMT/TLV Splitter: segment RAP time offset=%I64d ms\n", segmentTime / 10000);
    }
}

REFERENCE_TIME CMmtTlvSplitter::GetSegmentTimeOffset() const
{
    return m_segmentTimeOffset.load(std::memory_order_acquire);
}

int CMmtTlvSplitter::GetPinCount()
{
    CAutoLock lock(&m_pinLock);
    return static_cast<int>(m_pins.size());
}

CBasePin* CMmtTlvSplitter::GetPin(int n)
{
    CAutoLock lock(&m_pinLock);
    if (n < 0 || n >= static_cast<int>(m_pins.size())) return nullptr;
    return m_pins[n];
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------
STDMETHODIMP CMmtTlvSplitter::Run(REFERENCE_TIME tStart)
{
    HRESULT hr = CBaseFilter::Run(tStart);
    if (SUCCEEDED(hr))
        StartThread();
    return hr;
}

STDMETHODIMP CMmtTlvSplitter::Pause()
{
    HRESULT hr = CBaseFilter::Pause();
    if (SUCCEEDED(hr) && m_State == State_Paused && !m_hThread)
        StartThread();
    return hr;
}

STDMETHODIMP CMmtTlvSplitter::Stop()
{
    StopThread();
    return CBaseFilter::Stop();
}

// ---------------------------------------------------------------------------
// Thread
// ---------------------------------------------------------------------------
void CMmtTlvSplitter::StartThread()
{
    if (m_hThread) return;
    ResetEvent(m_hStop);
    m_active = true;
    m_hThread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    LogMsg(L"MMT/TLV Splitter: StartThread target=%I64d ms, handle=%p\n",
           m_seekTarget / 10000, m_hThread);
}

void CMmtTlvSplitter::StopThread()
{
    if (!m_hThread) return;
    LogMsg(L"MMT/TLV Splitter: StopThread begin handle=%p\n", m_hThread);
    m_active = false;
    SetEvent(m_hStop);
    DWORD wait = WaitForSingleObject(m_hThread, 5000);
    LogMsg(L"MMT/TLV Splitter: StopThread wait=%lu\n", wait);
    CloseHandle(m_hThread);
    m_hThread = NULL;
    ResetEvent(m_hStop);
}

void CMmtTlvSplitter::SeekTo(REFERENCE_TIME pos)
{
    LogMsg(L"MMT/TLV Splitter: SeekTo begin pos=%I64d ms, state=%d, thread=%p\n",
           pos / 10000, m_State, m_hThread);

    // Flush downstream first so blocked Deliver() calls can return
    for (auto* pin : m_pins)
        if (pin->IsConnected()) pin->DeliverBeginFlush();

    m_isSeeking = true;
    StopThread();
    m_isSeeking = false;

    for (auto* pin : m_pins)
        if (pin->IsConnected()) pin->DeliverEndFlush();

    for (auto* pin : m_pins)
        pin->ResetForSeek();

    m_seekTarget = pos;
    m_currentPts = pos;   // normalised position (0-based)

    // Keep stream registration intact across seek so video starts faster.
    m_demuxer.resetStreams();
    m_handler.reset();

    if (m_State != State_Stopped)
        StartThread();

    LogMsg(L"MMT/TLV Splitter: SeekTo end pos=%I64d ms, state=%d, thread=%p\n",
           pos / 10000, m_State, m_hThread);
}

DWORD WINAPI CMmtTlvSplitter::ThreadProc(LPVOID pv)
{
    static_cast<CMmtTlvSplitter*>(pv)->DemuxLoop();
    return 0;
}

void CMmtTlvSplitter::DemuxLoop()
{
    constexpr size_t kChunk = 1024 * 1024;

    std::ifstream ifs(m_filename, std::ios::binary);
    if (!ifs.is_open()) {
        LogMsg(L"MMT/TLV Splitter: DemuxLoop failed to open file\n");
        return;
    }

    // Seek to requested position by byte-offset approximation
    REFERENCE_TIME seekTarget = m_seekTarget;
    m_segmentStart = seekTarget;
    m_segmentTimeOffset.store(0, std::memory_order_release);
    const bool waitForRap = seekTarget > 0;
    m_waitingForVideoRap.store(waitForRap, std::memory_order_release);
    for (auto* pin : m_pins) {
        if (pin->IsVideo())
            pin->SetWaitForVideoRap(waitForRap);
    }
    // Clear demuxer state for normal play; seek path already called resetStreams()
    if (seekTarget == 0) {
        m_demuxer.clear();
        m_handler.reset();
    }

    if (seekTarget > 0 && m_fileSize > 0) {
        double ratio = (m_duration > 0)
            ? static_cast<double>(seekTarget) / m_duration
            : 0.0;
        if (ratio > 1.0) ratio = 1.0;
        auto byteOffset = static_cast<std::streamoff>(ratio * m_fileSize);
        ifs.seekg(byteOffset);
        LogMsg(L"MMT/TLV Splitter: DemuxLoop seek target=%I64d ms ratio=%.6f byte=%I64d/%I64d ok=%d\n",
               seekTarget / 10000,
               ratio,
               static_cast<long long>(byteOffset),
               static_cast<long long>(m_fileSize),
               ifs.good() ? 1 : 0);
    } else {
        LogMsg(L"MMT/TLV Splitter: DemuxLoop start target=%I64d ms byte=0\n",
               seekTarget / 10000);
    }

    // tStart = seekTarget (relative, 0 = start of content).
    // Sample timestamps are also normalised to 0-based (see CreatePins callbacks),
    // so the segment time and sample times share the same [0, m_duration] range.
    for (auto* pin : m_pins)
        pin->DeliverNewSegment(seekTarget, _I64_MAX, m_rate);

    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);

    while (m_active) {
        if (WaitForSingleObject(m_hStop, 0) == WAIT_OBJECT_0) break;

        // Keep buffer supplied with data so demuxer never deadlocks on NotEnoughBuffer
        if (buf.size() < 2 * 1024 * 1024) {
            size_t oldSz = buf.size();
            buf.resize(oldSz + kChunk);
            ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
            std::streamsize got = ifs.gcount();
            buf.resize(oldSz + static_cast<size_t>(got));
            if (got == 0 && buf.empty()) break;
        }

        MmtTlv::Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            if (!m_active) break;
            auto status = m_demuxer.demux(stream);
            if (status == MmtTlv::DemuxStatus::NotEnoughBuffer) break;
        }

        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) {
            buf.erase(buf.begin(), buf.begin() + consumed);
        } else {
            // Avoid infinite loop at EOF when remaining buffer is unparseable
            if (ifs.eof() || !ifs.good()) break;
        }
    }

    if (!m_isSeeking) {
        for (auto* pin : m_pins)
            pin->DeliverEOS();
    }

    LogMsg(L"MMT/TLV Splitter: DemuxLoop exit target=%I64d ms, seeking=%d, active=%d\n",
           seekTarget / 10000, m_isSeeking.load() ? 1 : 0, m_active.load() ? 1 : 0);
}

// ---------------------------------------------------------------------------
// IMediaSeeking
// ---------------------------------------------------------------------------
STDMETHODIMP CMmtTlvSplitter::GetCapabilities(DWORD* pCaps)
{
    if (!pCaps) return E_POINTER;
    *pCaps = AM_SEEKING_CanSeekAbsolute
           | AM_SEEKING_CanSeekForwards
           | AM_SEEKING_CanSeekBackwards
           | AM_SEEKING_CanGetDuration
           | AM_SEEKING_CanGetCurrentPos
           | AM_SEEKING_CanGetStopPos;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::CheckCapabilities(DWORD* pCaps)
{
    if (!pCaps) return E_POINTER;
    DWORD caps;
    GetCapabilities(&caps);
    DWORD have = *pCaps & caps;
    if (have == 0)       return E_FAIL;
    if (have != *pCaps)  return S_FALSE;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::IsFormatSupported(const GUID* pFormat)
{
    if (!pFormat) return E_POINTER;
    return (*pFormat == TIME_FORMAT_MEDIA_TIME) ? S_OK : S_FALSE;
}

STDMETHODIMP CMmtTlvSplitter::QueryPreferredFormat(GUID* pFormat)
{
    if (!pFormat) return E_POINTER;
    *pFormat = TIME_FORMAT_MEDIA_TIME;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::GetTimeFormat(GUID* pFormat)
{
    if (!pFormat) return E_POINTER;
    *pFormat = TIME_FORMAT_MEDIA_TIME;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::IsUsingTimeFormat(const GUID* pFormat)
{
    if (!pFormat) return E_POINTER;
    return (*pFormat == TIME_FORMAT_MEDIA_TIME) ? S_OK : S_FALSE;
}

STDMETHODIMP CMmtTlvSplitter::SetTimeFormat(const GUID* pFormat)
{
    if (!pFormat) return E_POINTER;
    return (*pFormat == TIME_FORMAT_MEDIA_TIME) ? S_OK : E_INVALIDARG;
}

STDMETHODIMP CMmtTlvSplitter::GetDuration(LONGLONG* pDuration)
{
    if (!pDuration) return E_POINTER;
    *pDuration = m_duration;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::GetStopPosition(LONGLONG* pStop)
{
    if (!pStop) return E_POINTER;
    *pStop = (m_stopPos == _I64_MAX) ? m_duration : m_stopPos;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::GetCurrentPosition(LONGLONG* pCurrent)
{
    if (!pCurrent) return E_POINTER;
    // m_currentPts is already normalised (0-based) by CreatePins callback.
    *pCurrent = m_currentPts.load(std::memory_order_relaxed);
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::ConvertTimeFormat(LONGLONG* pTarget,
    const GUID* pTargetFormat, LONGLONG Source, const GUID* pSourceFormat)
{
    if (!pTarget) return E_POINTER;
    const GUID& src = pSourceFormat ? *pSourceFormat : TIME_FORMAT_MEDIA_TIME;
    const GUID& dst = pTargetFormat ? *pTargetFormat : TIME_FORMAT_MEDIA_TIME;
    if (src != TIME_FORMAT_MEDIA_TIME || dst != TIME_FORMAT_MEDIA_TIME)
        return E_INVALIDARG;
    *pTarget = Source;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::SetPositions(
    LONGLONG* pCurrent, DWORD dwCurrentFlags,
    LONGLONG* pStop,    DWORD dwStopFlags)
{
    REFERENCE_TIME curBefore = m_currentPts.load(std::memory_order_relaxed);
    LogMsg(L"MMT/TLV Splitter: SetPositions in current=%s %I64d ms flags=0x%08X(%s), stop=%s %I64d ms flags=0x%08X(%s), curBefore=%I64d ms, duration=%I64d ms, thread=%p\n",
           pCurrent ? L"yes" : L"no",
           pCurrent ? (*pCurrent / 10000) : 0,
           dwCurrentFlags,
           PositioningName(dwCurrentFlags),
           pStop ? L"yes" : L"no",
           pStop ? (*pStop / 10000) : 0,
           dwStopFlags,
           PositioningName(dwStopFlags),
           curBefore / 10000,
           m_duration / 10000,
           m_hThread);

    if (pStop && (dwStopFlags & AM_SEEKING_PositioningBitsMask) != AM_SEEKING_NoPositioning) {
        REFERENCE_TIME s = *pStop;
        switch (dwStopFlags & AM_SEEKING_PositioningBitsMask) {
        case AM_SEEKING_RelativePositioning:
            s = ((m_stopPos == _I64_MAX) ? curBefore : m_stopPos) + *pStop;
            break;
        case AM_SEEKING_IncrementalPositioning:
            s = curBefore + *pStop;
            break;
        default:
            break;
        }
        if (s < 0) s = 0;
        if (m_duration > 0 && s > m_duration) s = m_duration;
        if (s > 0) m_stopPos = s;  // ignore stop=0 (MPC-BE sets this when duration is unknown)
        *pStop = (m_stopPos == _I64_MAX) ? m_duration : m_stopPos;
    }

    if (pCurrent && (dwCurrentFlags & AM_SEEKING_PositioningBitsMask) != AM_SEEKING_NoPositioning) {
        REFERENCE_TIME pos = *pCurrent;
        switch (dwCurrentFlags & AM_SEEKING_PositioningBitsMask) {
        case AM_SEEKING_RelativePositioning:
        case AM_SEEKING_IncrementalPositioning:
            pos = curBefore + *pCurrent;
            break;
        default:
            break;
        }
        if (pos < 0) pos = 0;
        if (m_duration > 0 && pos > m_duration) pos = m_duration;

        LogMsg(L"MMT/TLV Splitter: SetPositions resolved current=%I64d ms\n", pos / 10000);
        const bool sameSeekTarget = llabs(pos - m_seekTarget) < 10000;
        const bool alreadyReportedAtTarget = llabs(pos - curBefore) < 10000;
        if (m_hThread && sameSeekTarget && alreadyReportedAtTarget) {
            LogMsg(L"MMT/TLV Splitter: SetPositions duplicate target ignored current=%I64d ms\n",
                   pos / 10000);
            *pCurrent = pos;
            LogMsg(L"MMT/TLV Splitter: SetPositions out current=%I64d ms, stop=%I64d ms, target=%I64d ms\n",
                   m_currentPts.load(std::memory_order_relaxed) / 10000,
                   ((m_stopPos == _I64_MAX) ? m_duration : m_stopPos) / 10000,
                   m_seekTarget / 10000);
            return S_OK;
        }
        if (m_hThread) {
            SeekTo(pos);
        } else {
            m_seekTarget = pos;
            m_currentPts = pos;
        }
        *pCurrent = pos;
    }

    LogMsg(L"MMT/TLV Splitter: SetPositions out current=%I64d ms, stop=%I64d ms, target=%I64d ms\n",
           m_currentPts.load(std::memory_order_relaxed) / 10000,
           ((m_stopPos == _I64_MAX) ? m_duration : m_stopPos) / 10000,
           m_seekTarget / 10000);
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::GetPositions(LONGLONG* pCurrent, LONGLONG* pStop)
{
    if (pCurrent) GetCurrentPosition(pCurrent);
    if (pStop)    GetStopPosition(pStop);
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::GetAvailable(LONGLONG* pEarliest, LONGLONG* pLatest)
{
    if (pEarliest) *pEarliest = 0;
    if (pLatest)   *pLatest   = m_duration;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::SetRate(double dRate)
{
    if (dRate <= 0.0) return E_INVALIDARG;
    m_rate = dRate;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::GetRate(double* pdRate)
{
    if (!pdRate) return E_POINTER;
    *pdRate = m_rate;
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::GetPreroll(LONGLONG* pllPreroll)
{
    if (!pllPreroll) return E_POINTER;
    *pllPreroll = 0;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAMStreamSelect
// ---------------------------------------------------------------------------
STDMETHODIMP CMmtTlvSplitter::Count(DWORD* pcStreams)
{
    if (!pcStreams) return E_POINTER;
    auto streams = m_handler.getAudioStreams();
    *pcStreams = static_cast<DWORD>(streams.size());
    LogMsg(L"MMT/TLV StreamSelect: Count=%lu, selectedStreamIndex=%d\n",
           *pcStreams, m_handler.getSelectedAudioStreamIndex());
    for (size_t i = 0; i < streams.size(); ++i) {
        const auto& info = streams[i];
        LogMsg(L"MMT/TLV StreamSelect: Count stream[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, samplingRate=%u\n",
               i, info.streamIndex, info.packetId, info.componentTag, info.samplingRate);
    }
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::Info(long lIndex, AM_MEDIA_TYPE** ppmt, DWORD* pdwFlags,
    LCID* plcid, DWORD* pdwGroup, LPWSTR* ppszName, IUnknown** ppObject, IUnknown** ppUnk)
{
    auto streams = m_handler.getAudioStreams();
    if (lIndex < 0 || static_cast<size_t>(lIndex) >= streams.size())
        return E_INVALIDARG;

    const auto& info = streams[static_cast<size_t>(lIndex)];
    if (ppmt) *ppmt = nullptr;
    if (pdwFlags) {
        *pdwFlags = AMSTREAMSELECTINFO_EXCLUSIVE;
        if (info.streamIndex == m_handler.getSelectedAudioStreamIndex())
            *pdwFlags |= AMSTREAMSELECTINFO_ENABLED;
    }
    if (plcid) *plcid = 0;
    if (pdwGroup) *pdwGroup = 1;
    if (ppszName) {
        *ppszName = AllocStreamName(L"Audio %d (stream %d, component %d)",
                                    lIndex + 1, info);
        if (!*ppszName)
            return E_OUTOFMEMORY;
    }
    if (ppObject) *ppObject = nullptr;
    if (ppUnk) *ppUnk = nullptr;
    LogMsg(L"MMT/TLV StreamSelect: Info index=%ld, streamIndex=%d, flags=0x%08X, group=1\n",
           lIndex,
           info.streamIndex,
           pdwFlags ? *pdwFlags : 0);
    return S_OK;
}

STDMETHODIMP CMmtTlvSplitter::Enable(long lIndex, DWORD dwFlags)
{
    if ((dwFlags & AMSTREAMSELECTENABLE_ENABLE) == 0)
        return S_OK;

    auto streams = m_handler.getAudioStreams();
    if (lIndex < 0 || static_cast<size_t>(lIndex) >= streams.size())
        return E_INVALIDARG;

    for (auto* pin : m_pins)
        if (pin->IsAudio() && pin->IsConnected())
            pin->DeliverBeginFlush();

    bool ok = m_handler.selectAudioStreamByListIndex(static_cast<size_t>(lIndex));

    for (auto* pin : m_pins) {
        if (pin->IsAudio()) {
            pin->ResetForSeek();
            if (pin->IsConnected())
                pin->DeliverEndFlush();
        }
    }

    LogMsg(L"MMT/TLV StreamSelect: Enable index=%ld flags=0x%08X ok=%d\n",
           lIndex, dwFlags, ok ? 1 : 0);
    return ok ? S_OK : E_INVALIDARG;
}
