#include "MmtTlvSplitter.h"
#include "DebugLog.h"
#include "Guids.h"
#include "stream.h"     // MmtTlv::Common::ReadStream
#include "ttml.h"
#include "pugixml.hpp"
#include <fstream>
#include <strmif.h>
#include <cwchar>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

static const WCHAR kFilterName[] = L"MMT/TLV Splitter";
static constexpr REFERENCE_TIME kDefaultSubtitleDuration = 25 * 1000000LL; // 2.5 sec fallback
static constexpr REFERENCE_TIME kSubtitleChunkDuration = kDefaultSubtitleDuration;
static constexpr REFERENCE_TIME kSubtitleInitialDelay = 300 * 10000LL; // 300 ms
static constexpr double kAssLineHeightRatio = 1.18;
static constexpr double kAssSubtitleMargin = 20.0;
static constexpr uint8_t kDefaultBackgroundRgb = 0x30;

#define LogMsg MmtTlvLogInfo
#define LogDetail MmtTlvLogDebug

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
    std::vector<std::string> assEvents;
    bool hasBegin = false;
    bool hasEnd = false;
    bool missingGlyph = false;
    REFERENCE_TIME begin = 0;
    REFERENCE_TIME end = 0;
};

struct SubtitleGlyphResource {
    int unitsPerEm = 360;
    int ascent = 360;
    int descent = 0;
    std::string pathData;
};

static std::mutex g_subtitleGlyphMutex;
static std::map<std::pair<int, uint32_t>, SubtitleGlyphResource> g_subtitleGlyphs;
static std::mutex g_subtitleGlyphLoadMutex;
static std::vector<std::wstring> g_loadedSubtitleGlyphFiles;

struct MmtsCaptionSettings {
    int captionAlpha = 0;       // ASS alpha: 0=opaque, 255=fully transparent
    int backgroundAlpha = -1;   // -1=use TTML data, 0-255=fixed override
    bool showBackground = true;
    int outlineWidth = 0;
    int delayMs = 0;
    bool dumpSubtitleData = false;
    int dumpSubtitleMaxFiles = 200;
    std::string fontName = "MS Gothic";
    std::wstring dumpSubtitleDir;
};

static void GetMmtsIniPath(WCHAR* iniPath, DWORD size)
{
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetMmtsIniPath), &hMod);
    if (!GetModuleFileNameW(hMod, iniPath, size)) {
        iniPath[0] = L'\0';
        return;
    }

    WCHAR* dot = wcsrchr(iniPath, L'.');
    if (dot)
        wcscpy_s(dot, size - static_cast<DWORD>(dot - iniPath), L".ini");
}

static bool ReadIniValue(const WCHAR* iniPath, const WCHAR* section, const WCHAR* key,
                         WCHAR* buf, DWORD size)
{
    return GetPrivateProfileStringW(section, key, L"", buf, size, iniPath) > 0;
}

static MmtsCaptionSettings GetMmtsCaptionSettings()
{
    static MmtsCaptionSettings cached;
    static DWORD lastLoad = 0;
    static WCHAR lastPath[MAX_PATH] = {};

    WCHAR iniPath[MAX_PATH] = {};
    GetMmtsIniPath(iniPath, MAX_PATH);

    DWORD now = GetTickCount();
    if (lastLoad != 0 && now - lastLoad <= 5000 && wcscmp(lastPath, iniPath) == 0)
        return cached;

    MmtsCaptionSettings s;
    WCHAR buf[MAX_PATH] = {};

    if (ReadIniValue(iniPath, L"MMTS", L"CaptionTransparency", buf, ARRAYSIZE(buf))) {
        int t = (std::max)(0, (std::min)(100, _wtoi(buf)));
        s.captionAlpha = t * 255 / 100;
    }
    if (ReadIniValue(iniPath, L"MMTS", L"BackgroundTransparency", buf, ARRAYSIZE(buf))) {
        int t = (std::max)(0, (std::min)(100, _wtoi(buf)));
        s.backgroundAlpha = t * 255 / 100;
    }
    if (ReadIniValue(iniPath, L"MMTS", L"ShowBackground", buf, ARRAYSIZE(buf)))
        s.showBackground = _wtoi(buf) != 0;
    if (ReadIniValue(iniPath, L"MMTS", L"OutlineWidth", buf, ARRAYSIZE(buf)))
        s.outlineWidth = (std::max)(0, (std::min)(10, _wtoi(buf)));
    if (ReadIniValue(iniPath, L"MMTS", L"DelayMs", buf, ARRAYSIZE(buf)))
        s.delayMs = (std::max)(-30000, (std::min)(30000, _wtoi(buf)));
    if (ReadIniValue(iniPath, L"MMTS", L"DumpSubtitleData", buf, ARRAYSIZE(buf)))
        s.dumpSubtitleData = _wtoi(buf) != 0;
    if (ReadIniValue(iniPath, L"MMTS", L"DumpSubtitleMaxFiles", buf, ARRAYSIZE(buf)))
        s.dumpSubtitleMaxFiles = (std::max)(1, (std::min)(10000, _wtoi(buf)));
    if (ReadIniValue(iniPath, L"MMTS", L"DumpSubtitleDir", buf, ARRAYSIZE(buf)))
        s.dumpSubtitleDir = buf;
    if (ReadIniValue(iniPath, L"MMTS", L"FontName", buf, ARRAYSIZE(buf))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            s.fontName.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s.fontName[0], len, nullptr, nullptr);
        }
    }

    cached = s;
    lastLoad = now;
    wcscpy_s(lastPath, iniPath);
    return cached;
}

static bool EnsureDirectoryTree(const std::wstring& path)
{
    if (path.empty())
        return false;

    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        std::wstring parent = path.substr(0, slash);
        if (!parent.empty() && !EnsureDirectoryTree(parent))
            return false;
    }

    if (CreateDirectoryW(path.c_str(), nullptr))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool WriteFileBytes(const std::wstring& path, const void* data, size_t size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    bool ok = true;
    while (remaining > 0) {
        DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(DWORD_MAX)));
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        p += written;
        remaining -= written;
    }
    CloseHandle(h);
    return ok;
}

static bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }

    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = data.empty() ||
              ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(h);
    return ok && read == data.size();
}

static std::wstring DefaultSubtitleDumpDir()
{
    WCHAR iniPath[MAX_PATH] = {};
    GetMmtsIniPath(iniPath, MAX_PATH);
    WCHAR* slash = wcsrchr(iniPath, L'\\');
    if (slash)
        *(slash + 1) = L'\0';
    else
        iniPath[0] = L'\0';

    std::wstring dir = iniPath;
    dir += L"subtitle_dump";
    return dir;
}

static void DumpSubtitleDataIfEnabled(int streamIndex, LONG callbackNo, REFERENCE_TIME normPts,
                                      const uint8_t* data, size_t size,
                                      const TtmlTextCue& cue, const TtmlDebugStats& stats)
{
    const MmtsCaptionSettings settings = GetMmtsCaptionSettings();
    if (!settings.dumpSubtitleData || !data || size == 0)
        return;

    static volatile LONG s_dumpedSamples = 0;
    LONG dumpNo = InterlockedIncrement(&s_dumpedSamples);
    if (dumpNo > settings.dumpSubtitleMaxFiles)
        return;

    std::wstring dir = settings.dumpSubtitleDir.empty() ? DefaultSubtitleDumpDir() : settings.dumpSubtitleDir;
    if (!EnsureDirectoryTree(dir)) {
        LogMsg(L"MMT/TLV Subtitle dump: cannot create dir \"%s\"\n", dir.c_str());
        return;
    }

    WCHAR baseName[192] = {};
    StringCchPrintfW(baseName, ARRAYSIZE(baseName),
                    L"subtitle_s%d_cb%06ld_pts%I64d", streamIndex, callbackNo, normPts / 10000);

    std::wstring ttmlPath = dir + L"\\" + baseName + L".ttml";
    if (!WriteFileBytes(ttmlPath, data, size)) {
        LogMsg(L"MMT/TLV Subtitle dump: failed to write \"%s\"\n", ttmlPath.c_str());
        return;
    }

    char meta[512] = {};
    std::snprintf(meta, sizeof(meta),
                  "streamIndex=%d\ncallback=%ld\nptsMs=%lld\nsize=%zu\n"
                  "ttmlBeginMs=%lld\nhasBegin=%d\nttmlEndMs=%lld\nhasEnd=%d\n"
                  "divs=%zu\nparagraphs=%zu\nspans=%zu\ntextBytes=%zu\n",
                  streamIndex, callbackNo, static_cast<long long>(normPts / 10000), size,
                  static_cast<long long>(cue.begin / 10000), cue.hasBegin ? 1 : 0,
                  static_cast<long long>(cue.end / 10000), cue.hasEnd ? 1 : 0,
                  stats.divs, stats.paragraphs, stats.spans, stats.textBytes);
    std::wstring metaPath = dir + L"\\" + baseName + L".txt";
    WriteFileBytes(metaPath, meta, strlen(meta));

    if (dumpNo <= 5 || (dumpNo % 50) == 0) {
        LogMsg(L"MMT/TLV Subtitle dump #%ld: \"%s\"\n", dumpNo, ttmlPath.c_str());
    }
}

static const WCHAR* GuessSubtitleResourceExtension(const uint8_t* data, size_t size)
{
    if (!data || size == 0)
        return L".bin";

    const size_t probe = (std::min)(size, static_cast<size_t>(64));
    size_t i = 0;
    while (i < probe && (data[i] == 0xEF || data[i] == 0xBB || data[i] == 0xBF ||
                         data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) {
        ++i;
    }

    if (i < probe && data[i] == '<') {
        if (i + 4 < size && std::memcmp(data + i, "<svg", 4) == 0)
            return L".svg";
        if (i + 5 < size && std::memcmp(data + i, "<?xml", 5) == 0)
            return L".svg";
    }
    if (size >= 2 && data[0] == 0x1F && data[1] == 0x8B)
        return L".gz";
    return L".bin";
}

static void DumpSubtitleResourceIfEnabled(int streamIndex, LONG callbackNo, REFERENCE_TIME normPts,
                                          int dataType, int subsampleNumber, int lastSubsampleNumber,
                                          const uint8_t* data, size_t size)
{
    const MmtsCaptionSettings settings = GetMmtsCaptionSettings();
    if (!settings.dumpSubtitleData || !data || size == 0)
        return;

    static volatile LONG s_dumpedResources = 0;
    LONG dumpNo = InterlockedIncrement(&s_dumpedResources);
    if (dumpNo > settings.dumpSubtitleMaxFiles)
        return;

    std::wstring dir = settings.dumpSubtitleDir.empty() ? DefaultSubtitleDumpDir() : settings.dumpSubtitleDir;
    if (!EnsureDirectoryTree(dir)) {
        LogMsg(L"MMT/TLV Subtitle resource dump: cannot create dir \"%s\"\n", dir.c_str());
        return;
    }

    WCHAR baseName[224] = {};
    StringCchPrintfW(baseName, ARRAYSIZE(baseName),
                    L"subtitle_resource_s%d_cb%06ld_pts%I64d_type%d_sub%d_of%d",
                    streamIndex, callbackNo, normPts / 10000,
                    dataType, subsampleNumber, lastSubsampleNumber);

    std::wstring dataPath = dir + L"\\" + baseName + GuessSubtitleResourceExtension(data, size);
    if (!WriteFileBytes(dataPath, data, size)) {
        LogMsg(L"MMT/TLV Subtitle resource dump: failed to write \"%s\"\n", dataPath.c_str());
        return;
    }

    char meta[384] = {};
    std::snprintf(meta, sizeof(meta),
                  "streamIndex=%d\ncallback=%ld\nptsMs=%lld\nsize=%zu\n"
                  "dataType=%d\nsubsampleNumber=%d\nlastSubsampleNumber=%d\n",
                  streamIndex, callbackNo, static_cast<long long>(normPts / 10000), size,
                  dataType, subsampleNumber, lastSubsampleNumber);
    std::wstring metaPath = dir + L"\\" + baseName + L".txt";
    WriteFileBytes(metaPath, meta, strlen(meta));

    if (dumpNo <= 5 || (dumpNo % 50) == 0) {
        LogMsg(L"MMT/TLV Subtitle resource dump #%ld: \"%s\"\n", dumpNo, dataPath.c_str());
    }
}

static bool ParseSvgUnicodeValue(const char* value, uint32_t& codepoint)
{
    if (!value || !*value)
        return false;

    std::string text(value);
    const std::string hexEntity = "&#x";
    size_t pos = text.find(hexEntity);
    if (pos != std::string::npos) {
        pos += hexEntity.size();
        size_t end = text.find(';', pos);
        if (end == std::string::npos)
            end = text.size();
        codepoint = static_cast<uint32_t>(std::strtoul(text.substr(pos, end - pos).c_str(), nullptr, 16));
        return codepoint != 0;
    }

    if (text.rfind("U+", 0) == 0 || text.rfind("u+", 0) == 0) {
        codepoint = static_cast<uint32_t>(std::strtoul(text.c_str() + 2, nullptr, 16));
        return codepoint != 0;
    }

    int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
        return false;
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
    codepoint = static_cast<uint32_t>(wide[0]);
    return codepoint != 0;
}

static bool ParseUnicodeRangeValue(const char* value, uint32_t& codepoint)
{
    if (!value || !*value)
        return false;
    std::string text(value);
    size_t pos = text.find("U+");
    if (pos == std::string::npos)
        pos = text.find("u+");
    if (pos == std::string::npos)
        return false;
    pos += 2;
    size_t end = pos;
    while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])))
        ++end;
    if (end == pos)
        return false;
    codepoint = static_cast<uint32_t>(std::strtoul(text.substr(pos, end - pos).c_str(), nullptr, 16));
    return codepoint != 0;
}

static void RegisterSubtitleGlyphResource(int streamIndex, const uint8_t* data, size_t size)
{
    if (!data || size == 0)
        return;

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(data, size);
    if (result.status != pugi::status_ok) {
        LogDetail(L"MMT/TLV Subtitle glyph resource parse failed: streamIndex=%d status=%d\n",
                  streamIndex, static_cast<int>(result.status));
        return;
    }

    pugi::xml_node svg = doc.child("svg");
    if (!svg) {
        LogDetail(L"MMT/TLV Subtitle glyph resource ignored: streamIndex=%d no svg root\n", streamIndex);
        return;
    }

    int registered = 0;
    for (pugi::xml_node defs : svg.children("defs")) {
        for (pugi::xml_node font : defs.children("font")) {
            SubtitleGlyphResource base;
            if (font.attribute("horiz-adv-x"))
                base.unitsPerEm = font.attribute("horiz-adv-x").as_int(base.unitsPerEm);

            pugi::xml_node face = font.child("font-face");
            if (face) {
                base.unitsPerEm = face.attribute("units-per-em").as_int(base.unitsPerEm);
                base.ascent = face.attribute("ascent").as_int(base.ascent);
                base.descent = face.attribute("descent").as_int(base.descent);
            }

            uint32_t faceCodepoint = 0;
            ParseUnicodeRangeValue(face.attribute("unicode-range").value(), faceCodepoint);

            for (pugi::xml_node glyph : font.children("glyph")) {
                uint32_t codepoint = 0;
                if (!ParseSvgUnicodeValue(glyph.attribute("unicode").value(), codepoint))
                    codepoint = faceCodepoint;
                const char* path = glyph.attribute("d").value();
                if (codepoint == 0 || !path || !*path)
                    continue;

                SubtitleGlyphResource resource = base;
                resource.pathData = path;
                std::lock_guard<std::mutex> lock(g_subtitleGlyphMutex);
                g_subtitleGlyphs[{streamIndex, codepoint}] = std::move(resource);
                ++registered;
                LogDetail(L"MMT/TLV Subtitle glyph registered: streamIndex=%d U+%04X units=%d ascent=%d descent=%d\n",
                          streamIndex, codepoint, resource.unitsPerEm, resource.ascent, resource.descent);
            }
        }
    }

    if (registered == 0) {
        LogDetail(L"MMT/TLV Subtitle glyph resource parsed but no glyph registered: streamIndex=%d\n", streamIndex);
    }
}

static void TryLoadDumpedSubtitleGlyphResources(int streamIndex)
{
    const MmtsCaptionSettings settings = GetMmtsCaptionSettings();
    if (!settings.dumpSubtitleData)
        return;

    std::lock_guard<std::mutex> loadLock(g_subtitleGlyphLoadMutex);
    std::wstring dir = settings.dumpSubtitleDir.empty() ? DefaultSubtitleDumpDir() : settings.dumpSubtitleDir;
    std::wstring pattern = dir + L"\\subtitle_resource_s*_*.svg";

    WIN32_FIND_DATAW findData = {};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        std::wstring path = dir + L"\\" + findData.cFileName;
        if (std::find(g_loadedSubtitleGlyphFiles.begin(), g_loadedSubtitleGlyphFiles.end(), path) !=
            g_loadedSubtitleGlyphFiles.end()) {
            continue;
        }

        std::vector<uint8_t> data;
        if (ReadFileBytes(path, data)) {
            RegisterSubtitleGlyphResource(streamIndex, data.data(), data.size());
            g_loadedSubtitleGlyphFiles.push_back(path);
            LogDetail(L"MMT/TLV Subtitle glyph loaded from dump: streamIndex=%d file=\"%s\"\n",
                      streamIndex, path.c_str());
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
}

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

static int BaseAssFontSizeFromSpan(const TTMLSpanTag* span)
{
    if (!span)
        return 64;

    float fontWidth = 0;
    float fontHeight = 0;
    if (TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight) && fontHeight > 0)
        return (std::max)(1, static_cast<int>(std::lround(fontHeight * 1080.0 / 2160.0)));

    return 64;
}

static int ParagraphMaxBaseFontSize(const TTMLPTag& p)
{
    int maxSize = 0;
    for (const auto& span : p.spanTags) {
        if (!span.text.empty())
            maxSize = (std::max)(maxSize, BaseAssFontSizeFromSpan(&span));
    }
    return maxSize;
}

static int DivMaxBaseFontSize(const TTMLDivTag& div)
{
    int maxSize = 0;
    for (const auto& p : div.pTags)
        maxSize = (std::max)(maxSize, ParagraphMaxBaseFontSize(p));
    return maxSize;
}

static double RubyLikeYOffsetAss(const TTMLPTag& p, int divMaxFontSize)
{
    const int pFontSize = ParagraphMaxBaseFontSize(p);
    if (divMaxFontSize <= 0 || pFontSize <= 0 || pFontSize * 10 > divMaxFontSize * 6)
        return 0;

    return divMaxFontSize - pFontSize;
}

static bool IsRubyLikeParagraph(const TTMLPTag& p, int divMaxFontSize)
{
    return RubyLikeYOffsetAss(p, divMaxFontSize) > 0;
}

static bool HasRubyLikeParagraph(const TTMLDivTag& div, int divMaxFontSize)
{
    for (const auto& p : div.pTags) {
        if (IsRubyLikeParagraph(p, divMaxFontSize))
            return true;
    }
    return false;
}

static double B24FontHeightFromSpan(const TTMLSpanTag* span)
{
    if (!span)
        return 0;

    float fontWidth = 0;
    float fontHeight = 0;
    if (!TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight))
        return 0;

    if (fontWidth == 144 && fontHeight == 144)
        return 240;
    if (fontWidth == 72 && fontHeight == 144)
        return 240;
    if (fontWidth == 72 && fontHeight == 72)
        return 120;
    return fontHeight;
}

static double B24LineOffsetYFromParagraph(const TTMLPTag& p)
{
    if (p.spanTags.empty() || !p.region.extent.has_value())
        return 0;

    const TTMLSpanTag* firstSpan = &(*p.spanTags.begin());
    if (!firstSpan->style.lineHeight.has_value() || !firstSpan->style.fontSize.has_value())
        return 0;

    try {
        const double lineHeight = firstSpan->style.lineHeight->getValue<TTMLCssValueLength>().value;
        const double fontHeight = B24FontHeightFromSpan(firstSpan);
        return (lineHeight - fontHeight) / 2.0;
    } catch (const std::bad_variant_access&) {
        return 0;
    }
}

static int CountAssTextLines(const std::string& text)
{
    if (text.empty())
        return 1;

    int lines = 1;
    for (char c : text) {
        if (c == '\n')
            ++lines;
    }
    return lines;
}

static int CountAssParagraphLines(const TTMLPTag& p)
{
    int lines = 1;
    for (const auto& span : p.spanTags)
        lines = (std::max)(lines, CountAssTextLines(span.text));
    return lines;
}

static double FitAssFontScale(int baseFontSize, int lineCount, double anchorYAss, bool hasExtent, float extentY)
{
    if (baseFontSize <= 0)
        return 1.0;

    double scale = 1.0;
    const double lines = (std::max)(1, lineCount);

    if (hasExtent && extentY > 0) {
        const double regionHeight = extentY * 1080.0 / 2160.0;
        const double maxFont = regionHeight / (lines * kAssLineHeightRatio);
        if (maxFont > 0)
            scale = (std::min)(scale, maxFont / baseFontSize);
    }

    const double availableDown = (std::max)(0.0, 1080.0 - kAssSubtitleMargin - anchorYAss);
    const double availableUp = (std::max)(0.0, anchorYAss - kAssSubtitleMargin);
    const double verticalBudget = hasExtent ? (std::min)(availableDown, availableUp) * 2.0 : availableDown;
    const double maxFrameFont = verticalBudget / (lines * kAssLineHeightRatio);
    if (maxFrameFont > 0)
        scale = (std::min)(scale, maxFrameFont / baseFontSize);

    if (scale + 0.01 < 1.0) {
        LogDetail(L"MMT/TLV Subtitle ASS font fit: base=%d lines=%d scale=%.3f extent=%s anchorY=%.1f\r\n",
               baseFontSize, lineCount, scale, hasExtent ? L"yes" : L"no", anchorYAss);
    }

    return scale;
}

static int AssFontSizeFromSpan(const TTMLSpanTag* span, double fontScale)
{
    return (std::max)(1, static_cast<int>(std::lround(BaseAssFontSizeFromSpan(span) * fontScale)));
}

// Returns true when the span text consists solely of full-width bracket
// characters that some broadcasters incorrectly transmit as MSZ (72x144).
// These must be rendered at full width (scaleX 100%) to avoid overlapping
// the adjacent glyph.
static bool SpanIsMistaggedFullwidthBracket(const TTMLSpanTag* span)
{
    if (!span || span->text.empty())
        return false;

    // UTF-8 encodings of the brackets to check
    static const char* kBrackets[] = {
        "\xe3\x80\x8e",  // U+300E 『
        "\xe3\x80\x8f",  // U+300F 』
        "\xe3\x80\x8c",  // U+300C 「
        "\xe3\x80\x8d",  // U+300D 」
        "\xe3\x80\x90",  // U+3010 【
        "\xe3\x80\x91",  // U+3011 】
    };

    const std::string& t = span->text;
    size_t pos = 0;
    while (pos < t.size()) {
        bool matched = false;
        for (const char* br : kBrackets) {
            if (t.compare(pos, 3, br) == 0) { pos += 3; matched = true; break; }
        }
        if (!matched)
            return false;
    }
    return true;
}

static int AssFontScaleXPercentFromSpan(const TTMLSpanTag* span)
{
    if (!span)
        return 100;

    float fontWidth = 0;
    float fontHeight = 0;
    if (!TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight) || fontWidth <= 0 || fontHeight <= 0)
        return 100;

    // Broadcasters sometimes transmit full-width brackets (『』「」【】) as
    // MSZ (72x144), which would produce scaleX=50% and cause visual overlap.
    // Treat them as full-width (100%) regardless of the transmitted fontSize.
    if (fontWidth < fontHeight && SpanIsMistaggedFullwidthBracket(span))
        return 100;

    return (std::max)(1, static_cast<int>(std::lround(fontWidth * 100.0 / fontHeight)));
}

static std::string BuildAssStyleTags(const TTMLSpanTag* span, double fontScale,
                                     const MmtsCaptionSettings& settings)
{
    std::string tags;
    if (!span)
        return tags;

    const int assFontSize = AssFontSizeFromSpan(span, fontScale);
    if (assFontSize > 0) {
        tags += FormatAssTag("\\fs%d", assFontSize);
    }
    const int scaleX = AssFontScaleXPercentFromSpan(span);
    if (scaleX != 100)
        tags += FormatAssTag("\\fscx%d", scaleX);
    tags += "\\fn" + settings.fontName + "\\fsp0";
    if (settings.captionAlpha > 0) {
        char alphaBuf[32];
        std::snprintf(alphaBuf, sizeof(alphaBuf), "\\1a&H%02X&",
                      static_cast<unsigned>(settings.captionAlpha));
        tags += alphaBuf;
    }
    if (settings.outlineWidth > 0)
        tags += FormatAssTag("\\bord%d", settings.outlineWidth);

    if (span->style.color.has_value()) {
        try {
            tags += FormatAssColorTag(span->style.color->getValue<TTMLCssValueColor>());
        } catch (const std::bad_variant_access&) {
        }
    }

    return tags;
}

static std::wstring Utf8ToWide(const std::string& text)
{
    int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                    static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0)
        return {};

    std::wstring wide(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), chars);
    return wide;
}

static int CountAssTextChars(const TTMLPTag& p)
{
    int total = 0;
    for (const auto& span : p.spanTags) {
        std::wstring wide = Utf8ToWide(span.text);
        for (wchar_t ch : wide) {
            if (ch != L'\r' && ch != L'\n')
                ++total;
        }
    }
    return total;
}

static double AssCellWidthFromSpan(const TTMLSpanTag* span)
{
    if (!span)
        return 64.0;

    float fontWidth = 0;
    float fontHeight = 0;
    if (TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight) && fontWidth > 0)
        return fontWidth * 1920.0 / 3840.0;

    return BaseAssFontSizeFromSpan(span);
}

static double ParagraphCellWidthAss(const TTMLPTag& p)
{
    const TTMLSpanTag* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
    return AssCellWidthFromSpan(firstSpan);
}

static bool ParagraphOriginX(const TTMLPTag& p, float& originX)
{
    float originY = 0;
    return TryGetLengthPair(p.region.origin, originX, originY);
}

static std::string BuildAssPositionTagsFromAss(double xPos, double yPos)
{
    const int x = static_cast<int>(std::floor(xPos));
    const int y = static_cast<int>(std::floor(yPos));
    return std::string("\\an7") +
           FormatAssTag("\\pos(%d", x) + FormatAssTag(",%d)", y);
}

static bool ParagraphBasePositionAss(const TTMLPTag& p, double extraYAss,
                                     bool hasXOverride, double xOverrideAss,
                                     double& xAss, double& yAss)
{
    float originX = 0;
    float originY = 0;
    if (!TryGetLengthPair(p.region.origin, originX, originY))
        return false;

    xAss = hasXOverride ? xOverrideAss : originX * 1920.0 / 3840.0;
    yAss = (originY + B24LineOffsetYFromParagraph(p)) * 1080.0 / 2160.0 + extraYAss;
    return true;
}

static std::string WideCharSliceToUtf8(const std::wstring& text, size_t pos, size_t& nextPos)
{
    nextPos = pos + 1;
    if (pos >= text.size())
        return {};

    wchar_t chars[2] = { text[pos], 0 };
    int charCount = 1;
    if (chars[0] >= 0xD800 && chars[0] <= 0xDBFF && pos + 1 < text.size() &&
        text[pos + 1] >= 0xDC00 && text[pos + 1] <= 0xDFFF) {
        chars[1] = text[pos + 1];
        charCount = 2;
        nextPos = pos + 2;
    }

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, chars, charCount, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};

    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, chars, charCount, &utf8[0], bytes, nullptr, nullptr);
    return utf8;
}

class SvgPathParser {
public:
    explicit SvgPathParser(const std::string& path) : m_path(path) {}

    bool eof()
    {
        skipSeparators();
        return m_pos >= m_path.size();
    }

    bool readCommand(char& command)
    {
        skipSeparators();
        if (m_pos >= m_path.size())
            return false;
        char c = m_path[m_pos];
        if (std::isalpha(static_cast<unsigned char>(c))) {
            command = c;
            ++m_pos;
            return true;
        }
        return false;
    }

    bool readNumber(double& value)
    {
        skipSeparators();
        if (m_pos >= m_path.size())
            return false;
        const char* begin = m_path.c_str() + m_pos;
        char* end = nullptr;
        value = std::strtod(begin, &end);
        if (end == begin)
            return false;
        m_pos = static_cast<size_t>(end - m_path.c_str());
        return true;
    }

private:
    void skipSeparators()
    {
        while (m_pos < m_path.size()) {
            const unsigned char c = static_cast<unsigned char>(m_path[m_pos]);
            if (std::isspace(c) || c == ',') {
                ++m_pos;
                continue;
            }
            break;
        }
    }

    const std::string& m_path;
    size_t m_pos = 0;
};

static void AppendAssDrawingPoint(std::ostringstream& ass, double value)
{
    ass << static_cast<int>(std::lround(value));
}

static bool BuildAssGlyphDrawingPath(const SubtitleGlyphResource& glyph,
                                     double xPos, double yPos,
                                     double width, double height,
                                     std::string& out)
{
    if (glyph.pathData.empty() || width <= 0 || height <= 0)
        return false;

    const double units = glyph.unitsPerEm > 0 ? glyph.unitsPerEm : 360.0;
    const double glyphHeight = glyph.ascent > glyph.descent ? glyph.ascent - glyph.descent : units;
    const double scaleX = width / units;
    const double scaleY = height / glyphHeight;
    auto tx = [&](double v) { return xPos + v * scaleX; };
    auto ty = [&](double v) { return yPos + (glyph.ascent - v) * scaleY; };

    SvgPathParser parser(glyph.pathData);
    std::ostringstream ass;
    char command = 0;
    double cx = 0;
    double cy = 0;
    double sx = 0;
    double sy = 0;

    auto appendMove = [&](double x, double y) {
        ass << "m ";
        AppendAssDrawingPoint(ass, tx(x));
        ass << ' ';
        AppendAssDrawingPoint(ass, ty(y));
        ass << ' ';
        cx = sx = x;
        cy = sy = y;
    };
    auto appendLine = [&](double x, double y) {
        ass << "l ";
        AppendAssDrawingPoint(ass, tx(x));
        ass << ' ';
        AppendAssDrawingPoint(ass, ty(y));
        ass << ' ';
        cx = x;
        cy = y;
    };
    auto appendCubic = [&](double x1, double y1, double x2, double y2, double x, double y) {
        ass << "b ";
        AppendAssDrawingPoint(ass, tx(x1));
        ass << ' ';
        AppendAssDrawingPoint(ass, ty(y1));
        ass << ' ';
        AppendAssDrawingPoint(ass, tx(x2));
        ass << ' ';
        AppendAssDrawingPoint(ass, ty(y2));
        ass << ' ';
        AppendAssDrawingPoint(ass, tx(x));
        ass << ' ';
        AppendAssDrawingPoint(ass, ty(y));
        ass << ' ';
        cx = x;
        cy = y;
    };

    bool wrote = false;
    while (!parser.eof()) {
        char nextCommand = 0;
        if (parser.readCommand(nextCommand))
            command = nextCommand;
        if (command == 0)
            break;

        const bool relative = command >= 'a' && command <= 'z';
        switch (command) {
        case 'M':
        case 'm':
        {
            double x = 0;
            double y = 0;
            if (!parser.readNumber(x) || !parser.readNumber(y))
                return wrote;
            if (relative) {
                x += cx;
                y += cy;
            }
            appendMove(x, y);
            wrote = true;
            command = relative ? 'l' : 'L';
            break;
        }
        case 'L':
        case 'l':
        {
            double x = 0;
            double y = 0;
            if (!parser.readNumber(x) || !parser.readNumber(y))
                break;
            if (relative) {
                x += cx;
                y += cy;
            }
            appendLine(x, y);
            wrote = true;
            break;
        }
        case 'H':
        case 'h':
        {
            double x = 0;
            if (!parser.readNumber(x))
                break;
            if (relative)
                x += cx;
            appendLine(x, cy);
            wrote = true;
            break;
        }
        case 'V':
        case 'v':
        {
            double y = 0;
            if (!parser.readNumber(y))
                break;
            if (relative)
                y += cy;
            appendLine(cx, y);
            wrote = true;
            break;
        }
        case 'C':
        case 'c':
        {
            double x1 = 0;
            double y1 = 0;
            double x2 = 0;
            double y2 = 0;
            double x = 0;
            double y = 0;
            if (!parser.readNumber(x1) || !parser.readNumber(y1) ||
                !parser.readNumber(x2) || !parser.readNumber(y2) ||
                !parser.readNumber(x) || !parser.readNumber(y)) {
                break;
            }
            if (relative) {
                x1 += cx;
                y1 += cy;
                x2 += cx;
                y2 += cy;
                x += cx;
                y += cy;
            }
            appendCubic(x1, y1, x2, y2, x, y);
            wrote = true;
            break;
        }
        case 'Z':
        case 'z':
            appendLine(sx, sy);
            wrote = true;
            command = 0;
            break;
        default:
            return wrote;
        }
    }

    out = ass.str();
    return wrote && !out.empty();
}

static bool GetSubtitleGlyphResource(int streamIndex, uint32_t codepoint, SubtitleGlyphResource& glyph)
{
    {
        std::lock_guard<std::mutex> lock(g_subtitleGlyphMutex);
        auto it = g_subtitleGlyphs.find({streamIndex, codepoint});
        if (it != g_subtitleGlyphs.end()) {
            glyph = it->second;
            return true;
        }
    }

    TryLoadDumpedSubtitleGlyphResources(streamIndex);

    std::lock_guard<std::mutex> lock(g_subtitleGlyphMutex);
    auto it = g_subtitleGlyphs.find({streamIndex, codepoint});
    if (it != g_subtitleGlyphs.end()) {
        glyph = it->second;
        return true;
    }

    if (codepoint >= 0xE000 && codepoint <= 0xF8FF) {
        LogDetail(L"MMT/TLV Subtitle glyph missing: streamIndex=%d U+%04X\n", streamIndex, codepoint);
    }
    return false;
}

static std::string BuildAssDrawingStyleTags(const TTMLSpanTag* span,
                                            const MmtsCaptionSettings& settings)
{
    std::string tags = "\\bord0";
    if (settings.captionAlpha > 0) {
        char alphaBuf[32];
        std::snprintf(alphaBuf, sizeof(alphaBuf), "\\1a&H%02X&",
                      static_cast<unsigned>(settings.captionAlpha));
        tags += alphaBuf;
    }
    if (span && span->style.color.has_value()) {
        try {
            tags += FormatAssColorTag(span->style.color->getValue<TTMLCssValueColor>());
        } catch (const std::bad_variant_access&) {
        }
    }
    return tags;
}

static bool GetAssBackgroundColor(const TTMLSpanTag& span, const MmtsCaptionSettings& settings,
                                  uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& assAlpha)
{
    r = kDefaultBackgroundRgb;
    g = kDefaultBackgroundRgb;
    b = kDefaultBackgroundRgb;
    bool hasColor = false;

    if (span.style.backgroundColor.has_value()) {
        try {
            const TTMLCssValueColor color = span.style.backgroundColor->getValue<TTMLCssValueColor>();
            r = color.r;
            g = color.g;
            b = color.b;
            if (settings.backgroundAlpha < 0) {
                if (color.a == 0)
                    return false;
                assAlpha = 255 - color.a;
            }
            hasColor = true;
        } catch (const std::bad_variant_access&) {
        }
    }

    if (settings.backgroundAlpha >= 0) {
        if (settings.backgroundAlpha >= 255)
            return false;
        assAlpha = static_cast<uint8_t>(settings.backgroundAlpha);
        return true;
    }

    return hasColor;
}

static void AppendAssBackgroundEvent(double xPos, double yPos, double width, double height,
                                     const TTMLSpanTag& span, const MmtsCaptionSettings& settings,
                                     std::vector<std::string>& events)
{
    if (!settings.showBackground || width <= 0 || height <= 0)
        return;

    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t assAlpha = 0;
    if (!GetAssBackgroundColor(span, settings, r, g, b, assAlpha))
        return;

    const int x1 = static_cast<int>(std::floor(xPos));
    const int y1 = static_cast<int>(std::floor(yPos));
    const int x2 = static_cast<int>(std::floor(xPos + width));
    const int y2 = static_cast<int>(std::floor(yPos + height));
    if (x1 >= x2 || y1 >= y2)
        return;

    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%zu,0,Default,,0,0,0,,{\\an7\\pos(0,0)\\p1\\1c&H%02X%02X%02X&\\1a&H%02X&}"
                  "m %d %d l %d %d %d %d %d %d{\\p0}",
                  events.size(), b, g, r, assAlpha, x1, y1, x2, y1, x2, y2, x1, y2);
    events.emplace_back(buf);
}

static void AppendAssCellBackgroundEvents(const TTMLPTag& p, double fontScale, double extraYAss,
                                          bool hasXOverride, double xOverrideAss,
                                          const MmtsCaptionSettings& settings,
                                          std::vector<std::string>& events)
{
    if (!settings.showBackground)
        return;

    double baseX = 960.0;
    double baseY = 980.0;
    if (!ParagraphBasePositionAss(p, extraYAss, hasXOverride, xOverrideAss, baseX, baseY))
        return;

    const TTMLSpanTag* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
    const int lineFontSize = AssFontSizeFromSpan(firstSpan, fontScale);
    const double lineGap = lineFontSize > 0 ? lineFontSize * kAssLineHeightRatio : 85.0;
    const double fallbackCellWidth = ParagraphCellWidthAss(p);
    double x = baseX;
    double y = baseY;

    for (const auto& span : p.spanTags) {
        if (span.text.empty())
            continue;

        double cellWidth = AssCellWidthFromSpan(&span);
        if (cellWidth <= 0)
            cellWidth = fallbackCellWidth > 0 ? fallbackCellWidth : 64.0;

        const int spanFontSize = AssFontSizeFromSpan(&span, fontScale);
        const std::wstring wide = Utf8ToWide(span.text);
        bool inRun = false;
        double runX = x;
        double runWidth = 0;

        auto flushRun = [&]() {
            if (!inRun || runWidth <= 0)
                return;
            AppendAssBackgroundEvent(runX, y, runWidth, spanFontSize, span, settings, events);
            inRun = false;
            runWidth = 0;
        };

        for (wchar_t ch : wide) {
            if (ch == L'\r')
                continue;
            if (ch == L'\n') {
                flushRun();
                x = baseX;
                y += lineGap;
                continue;
            }

            if (!inRun) {
                inRun = true;
                runX = x;
                runWidth = 0;
            }
            runWidth += cellWidth;
            x += cellWidth;
        }
        flushRun();
    }
}

static bool AppendAssCellLayoutEvents(int streamIndex, const TTMLPTag& p, double fontScale, double extraYAss,
                                      bool hasXOverride, double xOverrideAss,
                                      const MmtsCaptionSettings& settings,
                                      std::vector<std::string>& events,
                                      bool showBackground,
                                      bool* missingGlyph = nullptr)
{
    double baseX = 960.0;
    double baseY = 980.0;
    if (!ParagraphBasePositionAss(p, extraYAss, hasXOverride, xOverrideAss, baseX, baseY))
        return false;

    const TTMLSpanTag* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
    const int lineFontSize = AssFontSizeFromSpan(firstSpan, fontScale);
    const double lineGap = lineFontSize > 0 ? lineFontSize * kAssLineHeightRatio : 85.0;
    const double fallbackCellWidth = ParagraphCellWidthAss(p);
    double x = baseX;
    double y = baseY;
    bool wroteEvent = false;

    if (showBackground)
        AppendAssCellBackgroundEvents(p, fontScale, extraYAss, hasXOverride, xOverrideAss, settings, events);

    for (const auto& span : p.spanTags) {
        if (span.text.empty())
            continue;

        double cellWidth = AssCellWidthFromSpan(&span);
        if (cellWidth <= 0)
            cellWidth = fallbackCellWidth > 0 ? fallbackCellWidth : 64.0;

        const std::string spanTags = BuildAssStyleTags(&span, fontScale, settings);
        const std::wstring wide = Utf8ToWide(span.text);
        for (size_t i = 0; i < wide.size();) {
            const wchar_t ch = wide[i];
            if (ch == L'\r') {
                ++i;
                continue;
            }
            if (ch == L'\n') {
                x = baseX;
                y += lineGap;
                ++i;
                continue;
            }

            const size_t current = i;
            size_t next = i + 1;
            const std::string utf8 = WideCharSliceToUtf8(wide, current, next);
            i = next;
            if (utf8.empty())
                continue;

            uint32_t codepoint = static_cast<uint32_t>(ch);
            if (ch >= 0xD800 && ch <= 0xDBFF && next <= wide.size() && next > current + 1) {
                const uint32_t hi = static_cast<uint32_t>(ch) - 0xD800;
                const uint32_t lo = static_cast<uint32_t>(wide[current + 1]) - 0xDC00;
                codepoint = 0x10000 + ((hi << 10) | lo);
            }

            SubtitleGlyphResource glyph;
            std::string glyphPath;
            const int spanFontSize = AssFontSizeFromSpan(&span, fontScale);
            if (GetSubtitleGlyphResource(streamIndex, codepoint, glyph) &&
                BuildAssGlyphDrawingPath(glyph, x, y, cellWidth, spanFontSize, glyphPath)) {
                std::ostringstream ass;
                ass << events.size() << ",1,Default,,0,0,0,,{\\an7\\pos(0,0)"
                    << BuildAssDrawingStyleTags(&span, settings)
                    << "\\p1}" << glyphPath << "{\\p0}";
                events.push_back(ass.str());
                wroteEvent = true;
                x += cellWidth;
                continue;
            }
            if (missingGlyph && codepoint >= 0xE000 && codepoint <= 0xF8FF)
                *missingGlyph = true;

            std::ostringstream ass;
            ass << events.size() << ",1,Default,,0,0,0,,{"
                << BuildAssPositionTagsFromAss(x, y)
                << spanTags << "}" << EscapeAssText(utf8);
            events.push_back(ass.str());
            wroteEvent = true;
            x += cellWidth;
        }
    }

    return wroteEvent;
}

static bool CalculateRubyCellX(const TTMLPTag& ruby, const TTMLPTag& parent, double& xAss)
{
    float rubyOriginX = 0;
    float parentOriginX = 0;
    if (!ParagraphOriginX(ruby, rubyOriginX) || !ParagraphOriginX(parent, parentOriginX))
        return false;

    const double parentCellAss = ParagraphCellWidthAss(parent);
    const double rubyCellAss = ParagraphCellWidthAss(ruby);
    if (parentCellAss <= 0 || rubyCellAss <= 0)
        return false;

    const double parentCellTtml = parentCellAss * 3840.0 / 1920.0;
    const int rubyChars = (std::max)(1, CountAssTextChars(ruby));
    const int parentIndex = (std::max)(0, static_cast<int>(std::lround((rubyOriginX - parentOriginX) / parentCellTtml)));
    const double parentLeftAss = parentOriginX * 1920.0 / 3840.0;
    xAss = parentLeftAss + parentIndex * parentCellAss +
           (parentCellAss - rubyChars * rubyCellAss) / 2.0;

    LogDetail(L"MMT/TLV Subtitle ruby cell x: ruby=%S parent=%S chars=%d parentIndex=%d parentCell=%.1f rubyCell=%.1f x=%.1f\r\n",
           ruby.id.c_str(), parent.id.c_str(), rubyChars, parentIndex, parentCellAss, rubyCellAss, xAss);
    return true;
}

static double BaseAssYFromParagraph(const TTMLPTag& p)
{
    float originX = 0;
    float originY = 0;
    if (!TryGetLengthPair(p.region.origin, originX, originY))
        return 980.0;
    return (originY + B24LineOffsetYFromParagraph(p)) * 1080.0 / 2160.0;
}

static double AssLineGapForParagraphs(const TTMLPTag& a, const TTMLPTag& b)
{
    const int fontSize = (std::max)(ParagraphMaxBaseFontSize(a), ParagraphMaxBaseFontSize(b));
    return fontSize > 0 ? fontSize * 1.18 : 85.0;
}

static double AssLineGapForRubyCluster(const TTMLPTag& a, const TTMLPTag& b, int rubyFontSize)
{
    const int mainFontSize = (std::max)(ParagraphMaxBaseFontSize(a), ParagraphMaxBaseFontSize(b));
    if (mainFontSize <= 0 || rubyFontSize <= 0)
        return AssLineGapForParagraphs(a, b);

    const double rubyGap = (std::max)(rubyFontSize * 0.25, 8.0);
    return mainFontSize + rubyFontSize + rubyGap * 2.0;
}

static void LogAssParagraphLayout(const TTMLPTag& p, size_t eventIndex, double fontScale, double extraYAss)
{
    float originX = 0;
    float originY = 0;
    float extentX = 0;
    float extentY = 0;
    const bool hasOrigin = TryGetLengthPair(p.region.origin, originX, originY);
    const bool hasExtent = TryGetLengthPair(p.region.extent, extentX, extentY);
    const TTMLSpanTag* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
    const int baseFontSize = BaseAssFontSizeFromSpan(firstSpan);
    const int assFontSize = AssFontSizeFromSpan(firstSpan, fontScale);
    const int lineCount = CountAssParagraphLines(p);

    if (hasOrigin) {
        const double anchorX = hasExtent ? (originX + extentX / 2.0f) : originX;
        const double anchorY = hasExtent ? (originY + extentY / 2.0f) : originY;
        const double assX = anchorX * 1920.0 / 3840.0;
        const double assY = anchorY * 1080.0 / 2160.0;
        const double b24OffsetY = B24LineOffsetYFromParagraph(p);
        const double b24AssX = originX * 1920.0 / 3840.0;
        const double b24AssY = (originY + b24OffsetY) * 1080.0 / 2160.0 + extraYAss;
        LogDetail(L"MMT/TLV Subtitle layout #%zu: pId=%S origin=%.1f,%.1f extent=%s %.1f,%.1f anchor=%.1f,%.1f ass=%.1f,%.1f b24ass=%.1f,%.1f b24offY=%.1f extraY=%.1f baseFs=%d assFs=%d scale=%.3f lines=%d\r\n",
               eventIndex, p.id.c_str(), originX, originY, hasExtent ? L"yes" : L"no",
               extentX, extentY, anchorX, anchorY, assX, assY, b24AssX, b24AssY, b24OffsetY,
               extraYAss, baseFontSize, assFontSize, fontScale, lineCount);
    } else {
        LogDetail(L"MMT/TLV Subtitle layout #%zu: pId=%S origin=none extraY=%.1f baseFs=%d assFs=%d scale=%.3f lines=%d\r\n",
               eventIndex, p.id.c_str(), extraYAss, baseFontSize, assFontSize, fontScale, lineCount);
    }
}

static double AssFontScaleForParagraph(const TTMLPTag& p)
{
    const TTMLSpanTag* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
    const int baseFontSize = BaseAssFontSizeFromSpan(firstSpan);
    float originX = 0;
    float originY = 0;
    if (!TryGetLengthPair(p.region.origin, originX, originY))
        return 1.0;

    float extentX = 0;
    float extentY = 0;
    const bool hasExtent = TryGetLengthPair(p.region.extent, extentX, extentY);
    const double yPos = (originY + B24LineOffsetYFromParagraph(p)) * 1080.0 / 2160.0;
    return FitAssFontScale(baseFontSize, CountAssParagraphLines(p), yPos, hasExtent, extentY);
}

static TtmlTextCue ExtractTtmlPlainText(const uint8_t* data, size_t size, TtmlDebugStats& stats,
                                        int streamIndex = -1)
{
    stats = {};
    TtmlTextCue cue;
    if (!data || size == 0)
        return cue;

    std::string xml(reinterpret_cast<const char*>(data), size);
    TTML ttml = TTMLPaser::parse(xml);
    const MmtsCaptionSettings settings = GetMmtsCaptionSettings();
    std::ostringstream text;

    for (const auto& div : ttml.divTags) {
        const int divMaxFontSize = DivMaxBaseFontSize(div);
        const bool splitParagraphs = HasRubyLikeParagraph(div, divMaxFontSize);
        ++stats.divs;
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
        if (!splitParagraphs) {
            std::vector<const TTMLPTag*> paragraphs;
            paragraphs.reserve(div.pTags.size());
            for (const auto& p : div.pTags)
                paragraphs.push_back(&p);

            if (paragraphs.size() <= 1) {
                for (const auto& p : div.pTags) {
                    ++stats.paragraphs;
                    bool wroteLine = false;
                    const double fontScale = AssFontScaleForParagraph(p);
                    LogAssParagraphLayout(p, cue.assEvents.size(), fontScale, 0);
                    for (const auto& span : p.spanTags) {
                        ++stats.spans;
                        if (!span.text.empty()) {
                            text << span.text;
                            stats.textBytes += span.text.size();
                            wroteLine = true;
                        }
                    }
                    if (wroteLine) {
                        text << "\n";
                        AppendAssCellLayoutEvents(streamIndex, p, fontScale, 0, false, 0,
                                                  settings, cue.assEvents, true, &cue.missingGlyph);
                    }
                }
            } else {
                std::vector<double> paragraphExtraY(paragraphs.size(), 0);
                for (size_t i = 1; i < paragraphs.size(); ++i) {
                    const double prevY = BaseAssYFromParagraph(*paragraphs[i - 1]) + paragraphExtraY[i - 1];
                    const double currentY = BaseAssYFromParagraph(*paragraphs[i]) + paragraphExtraY[i];
                    const double minGap = AssLineGapForParagraphs(*paragraphs[i - 1], *paragraphs[i]);
                    if (currentY - prevY < minGap) {
                        paragraphExtraY[i] += minGap - (currentY - prevY);
                        LogDetail(L"MMT/TLV Subtitle normal line fit: pId=%S prev=%S extraY=%.1f gap=%.1f\r\n",
                               paragraphs[i]->id.c_str(), paragraphs[i - 1]->id.c_str(),
                               paragraphExtraY[i], minGap);
                    }
                }

                for (size_t pIndex = 0; pIndex < paragraphs.size(); ++pIndex) {
                    const auto& p = *paragraphs[pIndex];
                    ++stats.paragraphs;
                    bool wroteLine = false;
                    const double fontScale = AssFontScaleForParagraph(p);
                    const double extraYAss = paragraphExtraY[pIndex];
                    LogAssParagraphLayout(p, cue.assEvents.size(), fontScale, extraYAss);
                    for (const auto& span : p.spanTags) {
                        ++stats.spans;
                        if (!span.text.empty()) {
                            text << span.text;
                            stats.textBytes += span.text.size();
                            wroteLine = true;
                        }
                    }
                    if (wroteLine) {
                        text << "\n";
                        AppendAssCellLayoutEvents(streamIndex, p, fontScale, extraYAss, false, 0,
                                                  settings, cue.assEvents, true, &cue.missingGlyph);
                    }
                }
            }
        } else {
            std::vector<const TTMLPTag*> paragraphs;
            paragraphs.reserve(div.pTags.size());
            for (const auto& p : div.pTags)
                paragraphs.push_back(&p);

            std::vector<double> paragraphExtraY(paragraphs.size(), 0);
            for (size_t i = 0; i < paragraphs.size(); ++i)
                paragraphExtraY[i] = RubyLikeYOffsetAss(*paragraphs[i], divMaxFontSize);
            std::vector<bool> paragraphHasXOverride(paragraphs.size(), false);
            std::vector<double> paragraphXOverride(paragraphs.size(), 0);

            for (size_t i = 0; i < paragraphs.size(); ++i) {
                if (IsRubyLikeParagraph(*paragraphs[i], divMaxFontSize))
                    continue;

                bool sawRubyBetween = false;
                int maxRubyFontBetween = 0;
                for (size_t j = i + 1; j < paragraphs.size(); ++j) {
                    if (IsRubyLikeParagraph(*paragraphs[j], divMaxFontSize)) {
                        sawRubyBetween = true;
                        maxRubyFontBetween = (std::max)(maxRubyFontBetween,
                                                        ParagraphMaxBaseFontSize(*paragraphs[j]));
                        continue;
                    }
                    if (!sawRubyBetween)
                        break;

                    const double currentY = BaseAssYFromParagraph(*paragraphs[i]) + paragraphExtraY[i];
                    const double nextY = BaseAssYFromParagraph(*paragraphs[j]) + paragraphExtraY[j];
                    const double desiredGap = AssLineGapForRubyCluster(*paragraphs[i], *paragraphs[j],
                                                                       maxRubyFontBetween);
                    const double desiredY = nextY - desiredGap;
                    if (desiredY > currentY) {
                        paragraphExtraY[i] += desiredY - currentY;
                        LogDetail(L"MMT/TLV Subtitle main line fit: pId=%S next=%S extraY=%.1f gap=%.1f\r\n",
                               paragraphs[i]->id.c_str(), paragraphs[j]->id.c_str(),
                               paragraphExtraY[i], desiredGap);
                    }
                    break;
                }
            }

            for (size_t i = 0; i < paragraphs.size(); ++i) {
                if (!IsRubyLikeParagraph(*paragraphs[i], divMaxFontSize))
                    continue;

                for (size_t j = i + 1; j < paragraphs.size(); ++j) {
                    if (IsRubyLikeParagraph(*paragraphs[j], divMaxFontSize))
                        continue;

                    const double currentY = BaseAssYFromParagraph(*paragraphs[i]) + paragraphExtraY[i];
                    const double nextY = BaseAssYFromParagraph(*paragraphs[j]) + paragraphExtraY[j];
                    const int rubyFontSize = ParagraphMaxBaseFontSize(*paragraphs[i]);
                    const double desiredGap = (std::max)(rubyFontSize * 0.25, 8.0);
                    const double desiredY = nextY - rubyFontSize - desiredGap;
                    const double deltaY = desiredY - currentY;
                    if (std::abs(deltaY) > 0.5) {
                        paragraphExtraY[i] = (std::max)(0.0, paragraphExtraY[i] + deltaY);
                        LogDetail(L"MMT/TLV Subtitle ruby line fit: pId=%S next=%S extraY=%.1f gap=%.1f\r\n",
                               paragraphs[i]->id.c_str(), paragraphs[j]->id.c_str(),
                               paragraphExtraY[i], desiredGap);
                    }
                    double xAss = 0;
                    if (CalculateRubyCellX(*paragraphs[i], *paragraphs[j], xAss)) {
                        paragraphHasXOverride[i] = true;
                        paragraphXOverride[i] = xAss;
                    }
                    break;
                }
            }

            for (size_t pIndex = 0; pIndex < paragraphs.size(); ++pIndex) {
                const auto& p = *paragraphs[pIndex];
                ++stats.paragraphs;
                bool wroteLine = false;
                const double fontScale = AssFontScaleForParagraph(p);
                const double extraYAss = paragraphExtraY[pIndex];
                LogAssParagraphLayout(p, cue.assEvents.size(), fontScale, extraYAss);
                for (const auto& span : p.spanTags) {
                    ++stats.spans;
                    if (!span.text.empty()) {
                        text << span.text;
                        stats.textBytes += span.text.size();
                        wroteLine = true;
                    }
                }
                if (wroteLine) {
                    text << "\n";
                    AppendAssCellLayoutEvents(streamIndex, p, fontScale, extraYAss,
                                              paragraphHasXOverride[pIndex],
                                              paragraphXOverride[pIndex],
                                              settings, cue.assEvents,
                                              !IsRubyLikeParagraph(p, divMaxFontSize),
                                              &cue.missingGlyph);
                }
            }
        }
    }

    std::string out = text.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    cue.text = std::move(out);
    if (!cue.assEvents.empty())
        cue.assText = cue.assEvents.front();
    if (cue.assText.empty() && !cue.text.empty()) {
        cue.assText = "0,0,Default,,0,0,0,,{\\an2\\pos(960,980)}" + EscapeAssText(cue.text);
        cue.assEvents.push_back(cue.assText);
    }
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
        LogDetail(L"MMT/TLV Splitter: QI for IFileSourceFilter\n");
        return GetInterface(static_cast<IFileSourceFilter*>(this), ppv);
    }
    if (riid == IID_IAMFilterMiscFlags) {
        LogDetail(L"MMT/TLV Splitter: QI for IAMFilterMiscFlags\n");
        return GetInterface(static_cast<IAMFilterMiscFlags*>(this), ppv);
    }
    if (riid == IID_IMediaSeeking) {
        LogDetail(L"MMT/TLV Splitter: QI for IMediaSeeking\n");
        return GetInterface(static_cast<IMediaSeeking*>(this), ppv);
    }
    if (riid == IID_IAMStreamSelect) {
        LogDetail(L"MMT/TLV Splitter: QI for IAMStreamSelect\n");
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
    m_subtitleTimeOffset.store(-1, std::memory_order_release);
    m_demuxByteOffset.store(0, std::memory_order_release);
    m_waitingForVideoRap.store(false, std::memory_order_release);
    ClearPendingSubtitleCues();

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
        auto playableAudioStreams = handler.getPlayableAudioStreams();
        if (!discoveredAudioStreams.empty() && playableAudioStreams.empty()) {
            m_audioUnsupported = true;
            m_handler.setKnownAudioStreams({});
            m_handler.setRequireAdtsConvertibleAudio(true);
            LogMsg(L"MMT/TLV Splitter: audio streams found but no playable ADTS/LATM stream; audio pins disabled\n");
        } else if (!playableAudioStreams.empty()) {
            m_handler.setKnownAudioStreams(playableAudioStreams);
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
            LogMsg(L"MMT/TLV Splitter: PreScan audio[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, samplingRate=%u, format=%s, channels=%u, extra=%zu\n",
                   i, info.streamIndex, info.packetId, info.componentTag, info.samplingRate,
                   info.latm ? L"LATM" : L"ADTS", info.channels, info.extraData.size());
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
                pin->SetAudioInfo(static_cast<int>(audioStreams[i].samplingRate),
                                  audioStreams[i].channels,
                                  16,
                                  audioStreams[i].latm,
                                  audioStreams[i].extraData);
            pin->SetTrackName(audioStreams[i].latm ? L"AAC LATM 22.2ch" : L"AAC ADTS");
            m_pins.push_back(pin);
            LogMsg(L"MMT/TLV Splitter: CreatePins created %s for streamIndex=%d, packetId=0x%04X, componentTag=%d, format=%s, channels=%u, extra=%zu\n",
                   pinName,
                   audioStreams[i].streamIndex,
                   audioStreams[i].packetId,
                   audioStreams[i].componentTag,
                   audioStreams[i].latm ? L"LATM" : L"ADTS",
                   audioStreams[i].channels,
                   audioStreams[i].extraData.size());
        }
    }
    constexpr bool kEnableSubtitlePins = true;
    auto subtitleStreams = m_handler.getSubtitleStreams();
    if (kEnableSubtitlePins) {
        int primarySubtitleStreamIndex = -1;
        for (const auto& info : subtitleStreams) {
            if (info.hasData) {
                primarySubtitleStreamIndex = info.streamIndex;
                break;
            }
        }
        if (primarySubtitleStreamIndex < 0) {
            for (const auto& info : subtitleStreams) {
                if (info.componentTag == 48) {
                    primarySubtitleStreamIndex = info.streamIndex;
                    break;
                }
            }
        }
        if (primarySubtitleStreamIndex < 0 && !subtitleStreams.empty())
            primarySubtitleStreamIndex = subtitleStreams.front().streamIndex;
        for (size_t i = 0; i < subtitleStreams.size(); ++i) {
            WCHAR pinName[64];
            WCHAR trackName[128];
            if (subtitleStreams[i].streamIndex == primarySubtitleStreamIndex) {
                StringCchCopyW(trackName, ARRAYSIZE(trackName), L"\x4E3B\x5B57\x5E55");
            } else {
                StringCchPrintfW(trackName, ARRAYSIZE(trackName),
                                 L"Subtitle (componentTag %d)",
                                 subtitleStreams[i].componentTag);
            }
            StringCchCopyW(pinName, ARRAYSIZE(pinName), trackName);
            auto* pin = new CMmtTlvOutputPin(MmtTlvPinKind::Subtitle, &hr, this, &m_pinLock,
                                             pinName, subtitleStreams[i].streamIndex);
            pin->SetTrackName(trackName);
            m_pins.push_back(pin);
            LogMsg(L"MMT/TLV Splitter: CreatePins created %s for streamIndex=%d, packetId=0x%04X, componentTag=%d, data=%d\n",
                   pinName,
                   subtitleStreams[i].streamIndex,
                   subtitleStreams[i].packetId,
                   subtitleStreams[i].componentTag,
                   subtitleStreams[i].hasData ? 1 : 0);
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

            PumpPendingSubtitleChunks(samplePts);
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
            TtmlTextCue cue = ExtractTtmlPlainText(d, sz, stats, streamIndex);

            REFERENCE_TIME normPts = (pts >= 0 && m_firstPts >= 0) ? pts - m_firstPts : pts;
            DumpSubtitleDataIfEnabled(streamIndex, callbackNo, normPts, d, sz, cue, stats);

            if (cue.missingGlyph && !cue.text.empty()) {
                DeferredSubtitleSample sample;
                sample.streamIndex = streamIndex;
                sample.pts = pts;
                sample.callbackNo = callbackNo;
                sample.data.assign(d, d + sz);
                m_deferredSubtitleSamples.push_back(std::move(sample));
                if (m_deferredSubtitleSamples.size() > 32)
                    m_deferredSubtitleSamples.erase(m_deferredSubtitleSamples.begin());
                LogDetail(L"SUBTITLE CALLBACK deferred for glyph: callback=%ld streamIndex=%d pts=%I64d ms size=%zu\n",
                          callbackNo, streamIndex, normPts / 10000, sz);
                return;
            }

            if (callbackNo <= 20 || cue.text.empty()) {
                std::wstring preview = Utf8Preview(cue.text);
                LogDetail(L"SUBTITLE CALLBACK #%ld: streamIndex=%d, pts=%I64d ms, ttmlBegin=%s%I64d ms, ttmlEnd=%s%I64d ms, size=%zu, divs=%zu, p=%zu, spans=%zu, textBytes=%zu, text=\"%s\"\n",
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
                LogDetail(L"SUBTITLE CALLBACK empty text #%ld: streamIndex=%d, firstBytes=%s\n",
                       callbackNo, streamIndex, hex);
                return;
            }

            REFERENCE_TIME sampleStart;
            REFERENCE_TIME sampleStop;
            bool repeatUntilNextCue = false;
            if (cue.hasBegin) {
                REFERENCE_TIME subtitleTimeOffset = m_subtitleTimeOffset.load(std::memory_order_acquire);
                if (subtitleTimeOffset < 0) {
                    REFERENCE_TIME anchorTime = normPts;
                    if (anchorTime <= 0) {
                        anchorTime = m_currentPts.load(std::memory_order_acquire);
                    }
                    if (anchorTime < 0)
                        anchorTime = 0;
                    REFERENCE_TIME newOffset = cue.begin - anchorTime;
                    REFERENCE_TIME expected = -1;
                    if (m_subtitleTimeOffset.compare_exchange_strong(expected, newOffset, std::memory_order_acq_rel)) {
                        subtitleTimeOffset = newOffset;
                    } else {
                        subtitleTimeOffset = expected;
                    }
                    LogDetail(L"SUBTITLE timing base set: ttmlOffset=%I64d ms, anchor=%I64d ms, firstTtmlBegin=%I64d ms\n",
                           subtitleTimeOffset / 10000,
                           anchorTime / 10000,
                           cue.begin / 10000);
                }
                sampleStart = cue.begin - subtitleTimeOffset;
                if (cue.hasEnd) {
                    sampleStop = cue.end - subtitleTimeOffset;
                } else {
                    REFERENCE_TIME nextBegin = -1;
                    const long long lookaheadOffset = m_demuxByteOffset.load(std::memory_order_acquire);
                    if (FindNextSubtitleBegin(streamIndex, cue.begin, lookaheadOffset, nextBegin)) {
                        sampleStop = nextBegin - subtitleTimeOffset;
                        LogDetail(L"SUBTITLE lookahead: streamIndex=%d, current=%I64d ms, next=%I64d ms, byte=%I64d\n",
                                  streamIndex,
                                  cue.begin / 10000,
                                  nextBegin / 10000,
                                  lookaheadOffset);
                    } else {
                        sampleStop = sampleStart + kSubtitleChunkDuration;
                        repeatUntilNextCue = true;
                        LogDetail(L"SUBTITLE lookahead miss, pending chunk: streamIndex=%d, current=%I64d ms, byte=%I64d\n",
                                  streamIndex,
                                  cue.begin / 10000,
                                  lookaheadOffset);
                    }
                }
                sampleStart = ToSegmentTime(sampleStart, m_segmentStart);
                sampleStop = ToSegmentTime(sampleStop, m_segmentStart);
            } else {
                sampleStart = ToSegmentTime(normPts, m_segmentStart);
                sampleStop = sampleStart + kDefaultSubtitleDuration;
            }
            if (sampleStop <= sampleStart)
                sampleStop = sampleStart + kDefaultSubtitleDuration;

            FlushPendingSubtitleCue(streamIndex, sampleStart);

            if (repeatUntilNextCue) {
                PendingSubtitleCue pending;
                pending.streamIndex = streamIndex;
                pending.start = sampleStart;
                pending.nextChunkStart = sampleStart;
                pending.assEvents = cue.assEvents;
                pending.assText = cue.assText;
                m_pendingSubtitleCues.push_back(std::move(pending));
                LogDetail(L"SUBTITLE pending open: streamIndex=%d, start=%I64d ms, firstDeadline=%I64d ms, chunkStop=%I64d ms\n",
                          streamIndex,
                          sampleStart / 10000,
                          (sampleStart + kSubtitleInitialDelay) / 10000,
                          sampleStop / 10000);
            } else {
                DeliverSubtitleCue(streamIndex, sampleStart, sampleStop, cue.assEvents, cue.assText);
            }
        });

    m_handler.setSubtitleResourceCallback(
        [this](int streamIndex, int dataType, int subsampleNumber, int lastSubsampleNumber,
               long long pts, long long, const uint8_t* d, size_t sz) {
            static volatile LONG s_subtitleResourceCallbacks = 0;
            LONG callbackNo = InterlockedIncrement(&s_subtitleResourceCallbacks);
            REFERENCE_TIME normPts = (pts >= 0 && m_firstPts >= 0) ? pts - m_firstPts : pts;
            RegisterSubtitleGlyphResource(streamIndex, d, sz);
            ProcessDeferredSubtitleSamples();
            DumpSubtitleResourceIfEnabled(streamIndex, callbackNo, normPts,
                                          dataType, subsampleNumber, lastSubsampleNumber,
                                          d, sz);
            if (callbackNo <= 20) {
                LogDetail(L"SUBTITLE RESOURCE CALLBACK #%ld: streamIndex=%d, pts=%I64d ms, dataType=%d, subsample=%d/%d, size=%zu\n",
                          callbackNo, streamIndex, normPts / 10000,
                          dataType, subsampleNumber, lastSubsampleNumber, sz);
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

bool CMmtTlvSplitter::FindNextSubtitleBegin(int streamIndex, REFERENCE_TIME currentBegin,
                                            long long startOffset, REFERENCE_TIME& nextBegin) const
{
    constexpr size_t kChunk = 1024 * 1024;
    constexpr long long kMaxLookaheadBytes = 64LL * 1024 * 1024;
    constexpr REFERENCE_TIME kMinGap = 10 * 10000LL;

    nextBegin = -1;
    if (startOffset < 0 || m_fileSize <= 0)
        return false;

    std::ifstream ifs(m_filename, std::ios::binary);
    if (!ifs.is_open())
        return false;

    if (startOffset > static_cast<long long>(m_fileSize))
        startOffset = static_cast<long long>(m_fileSize);
    ifs.seekg(static_cast<std::streamoff>(startOffset));
    if (!ifs.good())
        return false;

    bool found = false;
    CFilterDemuxerHandler handler;
    handler.setSubtitleCallback(
        [&](int si, bool, long long, long long, bool, bool, const uint8_t* d, size_t sz) {
            if (found || si != streamIndex)
                return;

            TtmlDebugStats stats;
            TtmlTextCue cue = ExtractTtmlPlainText(d, sz, stats, si);
            if (cue.hasBegin && cue.begin > currentBegin + kMinGap) {
                nextBegin = cue.begin;
                found = true;
            }
        });

    MmtTlv::MmtTlvDemuxer demuxer;
    demuxer.setDemuxerHandler(handler);

    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    long long readBytes = 0;

    while (!found && readBytes < kMaxLookaheadBytes) {
        if (buf.size() < 2 * 1024 * 1024) {
            size_t oldSz = buf.size();
            buf.resize(oldSz + kChunk);
            ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
            std::streamsize got = ifs.gcount();
            buf.resize(oldSz + static_cast<size_t>(got));
            readBytes += static_cast<long long>(got);
            if (got == 0 && buf.empty())
                break;
        }

        MmtTlv::Common::ReadStream stream(buf);
        while (!found && !stream.isEof()) {
            auto status = demuxer.demux(stream);
            if (status == MmtTlv::DemuxStatus::NotEnoughBuffer)
                break;
        }

        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) {
            buf.erase(buf.begin(), buf.begin() + consumed);
        } else if (ifs.eof() || !ifs.good()) {
            break;
        } else {
            break;
        }
    }

    return found;
}

void CMmtTlvSplitter::ClearPendingSubtitleCues()
{
    m_pendingSubtitleCues.clear();
    m_deferredSubtitleSamples.clear();
}

void CMmtTlvSplitter::FlushPendingSubtitleCue(int streamIndex, REFERENCE_TIME stopTime)
{
    for (auto it = m_pendingSubtitleCues.begin(); it != m_pendingSubtitleCues.end();) {
        if (it->streamIndex != streamIndex) {
            ++it;
            continue;
        }

        if (stopTime > it->nextChunkStart) {
            DeliverSubtitleCue(it->streamIndex, it->nextChunkStart, stopTime,
                               it->assEvents, it->assText);
            LogDetail(L"SUBTITLE pending close deliver: streamIndex=%d, start=%I64d ms, stop=%I64d ms\n",
                      streamIndex,
                      it->nextChunkStart / 10000,
                      stopTime / 10000);
        } else {
            LogDetail(L"SUBTITLE pending closed: streamIndex=%d, stop=%I64d ms, next=%I64d ms\n",
                      streamIndex,
                      stopTime / 10000,
                      it->nextChunkStart / 10000);
        }
        it = m_pendingSubtitleCues.erase(it);
    }
}

void CMmtTlvSplitter::FlushAllPendingSubtitleCues(REFERENCE_TIME stopTime)
{
    for (const auto& cue : m_pendingSubtitleCues) {
        if (stopTime > cue.nextChunkStart) {
            DeliverSubtitleCue(cue.streamIndex, cue.nextChunkStart, stopTime,
                               cue.assEvents, cue.assText);
            LogDetail(L"SUBTITLE pending final deliver: streamIndex=%d, start=%I64d ms, stop=%I64d ms\n",
                      cue.streamIndex,
                      cue.nextChunkStart / 10000,
                      stopTime / 10000);
        }
    }
    m_pendingSubtitleCues.clear();
}

void CMmtTlvSplitter::ProcessDeferredSubtitleSamples()
{
    if (m_deferredSubtitleSamples.empty())
        return;

    std::vector<DeferredSubtitleSample> remaining;
    for (const auto& sample : m_deferredSubtitleSamples) {
        TtmlDebugStats stats;
        TtmlTextCue cue = ExtractTtmlPlainText(sample.data.data(), sample.data.size(), stats, sample.streamIndex);
        if (cue.missingGlyph) {
            remaining.push_back(sample);
            continue;
        }
        if (cue.text.empty())
            continue;

        REFERENCE_TIME normPts = (sample.pts >= 0 && m_firstPts >= 0) ? sample.pts - m_firstPts : sample.pts;
        REFERENCE_TIME sampleStart = 0;
        REFERENCE_TIME sampleStop = 0;
        if (cue.hasBegin) {
            REFERENCE_TIME subtitleTimeOffset = m_subtitleTimeOffset.load(std::memory_order_acquire);
            if (subtitleTimeOffset < 0) {
                REFERENCE_TIME anchorTime = normPts;
                if (anchorTime <= 0)
                    anchorTime = m_currentPts.load(std::memory_order_acquire);
                subtitleTimeOffset = cue.begin - anchorTime;
                m_subtitleTimeOffset.store(subtitleTimeOffset, std::memory_order_release);
            }
            sampleStart = cue.begin - subtitleTimeOffset;
            sampleStop = cue.hasEnd ? cue.end - subtitleTimeOffset : sampleStart + kDefaultSubtitleDuration;
            sampleStart = ToSegmentTime(sampleStart, m_segmentStart);
            sampleStop = ToSegmentTime(sampleStop, m_segmentStart);
        } else {
            sampleStart = ToSegmentTime(normPts, m_segmentStart);
            sampleStop = sampleStart + kDefaultSubtitleDuration;
        }

        if (sampleStop <= sampleStart)
            sampleStop = sampleStart + kDefaultSubtitleDuration;

        FlushPendingSubtitleCue(sample.streamIndex, sampleStart);
        DeliverSubtitleCue(sample.streamIndex, sampleStart, sampleStop, cue.assEvents, cue.assText);
        LogDetail(L"SUBTITLE deferred delivered: callback=%ld streamIndex=%d start=%I64d ms stop=%I64d ms\n",
                  sample.callbackNo, sample.streamIndex, sampleStart / 10000, sampleStop / 10000);
    }

    m_deferredSubtitleSamples.swap(remaining);
}

void CMmtTlvSplitter::PumpPendingSubtitleChunks(REFERENCE_TIME currentTime)
{
    if (currentTime < 0 || m_pendingSubtitleCues.empty())
        return;

    for (auto& cue : m_pendingSubtitleCues) {
        if (cue.nextChunkStart == cue.start) {
            if (cue.start + kSubtitleInitialDelay > currentTime)
                continue;

            REFERENCE_TIME chunkStart = cue.start;
            REFERENCE_TIME chunkStop = chunkStart + kSubtitleChunkDuration;
            DeliverSubtitleCue(cue.streamIndex, chunkStart, chunkStop, cue.assEvents, cue.assText);
            cue.nextChunkStart = chunkStop;
            LogDetail(L"SUBTITLE pending first: streamIndex=%d, start=%I64d ms, stop=%I64d ms, current=%I64d ms\n",
                      cue.streamIndex,
                      chunkStart / 10000,
                      chunkStop / 10000,
                      currentTime / 10000);
        }

        while (cue.nextChunkStart + kSubtitleChunkDuration <= currentTime) {
            REFERENCE_TIME chunkStart = cue.nextChunkStart;
            REFERENCE_TIME chunkStop = chunkStart + kSubtitleChunkDuration;
            DeliverSubtitleCue(cue.streamIndex, chunkStart, chunkStop, cue.assEvents, cue.assText);
            cue.nextChunkStart = chunkStop;
            LogDetail(L"SUBTITLE pending repeat: streamIndex=%d, start=%I64d ms, stop=%I64d ms, current=%I64d ms\n",
                      cue.streamIndex,
                      chunkStart / 10000,
                      chunkStop / 10000,
                      currentTime / 10000);
        }
    }
}

void CMmtTlvSplitter::DeliverSubtitleCue(int streamIndex, REFERENCE_TIME start, REFERENCE_TIME stop,
                                         const std::vector<std::string>& assEvents,
                                         const std::string& assText)
{
    const MmtsCaptionSettings settings = GetMmtsCaptionSettings();
    if (settings.delayMs != 0) {
        const REFERENCE_TIME offset = static_cast<REFERENCE_TIME>(settings.delayMs) * 10000LL;
        start = (std::max)(static_cast<REFERENCE_TIME>(0), start + offset);
        stop = (std::max)(start + 1, stop + offset);
    }

    bool delivered = false;
    for (auto* pin : m_pins) {
        if (pin->IsSubtitle() && pin->StreamIndex() == streamIndex) {
            if (!assEvents.empty()) {
                for (const auto& sampleText : assEvents) {
                    pin->DeliverTextSample(start, stop, sampleText.c_str(), sampleText.size());
                }
            } else {
                pin->DeliverTextSample(start, stop, assText.c_str(), assText.size());
            }
            delivered = true;
        }
    }
    if (!delivered) {
        LogMsg(L"SUBTITLE CALLBACK: no pin for streamIndex=%d, start=%I64d ms\n",
               streamIndex, start / 10000);
    }
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
    ClearPendingSubtitleCues();

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
    m_subtitleTimeOffset.store(-1, std::memory_order_release);
    ClearPendingSubtitleCues();
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
        m_demuxByteOffset.store(static_cast<long long>(byteOffset), std::memory_order_release);
        LogMsg(L"MMT/TLV Splitter: DemuxLoop seek target=%I64d ms ratio=%.6f byte=%I64d/%I64d ok=%d\n",
               seekTarget / 10000,
               ratio,
               static_cast<long long>(byteOffset),
               static_cast<long long>(m_fileSize),
               ifs.good() ? 1 : 0);
    } else {
        m_demuxByteOffset.store(0, std::memory_order_release);
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
    long long bufferOffset = m_demuxByteOffset.load(std::memory_order_acquire);

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
            long long currentOffset = bufferOffset + static_cast<long long>(buf.size() - stream.leftBytes());
            m_demuxByteOffset.store(currentOffset, std::memory_order_release);
            auto status = m_demuxer.demux(stream);
            if (status == MmtTlv::DemuxStatus::NotEnoughBuffer) break;
        }

        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) {
            buf.erase(buf.begin(), buf.begin() + consumed);
            bufferOffset += static_cast<long long>(consumed);
            m_demuxByteOffset.store(bufferOffset, std::memory_order_release);
        } else {
            // Avoid infinite loop at EOF when remaining buffer is unparseable
            if (ifs.eof() || !ifs.good()) break;
        }
    }

    if (!m_isSeeking) {
        FlushAllPendingSubtitleCues(m_currentPts.load(std::memory_order_relaxed));
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
        LogDetail(L"MMT/TLV StreamSelect: Count stream[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, samplingRate=%u, format=%s, channels=%u\n",
               i, info.streamIndex, info.packetId, info.componentTag, info.samplingRate,
               info.latm ? L"LATM" : L"ADTS", info.channels);
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
        WCHAR formatName[16];
        StringCchCopyW(formatName, ARRAYSIZE(formatName), info.latm ? L"LATM" : L"ADTS");
        WCHAR buf[160];
        StringCchPrintfW(buf, ARRAYSIZE(buf),
                         L"Audio %ld %s %uch (stream %d, component %d)",
                         lIndex + 1, formatName, info.channels,
                         info.streamIndex, info.componentTag);
        const size_t chars = wcslen(buf) + 1;
        *ppszName = static_cast<LPWSTR>(CoTaskMemAlloc(chars * sizeof(WCHAR)));
        if (*ppszName)
            StringCchCopyW(*ppszName, chars, buf);
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
