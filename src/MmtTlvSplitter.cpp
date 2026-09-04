#include "MmtTlvSplitter.h"
#include "DebugLog.h"
#include "Guids.h"
#include "stream.h"     // MmtTlv::Common::ReadStream
#include "TtmlModel.h"
#include "ttml/drcs.h"
#include <fstream>
#include <strmif.h>
#include <cwchar>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

static const WCHAR kFilterName[] = L"MMT/TLV Splitter";
static constexpr REFERENCE_TIME kDefaultSubtitleDuration = 25 * 1000000LL; // 2.5 sec fallback
static constexpr REFERENCE_TIME kSubtitleChunkDuration = kDefaultSubtitleDuration;
static constexpr REFERENCE_TIME kSubtitleInitialDelay = 300 * 10000LL; // 300 ms
// Subtitle MFUs carry no PTS. Anchor each TTML timeline to the video position
// where that timeline is first observed, then preserve TTML spacing until a
// clear TTML begin rollback indicates a program boundary (see
// SubtitleTimingResolver, which owns the resync tolerance).
static constexpr double kAssLineHeightRatio = 1.18;
static constexpr double kAssSubtitleMargin = 20.0;
static constexpr uint8_t kDefaultBackgroundRgb = 0x30;
static constexpr long long kMaxMmtsMapSize = 64LL * 1024 * 1024;
static constexpr uint32_t kMaxMmtsMapTracks = 1024;
static constexpr uint32_t kMaxMmtsMapMptChanges = 100000;
static constexpr uint32_t kMaxMmtsMapPoints = 1000000;
static constexpr uint32_t kMaxMmtsMapMptTracks = 1024;
static constexpr uint32_t kMaxMmtsMapTextLines = 2000000;
static constexpr size_t kMaxMmtsMapTextLine = 4096;
static constexpr long long kMaxMmtsMapTimeMs = 24LL * 60 * 60 * 1000;

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

static bool IsCaptionComponentTag(int componentTag)
{
    return componentTag >= 0x30 && componentTag <= 0x37;
}

static bool ShouldDecodeLatmToPcm(bool latm, uint32_t channels)
{
    return latm && channels == 24;
}

static std::wstring AudioChannelLabel(uint32_t channels)
{
    switch (channels) {
    case 24: return L"22.2";
    case 6: return L"5.1";
    case 2: return L"2.0";
    case 1: return L"1.0";
    default:
        WCHAR buf[16];
        StringCchPrintfW(buf, ARRAYSIZE(buf), L"%uch", channels);
        return buf;
    }
}

static std::wstring AudioSourceLabel(const CFilterDemuxerHandler::AudioStreamInfo& info)
{
    const std::wstring channels = AudioChannelLabel(info.channels);
    return (info.latm ? L"LATM " : L"AAC ") + channels;
}

static int SubtitlePinPriority(const CFilterDemuxerHandler::SubtitleStreamInfo& info)
{
    if (info.hasData && IsCaptionComponentTag(info.componentTag))
        return 0;
    if (IsCaptionComponentTag(info.componentTag))
        return 1;
    if (info.hasData)
        return 2;
    return 3;
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
    const DsTtml::Document ttml = DsTtml::Parse(xml);
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

static std::mutex g_subtitleGlyphMutex;
static std::map<std::pair<int, uint32_t>, arib::ttml::DrcsGlyph> g_subtitleGlyphs;
static std::mutex g_subtitleGlyphLoadMutex;
static std::vector<std::wstring> g_loadedSubtitleGlyphFiles;

struct MmtsCaptionSettings {
    int captionAlpha = 0;       // ASS alpha: 0=opaque, 255=fully transparent
    int backgroundAlpha = -1;   // -1=use TTML data, 0-255=fixed override
    bool showBackground = true;
    bool showRubyBackground = true;
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

static bool IsMmtsEditPath(const std::wstring& path)
{
    static constexpr WCHAR kExtension[] = L".mmtsedit";
    if (path.size() < ARRAYSIZE(kExtension) - 1)
        return false;
    return _wcsicmp(path.c_str() + path.size() - (ARRAYSIZE(kExtension) - 1), kExtension) == 0;
}

static std::wstring MediaPathForEditPath(const std::wstring& editPath)
{
    // A .mmtsedit file is named by appending "edit" to its source .mmts path.
    return editPath.substr(0, editPath.size() - 4);
}

static std::wstring SidecarMapPathFor(const std::wstring& mediaPath)
{
    return mediaPath + L"map";
}

static bool ParseKeyValueLine(const std::string& line, std::string& key, std::string& value)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos)
        return false;
    key = line.substr(0, eq);
    value = line.substr(eq + 1);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
        key.pop_back();
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                              value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    size_t valueStart = value.find_first_not_of(" \t");
    if (valueStart != std::string::npos)
        value.erase(0, valueStart);
    return !key.empty();
}

static std::map<std::string, std::string> ParseSpaceKeyValues(const std::string& text)
{
    std::map<std::string, std::string> values;
    std::istringstream iss(text);
    std::string token;
    while (iss >> token) {
        std::string key;
        std::string value;
        if (ParseKeyValueLine(token, key, value))
            values[key] = value;
    }
    return values;
}

static std::vector<std::string> SplitString(const std::string& text, char delimiter)
{
    std::vector<std::string> parts;
    std::istringstream iss(text);
    std::string part;
    while (std::getline(iss, part, delimiter))
        parts.push_back(part);
    return parts;
}

static long long ParseInt64Value(const std::string& value, long long fallback = 0)
{
    char* end = nullptr;
    long long parsed = std::strtoll(value.c_str(), &end, 0);
    return end && *end == '\0' ? parsed : fallback;
}

static bool ExtractJsonInt64Value(const std::string& text, const char* key, long long& value)
{
    const std::string quotedKey = std::string("\"") + key + "\"";
    size_t pos = text.find(quotedKey);
    if (pos == std::string::npos)
        return false;
    pos = text.find(':', pos + quotedKey.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
        ++pos;
    if (text.compare(pos, 4, "null") == 0)
        return false;

    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str() + pos, &end, 10);
    if (!end || end == text.c_str() + pos)
        return false;
    value = parsed;
    return true;
}

// Like ExtractJsonInt64Value but starts scanning at `from` and reports the
// position just past the parsed number, so callers can iterate a JSON array.
// Returns std::string::npos when the key is not found at/after `from`.
static size_t ExtractJsonInt64ValueAt(const std::string& text, const char* key,
                                      size_t from, long long& value)
{
    const std::string quotedKey = std::string("\"") + key + "\"";
    size_t pos = text.find(quotedKey, from);
    if (pos == std::string::npos)
        return std::string::npos;
    pos = text.find(':', pos + quotedKey.size());
    if (pos == std::string::npos)
        return std::string::npos;
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
        ++pos;
    if (text.compare(pos, 4, "null") == 0)
        return std::string::npos;
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str() + pos, &end, 10);
    if (!end || end == text.c_str() + pos)
        return std::string::npos;
    value = parsed;
    return static_cast<size_t>(end - text.c_str());
}

template <typename T>
static bool ReadPod(std::ifstream& ifs, T& value)
{
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

static std::string MmtsMapTrackTypeName(uint8_t type)
{
    switch (type) {
    case 1: return "video";
    case 2: return "audio";
    case 3: return "subtitle";
    default: return {};
    }
}

static bool IsValidMmtsMapTimeMs(long long timeMs)
{
    return timeMs >= -1 && timeMs <= kMaxMmtsMapTimeMs;
}

static bool IsValidMmtsMapPtsMs(long long timeMs)
{
    return timeMs >= -1 && timeMs <= LLONG_MAX / 10000LL;
}

static bool IsValidMmtsMapOffset(long long offset, std::streamsize fileSize)
{
    return offset >= 0 && offset < static_cast<long long>(fileSize);
}

static void ConfigureMmtsDebugLoggingFromIni()
{
    WCHAR iniPath[MAX_PATH] = {};
    GetMmtsIniPath(iniPath, MAX_PATH);

    WCHAR path[MAX_PATH] = {};
    WCHAR verbose[32] = {};
    WCHAR traceCenter[32] = {};
    WCHAR traceWindow[32] = {};
    std::wstring debugLogPath;
    bool verboseLog = false;
    long long playbackTraceCenterMs = -1;
    long long playbackTraceWindowMs = 3000;

    if (ReadIniValue(iniPath, L"MMTS", L"DebugLogPath", path, ARRAYSIZE(path)))
        debugLogPath = path;
    if (ReadIniValue(iniPath, L"MMTS", L"VerboseLog", verbose, ARRAYSIZE(verbose)))
        verboseLog = _wtoi(verbose) != 0;
    if (ReadIniValue(iniPath, L"MMTS", L"PlaybackTraceCenterMs", traceCenter, ARRAYSIZE(traceCenter)))
        playbackTraceCenterMs = _wtoi64(traceCenter);
    if (ReadIniValue(iniPath, L"MMTS", L"PlaybackTraceWindowMs", traceWindow, ARRAYSIZE(traceWindow)))
        playbackTraceWindowMs = _wtoi64(traceWindow);

    MmtTlvConfigureDebugLog(debugLogPath, verboseLog, playbackTraceCenterMs, playbackTraceWindowMs);
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

    ConfigureMmtsDebugLoggingFromIni();

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
    if (ReadIniValue(iniPath, L"MMTS", L"ShowRubyBackground", buf, ARRAYSIZE(buf)))
        s.showRubyBackground = _wtoi(buf) != 0;
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

static void RegisterSubtitleGlyphResource(int streamIndex, const uint8_t* data, size_t size)
{
    if (!data || size == 0)
        return;

    const std::string payload(reinterpret_cast<const char*>(data), size);
    const auto glyphs = arib::ttml::parse_svg_glyph_resource(payload);
    int registered = 0;
    for (const auto& [codepoint, glyph] : glyphs) {
        std::lock_guard<std::mutex> lock(g_subtitleGlyphMutex);
        g_subtitleGlyphs[{streamIndex, codepoint}] = glyph;
        ++registered;
        LogDetail(L"MMT/TLV Subtitle glyph registered: streamIndex=%d U+%04X units=%d ascent=%d descent=%d\n",
                  streamIndex, codepoint, glyph.unitsPerEm, glyph.ascent, glyph.descent);
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

// DsTtml::Parse() already dropped every non-<length> value, so only the unit
// still has to be checked here - matching the old TryGetLength() behaviour.
static bool TryGetLengthPair(const std::optional<DsTtml::LengthPair>& pair, float& first, float& second)
{
    if (!pair.has_value() || pair->first.unit != "px" || pair->second.unit != "px")
        return false;

    first = pair->first.value;
    second = pair->second.value;
    return true;
}

static std::string FormatAssTag(const char* format, int value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, value);
    return std::string(buf);
}

static std::string FormatAssColorTag(const DsTtml::Color& color)
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

static int BaseAssFontSizeFromSpan(const DsTtml::Span* span)
{
    if (!span)
        return 64;

    float fontWidth = 0;
    float fontHeight = 0;
    if (TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight) && fontHeight > 0)
        return (std::max)(1, static_cast<int>(std::lround(fontHeight * 1080.0 / 2160.0)));

    return 64;
}

static int ParagraphMaxBaseFontSize(const DsTtml::Paragraph& p)
{
    int maxSize = 0;
    for (const auto& span : p.spanTags) {
        if (!span.text.empty())
            maxSize = (std::max)(maxSize, BaseAssFontSizeFromSpan(&span));
    }
    return maxSize;
}

static int DivMaxBaseFontSize(const DsTtml::Division& div)
{
    int maxSize = 0;
    for (const auto& p : div.pTags)
        maxSize = (std::max)(maxSize, ParagraphMaxBaseFontSize(p));
    return maxSize;
}

static double RubyLikeYOffsetAss(const DsTtml::Paragraph& p, int divMaxFontSize)
{
    const int pFontSize = ParagraphMaxBaseFontSize(p);
    if (divMaxFontSize <= 0 || pFontSize <= 0 || pFontSize * 10 > divMaxFontSize * 6)
        return 0;

    return divMaxFontSize - pFontSize;
}

// arib-tt:ruby states the role of a span outright, which beats inferring it
// from a font size. It is optional though, and the captures on hand never carry
// it, so it is only trusted for a division that actually names a role we know:
// anything else keeps the heuristic above. Values other than "base" and "text"
// are not interpreted.
static bool SpanRubyRoleIs(const DsTtml::Span& span, const char* role)
{
    return span.style.ruby.has_value() && *span.style.ruby == role;
}

static bool ParagraphIsDeclaredRubyText(const DsTtml::Paragraph& p)
{
    for (const auto& span : p.spanTags) {
        if (SpanRubyRoleIs(span, "text"))
            return true;
    }
    return false;
}

static bool DivisionDeclaresRubyRoles(const DsTtml::Division& div)
{
    for (const auto& p : div.pTags) {
        for (const auto& span : p.spanTags) {
            if (SpanRubyRoleIs(span, "text") || SpanRubyRoleIs(span, "base"))
                return true;
        }
    }
    return false;
}

static bool IsRubyLikeParagraph(const DsTtml::Paragraph& p, int divMaxFontSize, bool divDeclaresRubyRoles)
{
    if (divDeclaresRubyRoles)
        return ParagraphIsDeclaredRubyText(p);

    return RubyLikeYOffsetAss(p, divMaxFontSize) > 0;
}

static bool HasRubyLikeParagraph(const DsTtml::Division& div, int divMaxFontSize)
{
    const bool divDeclaresRubyRoles = DivisionDeclaresRubyRoles(div);
    for (const auto& p : div.pTags) {
        if (IsRubyLikeParagraph(p, divMaxFontSize, divDeclaresRubyRoles))
            return true;
    }
    return false;
}

static double B24FontHeightFromSpan(const DsTtml::Span* span)
{
    if (!span)
        return 0;

    float fontWidth = 0;
    float fontHeight = 0;
    if (!TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight))
        return 0;

    return fontHeight;
}

static double B24LineOffsetYFromParagraph(const DsTtml::Paragraph& p)
{
    if (p.spanTags.empty() || !p.region.extent.has_value())
        return 0;

    const DsTtml::Span* firstSpan = &(*p.spanTags.begin());
    if (!firstSpan->style.lineHeight.has_value() || !firstSpan->style.fontSize.has_value())
        return 0;

    const double lineHeight = firstSpan->style.lineHeight->value;
    const double fontHeight = B24FontHeightFromSpan(firstSpan);
    return (lineHeight - fontHeight) / 2.0;
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

static int CountAssParagraphLines(const DsTtml::Paragraph& p)
{
    int lines = 1;
    for (const auto& span : p.spanTags)
        lines = (std::max)(lines, CountAssTextLines(span.text));
    return lines;
}

static double AssBackgroundCellHeightFromSpan(const DsTtml::Paragraph& p, const DsTtml::Span* span,
                                              double fontScale)
{
    if (span && span->style.lineHeight.has_value()) {
        const DsTtml::Length& lineHeight = *span->style.lineHeight;
        if (lineHeight.unit == "px" && lineHeight.value > 0)
            return lineHeight.value * 1080.0 / 2160.0;
    }

    float extentX = 0;
    float extentY = 0;
    if (TryGetLengthPair(p.region.extent, extentX, extentY) && extentY > 0) {
        const int lines = (std::max)(1, CountAssParagraphLines(p));
        return extentY * 1080.0 / 2160.0 / lines;
    }

    return (std::max)(1.0, BaseAssFontSizeFromSpan(span) * fontScale);
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

static int AssFontSizeFromSpan(const DsTtml::Span* span, double fontScale)
{
    return (std::max)(1, static_cast<int>(std::lround(BaseAssFontSizeFromSpan(span) * fontScale)));
}

static int AssFontScaleXPercentFromSpan(const DsTtml::Span* span)
{
    if (!span)
        return 100;

    float fontWidth = 0;
    float fontHeight = 0;
    if (!TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight) || fontWidth <= 0 || fontHeight <= 0)
        return 100;

    return (std::max)(1, static_cast<int>(std::lround(fontWidth * 100.0 / fontHeight)));
}

static std::string BuildAssStyleTags(const DsTtml::Span* span, double fontScale,
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
    tags += FormatAssTag("\\bord%d", settings.outlineWidth);
    tags += "\\shad0";

    if (span->style.color.has_value())
        tags += FormatAssColorTag(*span->style.color);

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

static int CountAssTextChars(const DsTtml::Paragraph& p)
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

static double AssCellWidthFromSpan(const DsTtml::Span* span)
{
    if (!span)
        return 64.0;

    float fontWidth = 0;
    float fontHeight = 0;
    if (TryGetLengthPair(span->style.fontSize, fontWidth, fontHeight) && fontWidth > 0)
        return fontWidth * 1920.0 / 3840.0;

    return BaseAssFontSizeFromSpan(span);
}

static double AssLetterSpacingFromSpan(const DsTtml::Span* span)
{
    if (!span || !span->style.letterSpacing.has_value())
        return 0.0;

    const DsTtml::Length& spacing = *span->style.letterSpacing;
    if (spacing.unit != "px" || spacing.value <= 0)
        return 0.0;

    return spacing.value * 1920.0 / 3840.0;
}

// B24 lays text out on a grid of cells that are one font width plus one
// letter-spacing wide - a region's extent is exactly the cell count times this.
// The glyph itself only fills the font width, so this is the pen advance and
// the width of the cell background, not the size the character is drawn at.
static double AssCellAdvanceFromSpan(const DsTtml::Span* span)
{
    const double cellWidth = AssCellWidthFromSpan(span);
    if (cellWidth <= 0)
        return cellWidth;

    return cellWidth + AssLetterSpacingFromSpan(span);
}

static double ParagraphCellAdvanceAss(const DsTtml::Paragraph& p)
{
    const DsTtml::Span* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
    return AssCellAdvanceFromSpan(firstSpan);
}

// The cell grid makes a paragraph's laid-out width exactly its region extent:
// extent = cells * (fontSize + letter-spacing). This recomputes the left side
// of that identity from the spans, so the layout log can check it.
static double ParagraphLaidOutWidthAss(const DsTtml::Paragraph& p)
{
    const double fallbackAdvance = ParagraphCellAdvanceAss(p);
    double widest = 0;
    double lineWidth = 0;

    for (const auto& span : p.spanTags) {
        double advance = AssCellAdvanceFromSpan(&span);
        if (advance <= 0)
            advance = fallbackAdvance > 0 ? fallbackAdvance : 64.0;

        for (wchar_t ch : Utf8ToWide(span.text)) {
            if (ch == L'\r')
                continue;
            if (ch == L'\n') {
                widest = (std::max)(widest, lineWidth);
                lineWidth = 0;
                continue;
            }
            // A surrogate pair is one cell, as it is when the text is laid out.
            if (ch >= 0xDC00 && ch <= 0xDFFF)
                continue;
            lineWidth += advance;
        }
    }

    return (std::max)(widest, lineWidth);
}

static std::string BuildAssPositionTagsFromAss(double xPos, double yPos)
{
    const int x = static_cast<int>(std::floor(xPos));
    const int y = static_cast<int>(std::floor(yPos));
    return std::string("\\an7") +
           FormatAssTag("\\pos(%d", x) + FormatAssTag(",%d)", y);
}

static bool ParagraphBasePositionAss(const DsTtml::Paragraph& p, double extraYAss,
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

static bool ParagraphBackgroundPositionAss(const DsTtml::Paragraph& p, double extraYAss,
                                           bool hasXOverride, double xOverrideAss,
                                           double& xAss, double& yAss)
{
    float originX = 0;
    float originY = 0;
    if (!TryGetLengthPair(p.region.origin, originX, originY))
        return false;

    xAss = hasXOverride ? xOverrideAss : originX * 1920.0 / 3840.0;
    yAss = originY * 1080.0 / 2160.0 + extraYAss;
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

static void AppendAssDrawingPoint(std::ostringstream& ass, double value)
{
    ass << static_cast<int>(std::lround(value));
}

static bool BuildAssGlyphDrawingPath(const arib::ttml::DrcsGlyph& glyph,
                                     double xPos, double yPos,
                                     double width, double height,
                                     std::string& out)
{
    if (glyph.path.empty() || width <= 0 || height <= 0)
        return false;

    const double units = glyph.unitsPerEm > 0 ? glyph.unitsPerEm : 360.0;
    const double glyphHeight = glyph.ascent > glyph.descent ? glyph.ascent - glyph.descent : units;
    const double scaleX = width / units;
    const double scaleY = height / glyphHeight;
    auto tx = [&](double v) { return xPos + v * scaleX; };
    auto ty = [&](double v) { return yPos + (glyph.ascent - v) * scaleY; };

    const arib::ttml::SvgPath path = arib::ttml::parse_svg_path(glyph.path);
    if (!path.complete()) {
        LogDetail(L"MMT/TLV Subtitle glyph path unsupported: command=%C\n",
                  static_cast<wchar_t>(path.unsupportedCommand));
        return false;
    }

    std::ostringstream ass;
    bool wrote = false;
    for (const auto& command : path.commands) {
        switch (command.type) {
        case arib::ttml::SvgPathCommandType::MoveTo:
            ass << "m ";
            AppendAssDrawingPoint(ass, tx(command.point.x));
            ass << ' ';
            AppendAssDrawingPoint(ass, ty(command.point.y));
            ass << ' ';
            break;
        case arib::ttml::SvgPathCommandType::LineTo:
        case arib::ttml::SvgPathCommandType::ClosePath:
            ass << "l ";
            AppendAssDrawingPoint(ass, tx(command.point.x));
            ass << ' ';
            AppendAssDrawingPoint(ass, ty(command.point.y));
            ass << ' ';
            wrote = true;
            break;
        case arib::ttml::SvgPathCommandType::CubicTo:
            ass << "b ";
            AppendAssDrawingPoint(ass, tx(command.control1.x));
            ass << ' ';
            AppendAssDrawingPoint(ass, ty(command.control1.y));
            ass << ' ';
            AppendAssDrawingPoint(ass, tx(command.control2.x));
            ass << ' ';
            AppendAssDrawingPoint(ass, ty(command.control2.y));
            ass << ' ';
            AppendAssDrawingPoint(ass, tx(command.point.x));
            ass << ' ';
            AppendAssDrawingPoint(ass, ty(command.point.y));
            ass << ' ';
            wrote = true;
            break;
        }
    }

    out = ass.str();
    return wrote && !out.empty();
}

static bool GetSubtitleGlyphResource(int streamIndex, uint32_t codepoint, arib::ttml::DrcsGlyph& glyph)
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

static std::string BuildAssDrawingStyleTags(const DsTtml::Span* span,
                                            const MmtsCaptionSettings& settings)
{
    std::string tags = "\\bord0\\shad0";
    if (settings.captionAlpha > 0) {
        char alphaBuf[32];
        std::snprintf(alphaBuf, sizeof(alphaBuf), "\\1a&H%02X&",
                      static_cast<unsigned>(settings.captionAlpha));
        tags += alphaBuf;
    }
    if (span && span->style.color.has_value())
        tags += FormatAssColorTag(*span->style.color);

    return tags;
}

static bool GetAssBackgroundColor(const DsTtml::Span& span, const MmtsCaptionSettings& settings,
                                  uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& assAlpha)
{
    r = kDefaultBackgroundRgb;
    g = kDefaultBackgroundRgb;
    b = kDefaultBackgroundRgb;
    bool hasColor = false;

    if (span.style.backgroundColor.has_value()) {
        const DsTtml::Color& color = *span.style.backgroundColor;
        r = color.r;
        g = color.g;
        b = color.b;
        if (settings.backgroundAlpha < 0) {
            if (color.a == 0)
                return false;
            assAlpha = 255 - color.a;
        }
        hasColor = true;
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
                                     const DsTtml::Span& span, const MmtsCaptionSettings& settings,
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

static void AppendAssCellBackgroundEvents(const DsTtml::Paragraph& p, double fontScale, double extraYAss,
                                          bool hasXOverride, double xOverrideAss,
                                          const MmtsCaptionSettings& settings,
                                          std::vector<std::string>& events)
{
    if (!settings.showBackground)
        return;

    double baseX = 960.0;
    double baseY = 980.0;
    if (!ParagraphBackgroundPositionAss(p, extraYAss, hasXOverride, xOverrideAss, baseX, baseY))
        return;

    const DsTtml::Span* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
    const double lineGap = AssBackgroundCellHeightFromSpan(p, firstSpan, fontScale);
    const double fallbackCellAdvance = ParagraphCellAdvanceAss(p);
    double x = baseX;
    double y = baseY;

    // Merged run state: accumulate adjacent cells with identical background
    // color into a single rectangle to avoid dark edge lines from alpha
    // compositing between separately drawn same-color rectangles.
    bool     inRun     = false;
    double   runX      = 0;
    double   runWidth  = 0;
    double   runHeight = 0;
    uint8_t  runR = 0, runG = 0, runB = 0, runA = 0;

    auto flushRun = [&]() {
        if (!inRun || runWidth <= 0) return;
        const int x1 = static_cast<int>(std::floor(runX));
        const int y1 = static_cast<int>(std::floor(y));
        const int x2 = static_cast<int>(std::floor(runX + runWidth));
        const int y2 = static_cast<int>(std::floor(y + runHeight));
        if (x1 < x2 && y1 < y2) {
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                          "%zu,0,Default,,0,0,0,,{\\an7\\pos(0,0)\\p1"
                          "\\1c&H%02X%02X%02X&\\1a&H%02X&}"
                          "m %d %d l %d %d %d %d %d %d{\\p0}",
                          events.size(), runB, runG, runR, runA,
                          x1, y1, x2, y1, x2, y2, x1, y2);
            events.emplace_back(buf);
        }
        inRun = false;
        runWidth = 0;
    };

    for (const auto& span : p.spanTags) {
        if (span.text.empty())
            continue;

        double cellAdvance = AssCellAdvanceFromSpan(&span);
        if (cellAdvance <= 0)
            cellAdvance = fallbackCellAdvance > 0 ? fallbackCellAdvance : 64.0;

        const double spanBackgroundHeight = AssBackgroundCellHeightFromSpan(p, &span, fontScale);
        const std::wstring wide = Utf8ToWide(span.text);

        uint8_t spanR = 0, spanG = 0, spanB = 0, spanA = 0;
        const bool hasColor = GetAssBackgroundColor(span, settings, spanR, spanG, spanB, spanA);

        for (wchar_t ch : wide) {
            if (ch == L'\r')
                continue;
            if (ch == L'\n') {
                flushRun();
                x = baseX;
                y += lineGap;
                continue;
            }

            if (!hasColor) {
                // No background for this cell: flush any active run and skip
                flushRun();
                x += cellAdvance;
                continue;
            }

            // Extend the active run if color matches; otherwise start a new one
            if (inRun && spanR == runR && spanG == runG && spanB == runB && spanA == runA) {
                runWidth += cellAdvance;
            } else {
                flushRun();
                inRun    = true;
                runX     = x;
                runWidth = cellAdvance;
                runHeight = spanBackgroundHeight;
                runR = spanR; runG = spanG; runB = spanB; runA = spanA;
            }
            x += cellAdvance;
        }
    }
    flushRun();
}

static bool AppendAssCellLayoutEvents(int streamIndex, const DsTtml::Paragraph& p, double fontScale, double extraYAss,
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

    const DsTtml::Span* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
    const int lineFontSize = AssFontSizeFromSpan(firstSpan, fontScale);
    const double lineGap = lineFontSize > 0 ? lineFontSize * kAssLineHeightRatio : 85.0;
    const double fallbackCellAdvance = ParagraphCellAdvanceAss(p);
    double x = baseX;
    double y = baseY;
    bool wroteEvent = false;

    if (showBackground)
        AppendAssCellBackgroundEvents(p, fontScale, extraYAss, hasXOverride, xOverrideAss, settings, events);

    for (const auto& span : p.spanTags) {
        if (span.text.empty())
            continue;

        double cellAdvance = AssCellAdvanceFromSpan(&span);
        if (cellAdvance <= 0)
            cellAdvance = fallbackCellAdvance > 0 ? fallbackCellAdvance : 64.0;
        double glyphWidth = AssCellWidthFromSpan(&span);
        if (glyphWidth <= 0)
            glyphWidth = cellAdvance;

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

            arib::ttml::DrcsGlyph glyph;
            std::string glyphPath;
            const int spanFontSize = AssFontSizeFromSpan(&span, fontScale);
            const bool hasGlyph = GetSubtitleGlyphResource(streamIndex, codepoint, glyph);
            if (hasGlyph && BuildAssGlyphDrawingPath(glyph, x, y, glyphWidth, spanFontSize, glyphPath)) {
                std::ostringstream ass;
                ass << events.size() << ",1,Default,,0,0,0,,{\\an7\\pos(0,0)"
                    << BuildAssDrawingStyleTags(&span, settings)
                    << "\\p1}" << glyphPath << "{\\p0}";
                events.push_back(ass.str());
                wroteEvent = true;
                x += cellAdvance;
                continue;
            }
            if (missingGlyph && !hasGlyph && codepoint >= 0xE000 && codepoint <= 0xF8FF)
                *missingGlyph = true;

            std::ostringstream ass;
            ass << events.size() << ",1,Default,,0,0,0,,{"
                << BuildAssPositionTagsFromAss(x, y)
                << spanTags << "}" << EscapeAssText(utf8);
            events.push_back(ass.str());
            wroteEvent = true;
            x += cellAdvance;
        }
    }

    return wroteEvent;
}

static double BaseAssYFromParagraph(const DsTtml::Paragraph& p)
{
    float originX = 0;
    float originY = 0;
    if (!TryGetLengthPair(p.region.origin, originX, originY))
        return 980.0;
    return (originY + B24LineOffsetYFromParagraph(p)) * 1080.0 / 2160.0;
}

static double AssLineGapForParagraphs(const DsTtml::Paragraph& a, const DsTtml::Paragraph& b)
{
    const int fontSize = (std::max)(ParagraphMaxBaseFontSize(a), ParagraphMaxBaseFontSize(b));
    return fontSize > 0 ? fontSize * 1.18 : 85.0;
}

static double AssLineGapForRubyCluster(const DsTtml::Paragraph& a, const DsTtml::Paragraph& b, int rubyFontSize)
{
    const int mainFontSize = (std::max)(ParagraphMaxBaseFontSize(a), ParagraphMaxBaseFontSize(b));
    if (mainFontSize <= 0 || rubyFontSize <= 0)
        return AssLineGapForParagraphs(a, b);

    const double rubyGap = (std::max)(rubyFontSize * 0.25, 8.0);
    return mainFontSize + rubyFontSize + rubyGap * 2.0;
}

static void LogAssParagraphLayout(const DsTtml::Paragraph& p, size_t eventIndex, double fontScale, double extraYAss)
{
    float originX = 0;
    float originY = 0;
    float extentX = 0;
    float extentY = 0;
    const bool hasOrigin = TryGetLengthPair(p.region.origin, originX, originY);
    const bool hasExtent = TryGetLengthPair(p.region.extent, extentX, extentY);
    const DsTtml::Span* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
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

        // The region's extent is the broadcast's own statement of how wide the
        // paragraph is, so a laid-out width that disagrees means the pen advance
        // no longer matches the cell grid - which on screen is ruby sitting off
        // the character it annotates. Report it whether or not verbose logging
        // is on, since it is a defect rather than a detail.
        if (hasExtent && extentX > 0) {
            const double laidOutWidth = ParagraphLaidOutWidthAss(p);
            const double extentWidthAss = extentX * 1920.0 / 3840.0;
            const double delta = laidOutWidth - extentWidthAss;
            if (std::abs(delta) >= 1.0) {
                LogMsg(L"MMT/TLV Subtitle layout extent mismatch #%zu: pId=%S laidOut=%.1f extent=%.1f delta=%+.1f cells=%d\r\n",
                       eventIndex, p.id.c_str(), laidOutWidth, extentWidthAss, delta,
                       CountAssTextChars(p));
            } else {
                LogDetail(L"MMT/TLV Subtitle layout extent check #%zu: pId=%S laidOut=%.1f extent=%.1f cells=%d\r\n",
                       eventIndex, p.id.c_str(), laidOutWidth, extentWidthAss, CountAssTextChars(p));
            }
        }
    } else {
        LogDetail(L"MMT/TLV Subtitle layout #%zu: pId=%S origin=none extraY=%.1f baseFs=%d assFs=%d scale=%.3f lines=%d\r\n",
               eventIndex, p.id.c_str(), extraYAss, baseFontSize, assFontSize, fontScale, lineCount);
    }
}

static double AssFontScaleForParagraph(const DsTtml::Paragraph& p)
{
    const DsTtml::Span* firstSpan = p.spanTags.empty() ? nullptr : &(*p.spanTags.begin());
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
    const DsTtml::Document ttml = DsTtml::Parse(xml);
    const MmtsCaptionSettings settings = GetMmtsCaptionSettings();
    std::ostringstream text;

    for (const auto& div : ttml.divTags) {
        const int divMaxFontSize = DivMaxBaseFontSize(div);
        const bool divDeclaresRubyRoles = DivisionDeclaresRubyRoles(div);
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
            std::vector<const DsTtml::Paragraph*> paragraphs;
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
                    const double prevRawY = BaseAssYFromParagraph(*paragraphs[i - 1]);
                    const double currentRawY = BaseAssYFromParagraph(*paragraphs[i]);
                    // Paragraphs sharing the same Y origin are side-by-side on the
                    // same row (different X positions); skip vertical gap adjustment.
                    if (std::abs(currentRawY - prevRawY) < 1.0)
                        continue;
                    const double prevY = prevRawY + paragraphExtraY[i - 1];
                    const double currentY = currentRawY + paragraphExtraY[i];
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
            std::vector<const DsTtml::Paragraph*> paragraphs;
            paragraphs.reserve(div.pTags.size());
            for (const auto& p : div.pTags)
                paragraphs.push_back(&p);

            std::vector<double> paragraphExtraY(paragraphs.size(), 0);

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
                                              false,
                                              0,
                                              settings, cue.assEvents,
                                              IsRubyLikeParagraph(p, divMaxFontSize, divDeclaresRubyRoles)
                                                  ? settings.showRubyBackground
                                                  : true,
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
    ConfigureMmtsDebugLoggingFromIni();
    const std::wstring requestedPath = pszFileName;
    const bool loadEdit = IsMmtsEditPath(requestedPath);
    m_filename = loadEdit ? MediaPathForEditPath(requestedPath) : requestedPath;
    m_handler.reset();
    m_handler.resetAudioSelection();
    m_videoWidth = 3840;
    m_videoHeight = 2160;
    m_audioUnsupported = false;
    m_sourceDuration = 0;
    m_virtualStart = 0;
    m_virtualEnd = 0;
    m_hasSidecarIndex = false;
    m_seekTarget = 0;
    m_currentPts = 0;
    m_currentDts = -1;
    m_segmentStart = 0;
    m_segmentTimeOffset.store(0, std::memory_order_release);
    m_subtitleResolver.Reset(false);
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

    if (loadEdit)
        LoadSidecarEdit(requestedPath);
    LoadSidecarMap();
    PreScanFile();   // sets m_hevcExtradata, m_firstPts, m_duration
    ApplySidecarMap();
    ApplySidecarIndex();
    CreatePins();
    return S_OK;
}

void CMmtTlvSplitter::LoadSidecarEdit(const std::wstring& editPath)
{
    m_virtualStart = 0;
    m_virtualEnd = 0;
    m_hasSidecarIndex = false;
    m_editSegments.clear();

    std::ifstream ifs(editPath, std::ios::binary | std::ios::ate);
    if (!ifs.is_open())
        return;

    const std::streamoff size = ifs.tellg();
    if (size <= 0 || size > 1024 * 1024) {
        LogMsg(L"MMT/TLV Splitter: mmtsedit ignored, invalid sidecar size: %s size=%I64d\n",
               editPath.c_str(), static_cast<long long>(size));
        return;
    }
    ifs.seekg(0);
    std::string text(static_cast<size_t>(size), '\0');
    if (!ifs.read(text.data(), size)) {
        LogMsg(L"MMT/TLV Splitter: mmtsedit ignored, read failed: %s\n", editPath.c_str());
        return;
    }

    long long version = 0;
    long long sourceSize = -1;
    ExtractJsonInt64Value(text, "version", version);
    ExtractJsonInt64Value(text, "sourceSize", sourceSize);

    if (version != 1) {
        LogMsg(L"MMT/TLV Splitter: mmtsedit ignored, version=%I64d\n", version);
        return;
    }
    if (sourceSize >= 0 && sourceSize != static_cast<long long>(m_fileSize)) {
        LogMsg(L"MMT/TLV Splitter: mmtsedit ignored, size mismatch: edit=%I64d file=%I64d\n",
               sourceSize, static_cast<long long>(m_fileSize));
        return;
    }

    // Parse every timeline segment (the EDL). The edited program is their
    // concatenation in order. A single segment keeps the original trim path.
    std::vector<EditSeg> segs;
    size_t scan = 0;
    for (;;) {
        long long startMs = 0;
        const size_t afterStart = ExtractJsonInt64ValueAt(text, "sourceStartMs", scan, startMs);
        if (afterStart == std::string::npos)
            break;
        long long endMs = 0;
        const size_t afterEnd = ExtractJsonInt64ValueAt(text, "sourceEndMs", afterStart, endMs);
        scan = (afterEnd != std::string::npos) ? afterEnd : afterStart;
        if (afterEnd == std::string::npos)
            continue; // a start without an end is not a usable cut
        if (startMs < 0 || startMs > kMaxMmtsMapTimeMs || endMs <= startMs || endMs > kMaxMmtsMapTimeMs) {
            LogMsg(L"MMT/TLV Splitter: mmtsedit skipping invalid segment start=%I64d end=%I64d\n", startMs, endMs);
            continue;
        }
        EditSeg seg;
        seg.start = static_cast<REFERENCE_TIME>(startMs) * 10000LL;
        seg.end = static_cast<REFERENCE_TIME>(endMs) * 10000LL;
        segs.push_back(seg);
    }

    if (segs.empty()) {
        LogMsg(L"MMT/TLV Splitter: mmtsedit ignored, no usable segments: %s\n", editPath.c_str());
        return;
    }

    m_editSegments = segs;
    m_virtualStart = segs.front().start;
    m_virtualEnd = (segs.size() == 1) ? segs.front().end : 0; // multi-seg uses its own path
    m_hasSidecarIndex = true;

    REFERENCE_TIME totalMs = 0;
    for (const auto& s : segs)
        totalMs += (s.end - s.start) / 10000;
    LogMsg(L"MMT/TLV Splitter: mmtsedit loaded: %s segments=%zu totalDuration=%I64d ms\n",
           editPath.c_str(), segs.size(), totalMs);
}

void CMmtTlvSplitter::LoadSidecarMap()
{
    m_hasSidecarMap = false;
    m_mapDuration = 0;
    m_mapFirstVideoPts = -1;
    m_sidecarMapTracks.clear();
    m_sidecarMapMptChanges.clear();
    m_sidecarMapRapPoints.clear();
    m_sidecarMapSeekPoints.clear();

    const std::wstring mapPath = SidecarMapPathFor(m_filename);
    std::ifstream ifs(mapPath, std::ios::binary);
    if (!ifs.is_open())
        return;

    ifs.seekg(0, std::ios::end);
    const std::streamoff mapSize = ifs.tellg();
    if (mapSize < 0 || mapSize > kMaxMmtsMapSize) {
        LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, invalid sidecar size: %s size=%I64d\n",
               mapPath.c_str(), static_cast<long long>(mapSize));
        return;
    }
    ifs.seekg(0);

    auto isValidTrack = [](const SidecarMapTrack& track) {
        if (track.type != "video" && track.type != "audio" && track.type != "subtitle")
            return false;
        if (track.streamIndex < 0 || track.streamIndex > 8192)
            return false;
        if (track.packetId == 0)
            return false;
        if (track.componentTag < 0 || track.componentTag > 255)
            return false;
        if (track.type == "audio" && track.samplingRate > 768000)
            return false;
        if (track.type == "audio" && track.channels > 64)
            return false;
        return true;
    };

    long long sourceSize = -1;
    std::string magic;
    char binaryMagic[8] = {};
    ifs.read(binaryMagic, sizeof(binaryMagic));
    const bool isBinaryMap2 =
        ifs.gcount() == static_cast<std::streamsize>(sizeof(binaryMagic)) &&
        std::memcmp(binaryMagic, "MMTSMAP2", 8) == 0;
    const bool isBinaryMap3 =
        ifs.gcount() == static_cast<std::streamsize>(sizeof(binaryMagic)) &&
        std::memcmp(binaryMagic, "MMTSMAP3", 8) == 0;
    if (isBinaryMap2 || isBinaryMap3) {
        const uint32_t expectedVersion = isBinaryMap3 ? 3 : 2;
        uint32_t version = 0;
        uint32_t flags = 0;
        uint64_t binarySourceSize = 0;
        int64_t durationMs = 0;
        int64_t firstVideoPtsMs = -1;
        int64_t lastVideoPtsMs = -1;
        uint32_t trackCount = 0;
        uint32_t mptCount = 0;
        uint32_t rapCount = 0;
        uint32_t seekCount = 0;
        if (!ReadPod(ifs, version) || !ReadPod(ifs, flags) ||
            !ReadPod(ifs, binarySourceSize) || !ReadPod(ifs, durationMs) ||
            !ReadPod(ifs, firstVideoPtsMs) || !ReadPod(ifs, lastVideoPtsMs) ||
            !ReadPod(ifs, trackCount) || !ReadPod(ifs, mptCount) ||
            !ReadPod(ifs, rapCount) || !ReadPod(ifs, seekCount) ||
            version != expectedVersion) {
            LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, bad binary header: %s\n", mapPath.c_str());
            return;
        }
        if (flags != 0 ||
            binarySourceSize > static_cast<uint64_t>(LLONG_MAX) ||
            !IsValidMmtsMapTimeMs(durationMs) ||
            !IsValidMmtsMapPtsMs(firstVideoPtsMs) ||
            !IsValidMmtsMapPtsMs(lastVideoPtsMs) ||
            trackCount > kMaxMmtsMapTracks ||
            mptCount > kMaxMmtsMapMptChanges ||
            rapCount > kMaxMmtsMapPoints ||
            seekCount > kMaxMmtsMapPoints) {
            LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, invalid binary header values: %s tracks=%u mpt=%u rap=%u seek=%u flags=%u\n",
                   mapPath.c_str(), trackCount, mptCount, rapCount, seekCount, flags);
            return;
        }
        const uint64_t minBinarySize =
            64ULL +
            static_cast<uint64_t>(trackCount) * 20ULL +
            static_cast<uint64_t>(mptCount) * 20ULL +
            (static_cast<uint64_t>(rapCount) + static_cast<uint64_t>(seekCount)) * 16ULL;
        if (minBinarySize > static_cast<uint64_t>(mapSize)) {
            LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, binary table sizes exceed file size: %s min=%I64u size=%I64d\n",
                   mapPath.c_str(), minBinarySize, static_cast<long long>(mapSize));
            return;
        }

        sourceSize = static_cast<long long>(binarySourceSize);
        if (durationMs > 0)
            m_mapDuration = static_cast<REFERENCE_TIME>(durationMs) * 10000LL;
        if (firstVideoPtsMs >= 0)
            m_mapFirstVideoPts = static_cast<REFERENCE_TIME>(firstVideoPtsMs) * 10000LL;

        std::vector<SidecarMapTrack> binaryTracks;
        binaryTracks.reserve(trackCount);
        for (uint32_t i = 0; i < trackCount; ++i) {
            uint8_t type = 0;
            uint8_t trackFlags = 0;
            uint8_t audioMode = 0;
            uint8_t channels = 0;
            int32_t streamIndex = -1;
            uint16_t packetId = 0;
            int16_t componentTag = -1;
            uint32_t samplingRate = 0;
            uint32_t reserved2 = 0;
            if (!ReadPod(ifs, type) || !ReadPod(ifs, trackFlags)) {
                LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, truncated binary track table: %s\n", mapPath.c_str());
                return;
            }
            if (isBinaryMap3) {
                if (!ReadPod(ifs, audioMode) || !ReadPod(ifs, channels)) {
                    LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, truncated binary track table: %s\n", mapPath.c_str());
                    return;
                }
            } else {
                uint16_t reserved = 0;
                if (!ReadPod(ifs, reserved)) {
                    LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, truncated binary track table: %s\n", mapPath.c_str());
                    return;
                }
            }
            if (!ReadPod(ifs, streamIndex) || !ReadPod(ifs, packetId) ||
                !ReadPod(ifs, componentTag) || !ReadPod(ifs, samplingRate) ||
                !ReadPod(ifs, reserved2)) {
                LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, truncated binary track table: %s\n", mapPath.c_str());
                return;
            }

            SidecarMapTrack track;
            track.type = MmtsMapTrackTypeName(type);
            track.streamIndex = streamIndex;
            track.packetId = packetId;
            track.componentTag = componentTag;
            track.samplingRate = samplingRate;
            track.latm = (trackFlags & 1) != 0;
            track.audioMode = audioMode;
            track.channels = channels;
            if (!isValidTrack(track)) {
                LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, invalid binary track: %s index=%u type=%u streamIndex=%d packetId=0x%04X componentTag=%d\n",
                       mapPath.c_str(), i, type, streamIndex, packetId, componentTag);
                return;
            }
            binaryTracks.push_back(track);
            if ((track.type == "audio" || track.type == "subtitle") &&
                track.streamIndex >= 0 && track.packetId != 0) {
                m_sidecarMapTracks.push_back(track);
            }
        }

        for (uint32_t i = 0; i < mptCount; ++i) {
            int64_t timeMs = -1;
            uint64_t offset = 0;
            uint32_t mptTrackCount = 0;
            if (!ReadPod(ifs, timeMs) || !ReadPod(ifs, offset) || !ReadPod(ifs, mptTrackCount)) {
                LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, truncated binary mpt table: %s\n", mapPath.c_str());
                return;
            }
            if (!IsValidMmtsMapPtsMs(timeMs) ||
                !IsValidMmtsMapOffset(static_cast<long long>(offset), m_fileSize) ||
                mptTrackCount > kMaxMmtsMapMptTracks) {
                LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, invalid binary mpt: %s index=%u time=%I64d offset=%I64u tracks=%u\n",
                       mapPath.c_str(), i, timeMs, offset, mptTrackCount);
                return;
            }

            SidecarMapMptChange change;
            change.time = static_cast<REFERENCE_TIME>(timeMs) * 10000LL;
            change.offset = static_cast<long long>(offset);
            for (uint32_t j = 0; j < mptTrackCount; ++j) {
                uint32_t trackIndex = UINT32_MAX;
                if (!ReadPod(ifs, trackIndex)) {
                    LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, truncated binary mpt tracks: %s\n", mapPath.c_str());
                    return;
                }
                if (trackIndex >= binaryTracks.size()) {
                    LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, invalid binary mpt track reference: %s mpt=%u trackIndex=%u tracks=%zu\n",
                           mapPath.c_str(), i, trackIndex, binaryTracks.size());
                    return;
                }
                change.tracks.push_back(binaryTracks[trackIndex]);
            }
            if (change.offset >= 0 && !change.tracks.empty())
                m_sidecarMapMptChanges.push_back(change);
        }

        const std::streamsize mediaFileSize = m_fileSize;
        auto readPoint = [&ifs, mediaFileSize](SidecarMapPoint& point) {
            int64_t timeMs = -1;
            uint64_t offset = 0;
            if (!ReadPod(ifs, timeMs) || !ReadPod(ifs, offset))
                return false;
            point.time = static_cast<REFERENCE_TIME>(timeMs) * 10000LL;
            point.offset = static_cast<long long>(offset);
            return timeMs >= 0 && IsValidMmtsMapPtsMs(timeMs) &&
                   IsValidMmtsMapOffset(point.offset, mediaFileSize);
        };
        for (uint32_t i = 0; i < rapCount; ++i) {
            SidecarMapPoint point;
            if (!readPoint(point)) {
                LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, truncated or invalid binary rap table: %s\n", mapPath.c_str());
                return;
            }
            if (point.time >= 0 && point.offset >= 0)
                m_sidecarMapRapPoints.push_back(point);
        }
        for (uint32_t i = 0; i < seekCount; ++i) {
            SidecarMapPoint point;
            if (!readPoint(point)) {
                LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, truncated or invalid binary seek table: %s\n", mapPath.c_str());
                return;
            }
            if (point.time >= 0 && point.offset >= 0)
                m_sidecarMapSeekPoints.push_back(point);
        }
    } else {
        ifs.clear();
        ifs.seekg(0);
        std::getline(ifs, magic);
        if (magic != "MMTSMAP 1") {
        LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, bad magic: %s\n", mapPath.c_str());
        return;
        }

    std::string line;
    auto parseMptTrackList = [&isValidTrack](const std::string& value, const char* type) {
        std::vector<SidecarMapTrack> tracks;
        if (value.empty() || value == "-")
            return tracks;

        for (const auto& spec : SplitString(value, ',')) {
            const auto fields = SplitString(spec, ':');
            const bool isAudio = std::strcmp(type, "audio") == 0;
            if ((isAudio && fields.size() != 5 && fields.size() != 7) || (!isAudio && fields.size() != 3))
                continue;

            SidecarMapTrack track;
            track.type = type;
            track.streamIndex = static_cast<int>(ParseInt64Value(fields[0], -1));
            const long long packetId = ParseInt64Value(fields[1], 0);
            if (packetId < 0 || packetId > 0xFFFF)
                continue;
            track.packetId = static_cast<uint16_t>(packetId);
            track.componentTag = static_cast<int>(ParseInt64Value(fields[2], -1));
            if (isAudio) {
                track.samplingRate = static_cast<uint32_t>(ParseInt64Value(fields[3], 0));
                track.latm = ParseInt64Value(fields[4], 0) != 0;
                if (fields.size() >= 7) {
                    track.audioMode = static_cast<uint8_t>(ParseInt64Value(fields[5], 0));
                    track.channels = static_cast<uint16_t>(ParseInt64Value(fields[6], 0));
                }
            }
            if (isValidTrack(track))
                tracks.push_back(track);
        }
        return tracks;
    };

    uint32_t textLineCount = 0;
    while (std::getline(ifs, line)) {
        if (++textLineCount > kMaxMmtsMapTextLines || line.size() > kMaxMmtsMapTextLine) {
            LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, text sidecar is too large or has an oversized line: %s line=%u size=%zu\n",
                   mapPath.c_str(), textLineCount, line.size());
            return;
        }
        if (line.empty())
            continue;

        if (line.rfind("track ", 0) == 0) {
            auto values = ParseSpaceKeyValues(line.substr(6));
            SidecarMapTrack track;
            auto it = values.find("type");
            if (it != values.end())
                track.type = it->second;
            it = values.find("streamIndex");
            if (it != values.end())
                track.streamIndex = static_cast<int>(ParseInt64Value(it->second, -1));
            it = values.find("packetId");
            if (it != values.end()) {
                const long long packetId = ParseInt64Value(it->second, 0);
                if (packetId < 0 || packetId > 0xFFFF) {
                    LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, invalid text track packetId: %s value=%S\n",
                           mapPath.c_str(), it->second.c_str());
                    return;
                }
                track.packetId = static_cast<uint16_t>(packetId);
            }
            it = values.find("componentTag");
            if (it != values.end())
                track.componentTag = static_cast<int>(ParseInt64Value(it->second, -1));
            it = values.find("rate");
            if (it != values.end())
                track.samplingRate = static_cast<uint32_t>(ParseInt64Value(it->second, 0));
            it = values.find("latm");
            if (it != values.end())
                track.latm = ParseInt64Value(it->second, 0) != 0;
            it = values.find("audioMode");
            if (it != values.end())
                track.audioMode = static_cast<uint8_t>(ParseInt64Value(it->second, 0));
            it = values.find("channels");
            if (it != values.end())
                track.channels = static_cast<uint16_t>(ParseInt64Value(it->second, 0));

            if ((track.type == "audio" || track.type == "subtitle") &&
                track.streamIndex >= 0 && track.packetId != 0) {
                if (!isValidTrack(track)) {
                    LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, invalid text track: %s type=%S streamIndex=%d packetId=0x%04X componentTag=%d\n",
                           mapPath.c_str(), track.type.c_str(), track.streamIndex, track.packetId, track.componentTag);
                    return;
                }
                m_sidecarMapTracks.push_back(track);
            }
            continue;
        }

        if (line.rfind("mpt ", 0) == 0) {
            auto values = ParseSpaceKeyValues(line.substr(4));
            auto timeIt = values.find("time_ms");
            auto offsetIt = values.find("offset");
            if (timeIt != values.end() && offsetIt != values.end()) {
                const long long timeMs = ParseInt64Value(timeIt->second, -1);
                const long long offset = ParseInt64Value(offsetIt->second, -1);
                if (!IsValidMmtsMapPtsMs(timeMs) || !IsValidMmtsMapOffset(offset, m_fileSize)) {
                    LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, invalid text mpt: %s time=%I64d offset=%I64d\n",
                           mapPath.c_str(), timeMs, offset);
                    return;
                }
                SidecarMapMptChange change;
                change.time = static_cast<REFERENCE_TIME>(timeMs) * 10000LL;
                change.offset = offset;

                auto audioIt = values.find("audio");
                if (audioIt != values.end()) {
                    auto tracks = parseMptTrackList(audioIt->second, "audio");
                    change.tracks.insert(change.tracks.end(), tracks.begin(), tracks.end());
                }
                auto subtitleIt = values.find("subtitle");
                if (subtitleIt != values.end()) {
                    auto tracks = parseMptTrackList(subtitleIt->second, "subtitle");
                    change.tracks.insert(change.tracks.end(), tracks.begin(), tracks.end());
                }
                if (change.tracks.size() > kMaxMmtsMapMptTracks) {
                    LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, text mpt has too many tracks: %s tracks=%zu\n",
                           mapPath.c_str(), change.tracks.size());
                    return;
                }

                if (change.offset >= 0 && !change.tracks.empty())
                    m_sidecarMapMptChanges.push_back(change);
            }
            continue;
        }

        if (line.rfind("rap ", 0) == 0 || line.rfind("seek ", 0) == 0) {
            const bool isRap = line.rfind("rap ", 0) == 0;
            auto values = ParseSpaceKeyValues(line.substr(isRap ? 4 : 5));
            auto timeIt = values.find("time_ms");
            auto offsetIt = values.find("offset");
            if (timeIt != values.end() && offsetIt != values.end()) {
                const long long timeMs = ParseInt64Value(timeIt->second, -1);
                const long long offset = ParseInt64Value(offsetIt->second, -1);
                SidecarMapPoint point;
                point.time = static_cast<REFERENCE_TIME>(timeMs) * 10000LL;
                point.offset = offset;
                if (timeMs >= 0 && IsValidMmtsMapPtsMs(timeMs) &&
                    IsValidMmtsMapOffset(point.offset, m_fileSize)) {
                    if (isRap)
                        m_sidecarMapRapPoints.push_back(point);
                    else
                        m_sidecarMapSeekPoints.push_back(point);
                }
            }
            continue;
        }

        std::string key;
        std::string value;
        if (!ParseKeyValueLine(line, key, value))
            continue;
        if (key == "source_size") {
            sourceSize = ParseInt64Value(value, -1);
        } else if (key == "duration_ms") {
            long long durationMs = ParseInt64Value(value, 0);
            if (durationMs > 0 && IsValidMmtsMapTimeMs(durationMs))
                m_mapDuration = static_cast<REFERENCE_TIME>(durationMs) * 10000LL;
        } else if (key == "first_video_pts_ms") {
            long long firstMs = ParseInt64Value(value, -1);
            if (firstMs >= 0 && IsValidMmtsMapPtsMs(firstMs))
                m_mapFirstVideoPts = static_cast<REFERENCE_TIME>(firstMs) * 10000LL;
        }
    }
    }

    if (sourceSize >= 0 && sourceSize != static_cast<long long>(m_fileSize)) {
        LogMsg(L"MMT/TLV Splitter: mmtsmap ignored, size mismatch: map=%I64d file=%I64d\n",
               sourceSize, static_cast<long long>(m_fileSize));
        m_sidecarMapTracks.clear();
        m_sidecarMapMptChanges.clear();
        m_sidecarMapRapPoints.clear();
        m_sidecarMapSeekPoints.clear();
        m_mapDuration = 0;
        m_mapFirstVideoPts = -1;
        return;
    }

    if (m_mapFirstVideoPts >= 0) {
        for (auto& change : m_sidecarMapMptChanges) {
            if (change.time >= 0)
                change.time -= m_mapFirstVideoPts;
            else
                change.time = 0;
        }
        for (auto& point : m_sidecarMapRapPoints)
            point.time -= m_mapFirstVideoPts;
        for (auto& point : m_sidecarMapSeekPoints)
            point.time -= m_mapFirstVideoPts;
        m_sidecarMapRapPoints.erase(std::remove_if(m_sidecarMapRapPoints.begin(), m_sidecarMapRapPoints.end(),
            [](const SidecarMapPoint& point) { return point.time < 0; }), m_sidecarMapRapPoints.end());
        m_sidecarMapSeekPoints.erase(std::remove_if(m_sidecarMapSeekPoints.begin(), m_sidecarMapSeekPoints.end(),
            [](const SidecarMapPoint& point) { return point.time < 0; }), m_sidecarMapSeekPoints.end());
    }

    auto byTime = [](const SidecarMapPoint& a, const SidecarMapPoint& b) {
        return a.time < b.time;
    };
    std::sort(m_sidecarMapRapPoints.begin(), m_sidecarMapRapPoints.end(), byTime);
    std::sort(m_sidecarMapSeekPoints.begin(), m_sidecarMapSeekPoints.end(), byTime);
    std::sort(m_sidecarMapMptChanges.begin(), m_sidecarMapMptChanges.end(),
        [](const SidecarMapMptChange& a, const SidecarMapMptChange& b) {
            return a.time != b.time ? a.time < b.time : a.offset < b.offset;
        });

    m_hasSidecarMap = !m_sidecarMapTracks.empty() || !m_sidecarMapMptChanges.empty() ||
                      m_mapDuration > 0 ||
                      !m_sidecarMapRapPoints.empty() || !m_sidecarMapSeekPoints.empty();
    if (m_hasSidecarMap) {
        LogMsg(L"MMT/TLV Splitter: mmtsmap loaded: %s tracks=%zu mpt=%zu duration=%I64d ms rap=%zu seek=%zu firstVideoPts=%I64d ms\n",
               mapPath.c_str(), m_sidecarMapTracks.size(), m_sidecarMapMptChanges.size(), m_mapDuration / 10000,
               m_sidecarMapRapPoints.size(), m_sidecarMapSeekPoints.size(),
               m_mapFirstVideoPts / 10000);
    }
}

void CMmtTlvSplitter::ApplySidecarMap()
{
    if (!m_hasSidecarMap)
        return;

    if (m_mapDuration > m_duration) {
        LogMsg(L"MMT/TLV Splitter: mmtsmap duration applied: prescan=%I64d ms map=%I64d ms\n",
               m_duration / 10000, m_mapDuration / 10000);
        m_duration = m_mapDuration;
    }

    ApplySidecarMapTracks(0);
}

const CMmtTlvSplitter::SidecarMapMptChange* CMmtTlvSplitter::FindSidecarMapMptChange(REFERENCE_TIME sourceTarget) const
{
    if (m_sidecarMapMptChanges.empty())
        return nullptr;

    const SidecarMapMptChange* best = nullptr;
    for (const auto& change : m_sidecarMapMptChanges) {
        if (change.time > sourceTarget)
            break;
        best = &change;
    }
    return best ? best : &m_sidecarMapMptChanges.front();
}

std::wstring CMmtTlvSplitter::AudioTimelineLabel(const CFilterDemuxerHandler::AudioStreamInfo& info) const
{
    std::vector<std::wstring> labels;
    auto pushLabel = [&labels](bool latm, uint16_t channels) {
        CFilterDemuxerHandler::AudioStreamInfo labelInfo;
        labelInfo.latm = latm;
        labelInfo.channels = channels;
        const std::wstring label = AudioSourceLabel(labelInfo);
        if (labels.empty() || labels.back() != label)
            labels.push_back(label);
    };

    for (const auto& change : m_sidecarMapMptChanges) {
        for (const auto& track : change.tracks) {
            if (track.type != "audio")
                continue;
            if (track.streamIndex != info.streamIndex &&
                !(track.packetId == info.packetId && track.componentTag == info.componentTag)) {
                continue;
            }
            const uint16_t channels = track.channels > 0 ? track.channels : (track.latm ? 24 : 2);
            pushLabel(track.latm, channels);
            break;
        }
    }

    if (labels.empty())
        return AudioSourceLabel(info);

    std::wstring label = labels.front();
    for (size_t i = 1; i < labels.size(); ++i) {
        label += L" -> ";
        label += labels[i];
    }
    return label;
}

void CMmtTlvSplitter::ApplySidecarMapTracks(REFERENCE_TIME sourceTarget)
{
    if (!m_hasSidecarMap)
        return;

    auto mapTrackChannels = [](const SidecarMapTrack& track) -> uint16_t {
        return track.channels > 0 ? track.channels : (track.latm ? 24 : 2);
    };
    auto isBetterMapAudio = [&](const SidecarMapTrack& track,
                                const CFilterDemuxerHandler::AudioStreamInfo& info) {
        const uint16_t trackChannels = mapTrackChannels(track);
        if (trackChannels > info.channels)
            return true;
        if (trackChannels == info.channels && track.latm && !info.latm)
            return true;
        if (info.samplingRate == 0 && track.samplingRate > 0)
            return true;
        return false;
    };

    auto audioStreams = m_handler.getAudioStreams();
    auto subtitleStreams = m_handler.getSubtitleStreams();
    size_t addedAudio = 0;
    size_t updatedAudio = 0;
    size_t addedSubtitle = 0;

    std::vector<SidecarMapTrack> mapTracks = m_sidecarMapTracks;
    for (const auto& change : m_sidecarMapMptChanges) {
        mapTracks.insert(mapTracks.end(), change.tracks.begin(), change.tracks.end());
    }

    for (const auto& track : mapTracks) {
        if (track.type == "audio") {
            auto it = std::find_if(audioStreams.begin(), audioStreams.end(),
                [&track](const CFilterDemuxerHandler::AudioStreamInfo& info) {
                    return info.streamIndex == track.streamIndex ||
                           (info.packetId == track.packetId && info.componentTag == track.componentTag);
                });
            if (it != audioStreams.end()) {
                if (isBetterMapAudio(track, *it)) {
                    it->streamIndex = track.streamIndex;
                    it->packetId = track.packetId;
                    it->componentTag = track.componentTag;
                    it->samplingRate = track.samplingRate;
                    it->channels = mapTrackChannels(track);
                    it->latm = track.latm;
                    ++updatedAudio;
                }
                continue;
            }

            CFilterDemuxerHandler::AudioStreamInfo info;
            info.streamIndex = track.streamIndex;
            info.packetId = track.packetId;
            info.componentTag = track.componentTag;
            info.samplingRate = track.samplingRate;
            info.channels = mapTrackChannels(track);
            info.latm = track.latm;
            audioStreams.push_back(info);
            ++addedAudio;
        } else if (track.type == "subtitle") {
            auto it = std::find_if(subtitleStreams.begin(), subtitleStreams.end(),
                [&track](const CFilterDemuxerHandler::SubtitleStreamInfo& info) {
                    return info.streamIndex == track.streamIndex ||
                           (info.packetId == track.packetId && info.componentTag == track.componentTag);
                });
            if (it != subtitleStreams.end())
                continue;

            CFilterDemuxerHandler::SubtitleStreamInfo info;
            info.streamIndex = track.streamIndex;
            info.packetId = track.packetId;
            info.componentTag = track.componentTag;
            info.hasData = false;
            subtitleStreams.push_back(info);
            ++addedSubtitle;
        }
    }

    if (addedAudio > 0 || updatedAudio > 0) {
        m_handler.setKnownAudioStreams(audioStreams);
        m_handler.setRequireAdtsConvertibleAudio(true);
        m_handler.setAudioStreamListLocked(true);
    }
    if (addedSubtitle > 0) {
        m_handler.setKnownSubtitleStreams(subtitleStreams);
    }

    if (addedAudio > 0 || updatedAudio > 0 || addedSubtitle > 0) {
        LogMsg(L"MMT/TLV Splitter: mmtsmap tracks merged: audio+%zu audio~%zu subtitle+%zu totalAudio=%zu totalSubtitle=%zu\n",
               addedAudio, updatedAudio, addedSubtitle, audioStreams.size(), subtitleStreams.size());
    }

    const auto* active = FindSidecarMapMptChange(sourceTarget);
    if (active) {
        size_t audioCount = 0;
        size_t subtitleCount = 0;
        for (const auto& track : active->tracks) {
            if (track.type == "audio")
                ++audioCount;
            else if (track.type == "subtitle")
                ++subtitleCount;
        }
        LogMsg(L"MMT/TLV Splitter: mmtsmap active mpt: source=%I64d ms mpt=%I64d ms offset=%I64d audio=%zu subtitle=%zu\n",
               sourceTarget / 10000,
               active->time / 10000,
               active->offset,
               audioCount,
               subtitleCount);
    }
}

bool CMmtTlvSplitter::FindSidecarMapSeekOffset(REFERENCE_TIME sourceTarget, long long& byteOffset) const
{
    if (!m_hasSidecarMap || sourceTarget <= 0)
        return false;

    auto findBest = [&](const std::vector<SidecarMapPoint>& points, SidecarMapPoint& best) {
        bool found = false;
        for (const auto& point : points) {
            if (point.time > sourceTarget)
                break;
            if (point.offset >= 0 && point.offset < static_cast<long long>(m_fileSize)) {
                best = point;
                found = true;
            }
        }
        return found;
    };

    SidecarMapPoint best;
    if (!findBest(m_sidecarMapRapPoints, best) &&
        !findBest(m_sidecarMapSeekPoints, best)) {
        return false;
    }

    byteOffset = best.offset;
    return true;
}

REFERENCE_TIME CMmtTlvSplitter::EditTotalDuration() const
{
    REFERENCE_TIME total = 0;
    for (const auto& s : m_editSegments)
        if (s.end > s.start)
            total += s.end - s.start;
    return total;
}

REFERENCE_TIME CMmtTlvSplitter::SegmentBase(int index) const
{
    REFERENCE_TIME base = 0;
    for (int i = 0; i < index && i < static_cast<int>(m_editSegments.size()); ++i)
        base += m_editSegments[i].end - m_editSegments[i].start;
    return base;
}

int CMmtTlvSplitter::SegmentAtProgram(REFERENCE_TIME programRT, REFERENCE_TIME& srcRelOut) const
{
    REFERENCE_TIME base = 0;
    for (int i = 0; i < static_cast<int>(m_editSegments.size()); ++i) {
        const REFERENCE_TIME dur = m_editSegments[i].end - m_editSegments[i].start;
        if (programRT < base + dur || i == static_cast<int>(m_editSegments.size()) - 1) {
            const REFERENCE_TIME within = programRT > base ? programRT - base : 0;
            srcRelOut = m_editSegments[i].start + (within < dur ? within : dur);
            return i;
        }
        base += dur;
    }
    srcRelOut = 0;
    return 0;
}

bool CMmtTlvSplitter::EditMapPts(REFERENCE_TIME absPts, REFERENCE_TIME& programOut)
{
    if (absPts < 0 || m_origFirstPts < 0) {
        programOut = absPts;
        return true;
    }
    if (m_curSegment < 0 || m_curSegment >= static_cast<int>(m_editSegments.size())) {
        programOut = absPts - m_origFirstPts;
        return true;
    }
    const REFERENCE_TIME srcRel = absPts - m_origFirstPts;
    const EditSeg& seg = m_editSegments[m_curSegment];
    if (srcRel >= seg.end) {
        const int next = m_curSegment + 1;
        m_pendingSegmentJump.store(next < static_cast<int>(m_editSegments.size()) ? next : -2,
                                   std::memory_order_release);
        return false; // past this segment -> drop and trigger jump/EOS
    }
    if (srcRel < seg.start)
        return false; // before this segment (pre-roll / gap) -> drop
    programOut = m_curSegmentBase + (srcRel - seg.start);
    return true;
}

void CMmtTlvSplitter::ApplySidecarIndex()
{
    m_sourceDuration = m_duration;
    m_origFirstPts = m_firstPts;

    // Multi-segment EDL: the program is the concatenation of the cut segments.
    if (IsMultiSegment()) {
        if (m_firstPts < 0 || m_sourceDuration <= 0) {
            m_editSegments.clear();
            m_hasSidecarIndex = false;
            return;
        }
        // Clamp each segment end to the available source duration; drop empties.
        std::vector<EditSeg> kept;
        for (auto seg : m_editSegments) {
            if (seg.start >= m_sourceDuration)
                continue;
            if (seg.end > m_sourceDuration)
                seg.end = m_sourceDuration;
            if (seg.end > seg.start)
                kept.push_back(seg);
        }
        m_editSegments = kept;
        if (m_editSegments.size() <= 1) {
            // Collapsed to one (or zero) segment: fall through to the single path.
            if (m_editSegments.empty()) {
                m_hasSidecarIndex = false;
                m_virtualStart = 0;
                m_virtualEnd = 0;
                return;
            }
            m_virtualStart = m_editSegments.front().start;
            m_virtualEnd = m_editSegments.front().end;
        } else {
            m_duration = EditTotalDuration();
            m_curSegment = 0;
            m_curSegmentBase = 0;
            m_pendingSegmentJump.store(-1, std::memory_order_release);
            LogMsg(L"MMT/TLV Splitter: EDL applied: segments=%zu programDuration=%I64d ms sourceDuration=%I64d ms\n",
                   m_editSegments.size(), m_duration / 10000, m_sourceDuration / 10000);
            return;
        }
    }

    if (!m_hasSidecarIndex || (m_virtualStart <= 0 && m_virtualEnd <= 0))
        return;

    if (m_duration <= m_virtualStart || m_firstPts < 0) {
        LogMsg(L"MMT/TLV Splitter: sidecar disabled after prescan, start=%I64d ms end=%I64d ms duration=%I64d ms firstPts=%I64d\n",
               m_virtualStart / 10000, m_virtualEnd / 10000, m_duration / 10000, m_firstPts / 10000);
        m_virtualStart = 0;
        m_virtualEnd = 0;
        m_hasSidecarIndex = false;
        m_sourceDuration = m_duration;
        return;
    }

    m_firstPts += m_virtualStart;
    m_duration -= m_virtualStart;
    if (m_virtualEnd > m_virtualStart) {
        const REFERENCE_TIME editDuration = m_virtualEnd - m_virtualStart;
        if (editDuration < m_duration)
            m_duration = editDuration;
    }
    LogMsg(L"MMT/TLV Splitter: sidecar applied: virtualStart=%I64d ms virtualEnd=%I64d ms virtualDuration=%I64d ms\n",
           m_virtualStart / 10000, m_virtualEnd / 10000, m_duration / 10000);
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
    int  accumStreamIndex = -1;
    long long minPts = -1;

    handler.setVideoCallback(
        [&](int streamIndex, bool, long long pts, long long, bool isFirst, bool isLast,
            const uint8_t* data, size_t size)
        {
            if (phase1Done) return;
            // The handler forwards one video asset only, but the very first
            // fragments can arrive before the asset list settles. Never mix two
            // assets into the buffer the parameter sets are parsed from.
            if (streamIndex != accumStreamIndex) {
                accumVideo.clear();
                accumStreamIndex = streamIndex;
            }
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
        if (!playableAudioStreams.empty()) {
            m_handler.setKnownAudioStreams(playableAudioStreams);
            m_handler.setRequireAdtsConvertibleAudio(true);
        } else if (!discoveredAudioStreams.empty()) {
            m_handler.setKnownAudioStreams(discoveredAudioStreams);
            m_handler.setRequireAdtsConvertibleAudio(true);
            LogMsg(L"MMT/TLV Splitter: audio streams found before ADTS/LATM playback was confirmed; creating candidate audio pins\n");
        } else {
            m_handler.setKnownAudioStreams(discoveredAudioStreams);
            m_handler.setRequireAdtsConvertibleAudio(false);
        }
        m_handler.setAudioStreamListLocked(true);
    }
    m_handler.setKnownVideoStreams(handler.getVideoStreams());
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

    {
        const auto videoStreams = handler.getVideoStreams();
        const int selected = handler.getSelectedVideoStreamIndex();
        LogMsg(L"MMT/TLV Splitter: PreScanFile found %zu video asset(s), selected streamIndex=%d\n",
               videoStreams.size(), selected);
        for (const auto& info : videoStreams) {
            LogMsg(L"MMT/TLV Splitter: video asset streamIndex=%d packetId=0x%04X componentTag=%d %dx%d%s\n",
                   info.streamIndex, info.packetId, info.componentTag, info.width, info.height,
                   info.streamIndex == selected ? L" (selected)" : L"");
        }

        // The extradata parse is the authority on the coded size, but if it did
        // not produce one, the selected asset's video component descriptor beats
        // the hard-coded 4K default - a 1080p asset would otherwise be announced
        // as 3840x2160.
        if (m_hevcExtradata.empty()) {
            for (const auto& info : videoStreams) {
                if (info.streamIndex == selected && info.width > 0 && info.height > 0) {
                    m_videoWidth = info.width;
                    m_videoHeight = info.height;
                    LogMsg(L"MMT/TLV Splitter: PreScanFile using descriptor video size %dx%d\n",
                           m_videoWidth, m_videoHeight);
                    break;
                }
            }
        }
    }

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

static std::wstring AvErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    const int len = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
    if (len <= 1)
        return L"unknown";
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, out.data(), len);
    return out;
}

struct CMmtTlvSplitter::LatmPcmDecoder {
    struct PcmFrame {
        std::vector<uint8_t> data;
        REFERENCE_TIME pts = -1;
        REFERENCE_TIME duration = 0;
    };

    explicit LatmPcmDecoder(int streamIndex)
        : streamIndex(streamIndex)
    {
    }

    int StreamIndex() const { return streamIndex; }

    ~LatmPcmDecoder()
    {
        av_channel_layout_uninit(&lastInLayout);
        if (swr)
            swr_free(&swr);
        if (frame)
            av_frame_free(&frame);
        if (packet)
            av_packet_free(&packet);
        if (latmCtx)
            avcodec_free_context(&latmCtx);
        if (adtsCtx)
            avcodec_free_context(&adtsCtx);
    }

    bool EnsureOpen(AVCodecID codecId, AVCodecContext*& targetCtx, const wchar_t* codecName)
    {
        if (targetCtx)
            return true;

        const AVCodec* codec = avcodec_find_decoder(codecId);
        if (!codec) {
            LogMsg(L"MMT/TLV LATM PCM decoder: %s decoder not found for streamIndex=%d\n",
                   codecName, streamIndex);
            return false;
        }

        targetCtx = avcodec_alloc_context3(codec);
        if (!packet)
            packet = av_packet_alloc();
        if (!frame)
            frame = av_frame_alloc();
        if (!targetCtx || !packet || !frame) {
            LogMsg(L"MMT/TLV LATM PCM decoder: allocation failed for streamIndex=%d\n",
                   streamIndex);
            return false;
        }

        targetCtx->pkt_timebase = AVRational{1, 10000000};
        const int ret = avcodec_open2(targetCtx, codec, nullptr);
        if (ret < 0) {
            LogMsg(L"MMT/TLV LATM PCM decoder: avcodec_open2 failed codec=%s streamIndex=%d err=%s\n",
                   codecName, streamIndex, AvErrorString(ret).c_str());
            return false;
        }

        LogMsg(L"MMT/TLV LATM PCM decoder: opened %s decoder for streamIndex=%d\n",
               codecName, streamIndex);
        return true;
    }

    void Reset(bool preserveLatmContext = false)
    {
        loasBuffer.clear();
        adtsBuffer.clear();
        pendingPts = -1;
        adtsPendingPts = -1;
        nextPts = -1;
        if (latmCtx && !preserveLatmContext)
            avcodec_flush_buffers(latmCtx);
        if (adtsCtx)
            avcodec_flush_buffers(adtsCtx);
        if (swr && !preserveLatmContext)
            swr_close(swr);
        currentInputKind = InputKind::Unknown;
        LogDetail(L"MMT/TLV LATM PCM decoder: reset streamIndex=%d preserveLatm=%d\n",
                  streamIndex, preserveLatmContext ? 1 : 0);
    }

    bool Decode(bool, REFERENCE_TIME pts, bool first, bool last,
                const uint8_t* data, size_t size, std::vector<PcmFrame>& outFrames)
    {
        (void)first;
        (void)last;
        outFrames.clear();
        if (!data || size == 0)
            return true;

        static volatile LONG s_decodeInputs = 0;
        const LONG inputNo = InterlockedIncrement(&s_decodeInputs);
        if (inputNo <= 80 || (inputNo % 200) == 0) {
            LogMsg(L"MMT/TLV LATM PCM decoder: input #%ld streamIndex=%d size=%zu pts=%I64d ms first=%d last=%d firstBytes=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                   inputNo, streamIndex, size, pts / 10000,
                   first ? 1 : 0, last ? 1 : 0,
                   size > 0 ? data[0] : 0,
                   size > 1 ? data[1] : 0,
                   size > 2 ? data[2] : 0,
                   size > 3 ? data[3] : 0,
                   size > 4 ? data[4] : 0,
                   size > 5 ? data[5] : 0,
                   size > 6 ? data[6] : 0,
                   size > 7 ? data[7] : 0);
        }

        if (IsAdtsFrame(data, size)) {
            loasBuffer.clear();
            pendingPts = -1;
            if (currentInputKind != InputKind::Adts) {
                currentInputKind = InputKind::Adts;
                adtsBuffer.clear();
                adtsPendingPts = pts;
                nextPts = pts;
                LogMsg(L"MMT/TLV LATM PCM decoder: input switched to ADTS streamIndex=%d pts=%I64d ms\n",
                       streamIndex, pts / 10000);
            }
            if (!EnsureOpen(AV_CODEC_ID_AAC, adtsCtx, L"AAC/ADTS"))
                return false;
            return DecodeAdts(pts, data, size, outFrames);
        }

        if (!EnsureOpen(AV_CODEC_ID_AAC_LATM, latmCtx, L"AAC_LATM"))
            return false;
        if (currentInputKind != InputKind::Latm) {
            currentInputKind = InputKind::Latm;
            loasBuffer.clear();
            pendingPts = pts;
            nextPts = pts;
            LogMsg(L"MMT/TLV LATM PCM decoder: input switched to LATM streamIndex=%d pts=%I64d ms\n",
                   streamIndex, pts / 10000);
        }

        if (loasBuffer.empty())
            pendingPts = pts;
        loasBuffer.insert(loasBuffer.end(), data, data + size);

        bool ok = true;
        while (true) {
            const size_t sync = FindLoasSync();
            if (sync == (std::numeric_limits<size_t>::max)()) {
                TrimUnsyncedBuffer();
                break;
            }
            if (sync > 0) {
                if (droppedSyncBytes < 20) {
                    LogMsg(L"MMT/TLV LATM PCM decoder: dropped %zu bytes before LOAS sync streamIndex=%d\n",
                           sync, streamIndex);
                }
                ++droppedSyncBytes;
                loasBuffer.erase(loasBuffer.begin(), loasBuffer.begin() + sync);
                pendingPts = pts;
            }
            if (loasBuffer.size() < 3)
                break;

            const size_t payloadSize =
                (static_cast<size_t>(loasBuffer[1] & 0x1F) << 8) | loasBuffer[2];
            const size_t packetSize = payloadSize + 3;
            if (payloadSize == 0 || payloadSize > 8191) {
                if (badHeaderCount < 20) {
                    LogMsg(L"MMT/TLV LATM PCM decoder: invalid LOAS payload length streamIndex=%d length=%zu firstBytes=%02X %02X %02X\n",
                           streamIndex, payloadSize, loasBuffer[0], loasBuffer[1], loasBuffer[2]);
                }
                ++badHeaderCount;
                loasBuffer.erase(loasBuffer.begin());
                pendingPts = pts;
                continue;
            }
            if (loasBuffer.size() < packetSize)
                break;

            const REFERENCE_TIME packetPts = pendingPts;
            ok = DecodePacket(latmCtx, loasBuffer.data(), packetSize, packetPts, outFrames) && ok;
            loasBuffer.erase(loasBuffer.begin(), loasBuffer.begin() + packetSize);
            pendingPts = nextPts >= 0 ? nextPts : pts;
        }

        if (loasBuffer.size() > 256 * 1024) {
            LogMsg(L"MMT/TLV LATM PCM decoder: clearing oversized LOAS buffer streamIndex=%d size=%zu\n",
                   streamIndex, loasBuffer.size());
            loasBuffer.clear();
            pendingPts = -1;
            return false;
        }
        return ok;
    }

private:
    static bool IsAdtsFrame(const uint8_t* data, size_t size)
    {
        return size >= 7 && data[0] == 0xFF && (data[1] & 0xF0) == 0xF0;
    }

    static size_t AdtsFrameSize(const uint8_t* data, size_t size)
    {
        if (!IsAdtsFrame(data, size))
            return 0;
        return (static_cast<size_t>(data[3] & 0x03) << 11) |
               (static_cast<size_t>(data[4]) << 3) |
               ((data[5] & 0xE0) >> 5);
    }

    bool DecodeAdts(REFERENCE_TIME pts, const uint8_t* data, size_t size,
                    std::vector<PcmFrame>& outFrames)
    {
        if (adtsBuffer.empty())
            adtsPendingPts = pts;
        adtsBuffer.insert(adtsBuffer.end(), data, data + size);

        bool ok = true;
        while (true) {
            size_t sync = (std::numeric_limits<size_t>::max)();
            for (size_t i = 0; i + 1 < adtsBuffer.size(); ++i) {
                if (adtsBuffer[i] == 0xFF && (adtsBuffer[i + 1] & 0xF0) == 0xF0) {
                    sync = i;
                    break;
                }
            }
            if (sync == (std::numeric_limits<size_t>::max)()) {
                if (adtsBuffer.size() > 1) {
                    const uint8_t last = adtsBuffer.back();
                    adtsBuffer.clear();
                    adtsBuffer.push_back(last);
                }
                break;
            }
            if (sync > 0) {
                adtsBuffer.erase(adtsBuffer.begin(), adtsBuffer.begin() + sync);
                adtsPendingPts = pts;
            }
            if (adtsBuffer.size() < 7)
                break;

            const size_t frameSize = AdtsFrameSize(adtsBuffer.data(), adtsBuffer.size());
            if (frameSize < 7 || frameSize > 8191) {
                if (badAdtsHeaderCount < 20) {
                    LogMsg(L"MMT/TLV LATM PCM decoder: invalid ADTS frame length streamIndex=%d length=%zu firstBytes=%02X %02X %02X %02X %02X %02X %02X\n",
                           streamIndex, frameSize,
                           adtsBuffer[0], adtsBuffer[1], adtsBuffer[2], adtsBuffer[3],
                           adtsBuffer[4], adtsBuffer[5], adtsBuffer[6]);
                }
                ++badAdtsHeaderCount;
                adtsBuffer.erase(adtsBuffer.begin());
                adtsPendingPts = pts;
                continue;
            }
            if (adtsBuffer.size() < frameSize)
                break;

            const REFERENCE_TIME packetPts = adtsPendingPts;
            const size_t beforeFrames = outFrames.size();
            const bool decoded = DecodePacket(adtsCtx, adtsBuffer.data(), frameSize, packetPts, outFrames);
            adtsBuffer.erase(adtsBuffer.begin(), adtsBuffer.begin() + frameSize);
            if (outFrames.size() == beforeFrames && packetPts >= 0) {
                const REFERENCE_TIME fallbackDuration = 1024LL * 10000000LL / kOutputSampleRate;
                PcmFrame silence;
                silence.pts = packetPts;
                silence.duration = fallbackDuration;
                silence.data.resize(1024 * kOutputChannels * sizeof(int16_t), 0);
                outFrames.push_back(std::move(silence));
                nextPts = packetPts + fallbackDuration;
            }
            ok = decoded && ok;
            adtsPendingPts = nextPts >= 0 ? nextPts : pts;
        }

        if (adtsBuffer.size() > 256 * 1024) {
            LogMsg(L"MMT/TLV LATM PCM decoder: clearing oversized ADTS buffer streamIndex=%d size=%zu\n",
                   streamIndex, adtsBuffer.size());
            adtsBuffer.clear();
            adtsPendingPts = -1;
            return false;
        }
        return ok;
    }

    size_t FindLoasSync() const
    {
        for (size_t i = 0; i + 1 < loasBuffer.size(); ++i) {
            if (loasBuffer[i] == 0x56 && (loasBuffer[i + 1] & 0xE0) == 0xE0)
                return i;
        }
        return (std::numeric_limits<size_t>::max)();
    }

    void TrimUnsyncedBuffer()
    {
        if (loasBuffer.size() <= 1)
            return;
        const uint8_t last = loasBuffer.back();
        if (droppedSyncBytes < 20) {
            LogMsg(L"MMT/TLV LATM PCM decoder: waiting for LOAS sync, dropped %zu bytes streamIndex=%d\n",
                   loasBuffer.size() - 1, streamIndex);
        }
        ++droppedSyncBytes;
        loasBuffer.clear();
        loasBuffer.push_back(last);
    }

    bool DecodePacket(AVCodecContext* decodeCtx, const uint8_t* packetData, size_t packetSize, REFERENCE_TIME packetPts,
                      std::vector<PcmFrame>& outFrames)
    {
        if (!decodeCtx || !packetData || packetSize == 0)
            return true;

        av_packet_unref(packet);
        packet->data = const_cast<uint8_t*>(packetData);
        packet->size = static_cast<int>(packetSize);
        packet->pts = packetPts;
        packet->dts = packetPts;

        if (packetPts >= 0)
            nextPts = packetPts;

        int ret = avcodec_send_packet(decodeCtx, packet);
        packet->data = nullptr;
        packet->size = 0;
        av_packet_unref(packet);
        if (ret < 0) {
            LogMsg(L"MMT/TLV LATM PCM decoder: send failed streamIndex=%d size=%zu pts=%I64d ms firstBytes=%02X %02X %02X err=%s\n",
                   streamIndex, packetSize, packetPts / 10000,
                   packetSize > 0 ? packetData[0] : 0,
                   packetSize > 1 ? packetData[1] : 0,
                   packetSize > 2 ? packetData[2] : 0,
                   AvErrorString(ret).c_str());
            return false;
        }

        while (true) {
            ret = avcodec_receive_frame(decodeCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                LogMsg(L"MMT/TLV LATM PCM decoder: receive failed streamIndex=%d err=%s\n",
                       streamIndex, AvErrorString(ret).c_str());
                return false;
            }

            PcmFrame pcm;
            if (ConvertFrame(frame, pcm)) {
                outFrames.push_back(std::move(pcm));
                static volatile LONG s_outputFrames = 0;
                const LONG outNo = InterlockedIncrement(&s_outputFrames);
                if (outNo <= 40 || (outNo % 100) == 0) {
                    const auto& last = outFrames.back();
                    LogMsg(L"MMT/TLV LATM PCM decoder: output #%ld streamIndex=%d bytes=%zu pts=%I64d ms duration=%I64d ms\n",
                           outNo, streamIndex, last.data.size(), last.pts / 10000, last.duration / 10000);
                }
            }
            av_frame_unref(frame);
        }
        return true;
    }

    bool EnsureResampler(const AVFrame* in)
    {
        if (!in || in->nb_samples <= 0)
            return false;

        AVChannelLayout inLayout{};
        if (in->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&inLayout, &in->ch_layout);
        } else {
            av_channel_layout_default(&inLayout, 2);
        }

        const bool unchanged = swr &&
            inRate == in->sample_rate &&
            inFormat == static_cast<AVSampleFormat>(in->format) &&
            av_channel_layout_compare(&inLayout, &lastInLayout) == 0;
        if (unchanged) {
            av_channel_layout_uninit(&inLayout);
            return true;
        }

        if (swr)
            swr_free(&swr);
        av_channel_layout_uninit(&lastInLayout);
        av_channel_layout_copy(&lastInLayout, &inLayout);
        inRate = in->sample_rate;
        inFormat = static_cast<AVSampleFormat>(in->format);

        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        int ret = swr_alloc_set_opts2(&swr,
                                      &outLayout,
                                      AV_SAMPLE_FMT_S16,
                                      kOutputSampleRate,
                                      &inLayout,
                                      inFormat,
                                      inRate,
                                      0,
                                      nullptr);
        av_channel_layout_uninit(&inLayout);
        if (ret < 0 || !swr) {
            LogMsg(L"MMT/TLV LATM PCM decoder: swr_alloc failed streamIndex=%d err=%s\n",
                   streamIndex, AvErrorString(ret).c_str());
            return false;
        }

        ret = swr_init(swr);
        if (ret < 0) {
            LogMsg(L"MMT/TLV LATM PCM decoder: swr_init failed streamIndex=%d err=%s\n",
                   streamIndex, AvErrorString(ret).c_str());
            swr_free(&swr);
            return false;
        }

        LogMsg(L"MMT/TLV LATM PCM decoder: resampler configured streamIndex=%d inputChannels=%d inputRate=%d inputFormat=%d output=stereo s16 %dHz\n",
               streamIndex, lastInLayout.nb_channels, inRate, static_cast<int>(inFormat), kOutputSampleRate);
        return true;
    }

    bool ConvertFrame(const AVFrame* in, PcmFrame& pcm)
    {
        if (!EnsureResampler(in))
            return false;

        const int outSamples = swr_get_out_samples(swr, in->nb_samples);
        if (outSamples <= 0)
            return false;

        const int outBytes = av_samples_get_buffer_size(nullptr, kOutputChannels,
                                                       outSamples, AV_SAMPLE_FMT_S16, 1);
        if (outBytes <= 0)
            return false;

        pcm.data.resize(static_cast<size_t>(outBytes));
        uint8_t* outData[1] = { pcm.data.data() };
        const int converted = swr_convert(swr,
                                          outData,
                                          outSamples,
                                          const_cast<const uint8_t**>(in->extended_data),
                                          in->nb_samples);
        if (converted <= 0)
            return false;

        const int actualBytes = av_samples_get_buffer_size(nullptr, kOutputChannels,
                                                          converted, AV_SAMPLE_FMT_S16, 1);
        if (actualBytes <= 0)
            return false;
        pcm.data.resize(static_cast<size_t>(actualBytes));
        pcm.duration = static_cast<REFERENCE_TIME>(converted) * 10000000LL / kOutputSampleRate;
        const REFERENCE_TIME framePts =
            (in->pts != AV_NOPTS_VALUE) ? static_cast<REFERENCE_TIME>(in->pts) : nextPts;
        pcm.pts = framePts;
        if (nextPts >= 0)
            nextPts = ((framePts >= 0) ? framePts : nextPts) + pcm.duration;
        return !pcm.data.empty();
    }

    static constexpr int kOutputSampleRate = 48000;
    static constexpr int kOutputChannels = 2;
    enum class InputKind {
        Unknown,
        Latm,
        Adts,
    };

    int streamIndex = -1;
    AVCodecContext* latmCtx = nullptr;
    AVCodecContext* adtsCtx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    AVChannelLayout lastInLayout{};
    int inRate = 0;
    AVSampleFormat inFormat = AV_SAMPLE_FMT_NONE;
    std::vector<uint8_t> loasBuffer;
    std::vector<uint8_t> adtsBuffer;
    REFERENCE_TIME pendingPts = -1;
    REFERENCE_TIME adtsPendingPts = -1;
    REFERENCE_TIME nextPts = -1;
    InputKind currentInputKind = InputKind::Unknown;
    unsigned droppedSyncBytes = 0;
    unsigned badHeaderCount = 0;
    unsigned badAdtsHeaderCount = 0;
};

// ---------------------------------------------------------------------------
// Pin management
// ---------------------------------------------------------------------------
void CMmtTlvSplitter::CreatePins()
{
    for (auto* p : m_pins) delete p;
    m_pins.clear();
    m_latmPcmDecoders.clear();
    m_limitAudioToSelected = false;

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
        int preferredPcmAudioStreamIndex = -1;
        bool hasLatmPcmAudio = false;
        for (const auto& info : audioStreams) {
            if (ShouldDecodeLatmToPcm(info.latm, info.channels)) {
                hasLatmPcmAudio = true;
                preferredPcmAudioStreamIndex = info.streamIndex;
                break;
            }
        }
        const bool limitToLatmPcm = hasLatmPcmAudio && audioStreams.size() == 1;
        m_limitAudioToSelected = limitToLatmPcm;
        for (size_t i = 0; i < audioStreams.size(); ++i) {
            const bool decodeLatmToPcm =
                ShouldDecodeLatmToPcm(audioStreams[i].latm, audioStreams[i].channels);
            if (limitToLatmPcm && !decodeLatmToPcm)
                continue;

            WCHAR pinName[128];
            const std::wstring sourceLabel = AudioTimelineLabel(audioStreams[i]);
            StringCchPrintfW(pinName, ARRAYSIZE(pinName), L"Audio %zu %s",
                             i + 1, sourceLabel.c_str());
            auto* pin = new CMmtTlvOutputPin(false, &hr, this, &m_pinLock,
                                             pinName, audioStreams[i].streamIndex);
            if (decodeLatmToPcm) {
                pin->SetPcmAudioInfo(48000, 2, 16);
                m_latmPcmDecoders.push_back(
                    std::make_unique<LatmPcmDecoder>(audioStreams[i].streamIndex));
            } else if (audioStreams[i].samplingRate > 0) {
                pin->SetAudioInfo(static_cast<int>(audioStreams[i].samplingRate),
                                  audioStreams[i].channels,
                                  16,
                                  audioStreams[i].latm,
                                  audioStreams[i].extraData);
            }
            WCHAR trackName[128];
            if (decodeLatmToPcm) {
                StringCchPrintfW(trackName, ARRAYSIZE(trackName),
                                 L"%s -> PCM 2.0",
                                 sourceLabel.c_str());
            } else {
                StringCchPrintfW(trackName, ARRAYSIZE(trackName),
                                 L"%s",
                                 sourceLabel.c_str());
            }
            pin->SetTrackName(trackName);
            m_pins.push_back(pin);
            LogMsg(L"MMT/TLV Splitter: CreatePins created %s for streamIndex=%d, packetId=0x%04X, componentTag=%d, format=%s, channels=%u, output=%s, extra=%zu\n",
                   pinName,
                   audioStreams[i].streamIndex,
                   audioStreams[i].packetId,
                   audioStreams[i].componentTag,
                   audioStreams[i].latm ? L"LATM" : L"ADTS",
                   audioStreams[i].channels,
                   decodeLatmToPcm ? L"PCM stereo" : L"passthrough",
                   audioStreams[i].extraData.size());
        }
        if (preferredPcmAudioStreamIndex >= 0)
            m_handler.selectAudioStreamByStreamIndex(preferredPcmAudioStreamIndex);
        if (limitToLatmPcm)
            LogMsg(L"MMT/TLV Splitter: CreatePins limited audio outputs to LATM PCM streamIndex=%d\n",
                   preferredPcmAudioStreamIndex);
        else if (hasLatmPcmAudio)
            LogMsg(L"MMT/TLV Splitter: CreatePins kept non-LATM audio outputs alongside LATM PCM streamIndex=%d\n",
                   preferredPcmAudioStreamIndex);
    }
    constexpr bool kEnableSubtitlePins = true;
    auto subtitleStreams = m_handler.getSubtitleStreams();
    std::stable_sort(subtitleStreams.begin(), subtitleStreams.end(),
        [](const CFilterDemuxerHandler::SubtitleStreamInfo& a,
           const CFilterDemuxerHandler::SubtitleStreamInfo& b) {
            return SubtitlePinPriority(a) < SubtitlePinPriority(b);
        });
    if (kEnableSubtitlePins) {
        int primarySubtitleStreamIndex = -1;
        for (const auto& info : subtitleStreams) {
            if (info.hasData && IsCaptionComponentTag(info.componentTag)) {
                primarySubtitleStreamIndex = info.streamIndex;
                break;
            }
        }
        if (primarySubtitleStreamIndex < 0) {
            for (const auto& info : subtitleStreams) {
                if (IsCaptionComponentTag(info.componentTag)) {
                    primarySubtitleStreamIndex = info.streamIndex;
                    break;
                }
            }
        }
        if (primarySubtitleStreamIndex < 0) {
            for (const auto& info : subtitleStreams) {
                if (info.hasData) {
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
            REFERENCE_TIME normPts, normDts;
            if (IsMultiSegment()) {
                if (!EditMapPts(pts, normPts))
                    return; // sample is in a cut gap / past the segment -> drop
                const REFERENCE_TIME off = pts - normPts;
                normDts = (dts >= 0) ? dts - off : dts;
            } else {
                normPts = (pts >= 0 && m_firstPts >= 0) ? pts - m_firstPts : pts;
                normDts = (dts >= 0 && m_firstPts >= 0) ? dts - m_firstPts : dts;
            }
            if (last && normPts > 0)
                m_currentPts.store(normPts, std::memory_order_relaxed);
            if (last && normDts >= 0)
                m_currentDts.store(normDts, std::memory_order_relaxed);
            REFERENCE_TIME samplePts = ToSegmentTime(normPts, m_segmentStart);
            REFERENCE_TIME sampleDts = ToSegmentTime(normDts, m_segmentStart);

            PumpPendingSubtitleChunks(samplePts);
            videoPin->DeliverSample(key, samplePts, sampleDts, first, last, d, sz);
        });

    m_handler.setAudioCallback(
        [this](int streamIndex, bool key, long long pts, long long dts,
                         bool first, bool last, const uint8_t* d, size_t sz) {
            const int selectedStreamIndex = m_handler.getSelectedAudioStreamIndex();
            const bool useSelectedAudioOnly = m_limitAudioToSelected;
            if (useSelectedAudioOnly && selectedStreamIndex >= 0 && selectedStreamIndex != streamIndex)
                return;

            REFERENCE_TIME normPts, normDts;
            if (IsMultiSegment()) {
                if (!EditMapPts(pts, normPts))
                    return; // sample is in a cut gap / past the segment -> drop
                const REFERENCE_TIME off = pts - normPts;
                normDts = (dts >= 0) ? dts - off : dts;
            } else {
                normPts = (pts >= 0 && m_firstPts >= 0) ? pts - m_firstPts : pts;
                normDts = (dts >= 0 && m_firstPts >= 0) ? dts - m_firstPts : dts;
            }
            REFERENCE_TIME samplePts = ToSegmentTime(normPts, m_segmentStart);
            REFERENCE_TIME sampleDts = ToSegmentTime(normDts, m_segmentStart);

            bool delivered = false;
            for (auto* pin : m_pins) {
                if (pin->IsAudio() &&
                    (pin->AudioStreamIndex() == -1 || pin->AudioStreamIndex() == streamIndex)) {
                    LatmPcmDecoder* decoder = nullptr;
                    for (const auto& candidate : m_latmPcmDecoders) {
                        if (candidate && candidate->StreamIndex() == streamIndex) {
                            decoder = candidate.get();
                            break;
                        }
                    }
                    if (decoder) {
                        std::vector<LatmPcmDecoder::PcmFrame> frames;
                                    if (decoder->Decode(key, samplePts, first, last, d, sz, frames)) {
                            for (const auto& frame : frames) {
                                pin->DeliverSample(true, frame.pts, frame.pts, true, true,
                                                   frame.data.data(), frame.data.size());
                            }
                        }
                    } else {
                        pin->DeliverSample(key, samplePts, sampleDts, first, last, d, sz);
                    }
                    delivered = true;
                }
            }
            if (!delivered && last) {
                LogMsg(L"AUDIO CALLBACK: no pin for streamIndex=%d, pts=%I64d ms\n",
                       streamIndex, normPts / 10000);
            }
        });

    m_handler.setProgramStartCallback(
        [this](long long programStartRt) {
            const REFERENCE_TIME oldStart = m_subtitleResolver.OnProgramStart(programStartRt);
            if (oldStart != programStartRt) {
                LogMsg(L"SUBTITLE EIT program start updated: old=%I64d ms, new=%I64d ms\n",
                       oldStart / 10000,
                       programStartRt / 10000);
            }
            ProcessDeferredSubtitleSamples();
        });

    m_handler.setNtpCallback(
        [this](long long ntpRt) {
            const REFERENCE_TIME mediaRt = SubtitleTimingAnchor();
            m_subtitleResolver.OnNtpAnchor(ntpRt, mediaRt);
            static volatile LONG s_ntpLogCount = 0;
            LONG logNo = InterlockedIncrement(&s_ntpLogCount);
            if (logNo <= 40 || m_subtitleResolver.AwaitingProgramStart()) {
                LogDetail(L"SUBTITLE NTP anchor #%ld: ntp=%I64d ms, media=%I64d ms, current=%I64d ms\n",
                          logNo,
                          ntpRt / 10000,
                          mediaRt / 10000,
                          m_currentPts.load(std::memory_order_acquire) / 10000);
            }
            ProcessDeferredSubtitleSamples();
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

            const bool waitingForProgramStart =
                m_subtitleResolver.AwaitingProgramStart() &&
                m_subtitleResolver.ProgramStart() < 0;
            if (cue.hasBegin && waitingForProgramStart) {
                DeferredSubtitleSample sample;
                sample.streamIndex = streamIndex;
                sample.pts = pts;
                sample.callbackNo = callbackNo;
                sample.data.assign(d, d + sz);
                m_deferredSubtitleSamples.push_back(std::move(sample));
                if (m_deferredSubtitleSamples.size() > 32)
                    m_deferredSubtitleSamples.erase(m_deferredSubtitleSamples.begin());
                LogDetail(L"SUBTITLE CALLBACK deferred for EIT: callback=%ld streamIndex=%d waitEit=%d ttmlBegin=%I64d ms size=%zu\n",
                          callbackNo,
                          streamIndex,
                          waitingForProgramStart ? 1 : 0,
                          cue.begin / 10000,
                          sz);
                return;
            }

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

            if (callbackNo <= 120 || cue.text.empty()) {
                std::wstring preview = Utf8Preview(cue.text);
                LogMsg(L"SUBTITLE CALLBACK #%ld: streamIndex=%d, pts=%I64d ms, ttmlBegin=%s%I64d ms, ttmlEnd=%s%I64d ms, size=%zu, divs=%zu, p=%zu, spans=%zu, textBytes=%zu, assEvents=%zu, assTextBytes=%zu, text=\"%s\"\n",
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
                       cue.assEvents.size(),
                       cue.assText.size(),
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

            REFERENCE_TIME sampleStart;
            REFERENCE_TIME sampleStop;
            bool repeatUntilNextCue = false;
            if (cue.hasBegin) {
                const REFERENCE_TIME sourceBegin = SubtitleSourceTime(cue.begin);
                const REFERENCE_TIME subtitleTimeOffset = ResolveSubtitleOffset(cue.begin, sourceBegin);
                sampleStart = sourceBegin - subtitleTimeOffset;
                if (cue.hasEnd) {
                    sampleStop = SubtitleSourceTime(cue.end) - subtitleTimeOffset;
                } else {
                    REFERENCE_TIME nextBegin = -1;
                    const long long lookaheadOffset = m_demuxByteOffset.load(std::memory_order_acquire);
                    if (FindNextSubtitleBegin(streamIndex, cue.begin, lookaheadOffset, nextBegin)) {
                        sampleStop = SubtitleSourceTime(nextBegin) - subtitleTimeOffset;
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
                // No TTML begin: use the same stable anchor as timed cues.
                REFERENCE_TIME anchor = SubtitleTimingAnchor();
                sampleStart = ToSegmentTime(anchor, m_segmentStart);
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

REFERENCE_TIME CMmtTlvSplitter::SubtitleTimingAnchor() const
{
    REFERENCE_TIME anchor = m_currentDts.load(std::memory_order_acquire);
    if (anchor < 0)
        anchor = m_currentPts.load(std::memory_order_acquire);
    if (anchor < 0)
        anchor = 0;
    return anchor;
}

REFERENCE_TIME CMmtTlvSplitter::SubtitleSourceTime(REFERENCE_TIME ttmlTime) const
{
    return m_subtitleResolver.SourceTime(ttmlTime);
}

REFERENCE_TIME CMmtTlvSplitter::ResolveSubtitleOffset(REFERENCE_TIME ttmlBegin, REFERENCE_TIME sourceBegin)
{
    // Subtitle samples have no MFU PTS, so synthesize a source timeline from
    // EIT program start + TTML begin when possible. If an NTP/PCR anchor is
    // available, align the first cue of each program to the latest PCR-derived
    // media time and keep that offset for TTML spacing. dantto4k calibrated the
    // same way when this was written; it has since moved to per-document
    // anchoring - see SubtitleTimingResolver.h.
    // offset maps subtitle source time
    // -> normalized media time:
    //   mediaStart = sourceBegin - offset
    // See SubtitleTimingResolver.h for the actual state machine (shared with
    // the offline diagnostic tools under tools/).
    return m_subtitleResolver.ResolveOffset(ttmlBegin, sourceBegin, SubtitleTimingAnchor(),
                                            m_currentPts.load(std::memory_order_acquire),
                                            m_currentDts.load(std::memory_order_acquire),
                                            m_segmentStart,
                                            m_segmentTimeOffset.load(std::memory_order_acquire));
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
        if (cue.hasBegin) {
            const bool waitingForProgramStart =
                m_subtitleResolver.AwaitingProgramStart() &&
                m_subtitleResolver.ProgramStart() < 0;
            if (waitingForProgramStart) {
                remaining.push_back(sample);
                continue;
            }
        }
        if (cue.missingGlyph) {
            remaining.push_back(sample);
            continue;
        }
        if (cue.text.empty())
            continue;

        REFERENCE_TIME sampleStart = 0;
        REFERENCE_TIME sampleStop = 0;
        if (cue.hasBegin) {
            const REFERENCE_TIME sourceBegin = SubtitleSourceTime(cue.begin);
            const REFERENCE_TIME subtitleTimeOffset = ResolveSubtitleOffset(cue.begin, sourceBegin);
            sampleStart = sourceBegin - subtitleTimeOffset;
            sampleStop = cue.hasEnd ? SubtitleSourceTime(cue.end) - subtitleTimeOffset
                                    : sampleStart + kDefaultSubtitleDuration;
            sampleStart = ToSegmentTime(sampleStart, m_segmentStart);
            sampleStop = ToSegmentTime(sampleStop, m_segmentStart);
        } else {
            // No TTML begin: use the same stable anchor as timed cues.
            REFERENCE_TIME anchor = SubtitleTimingAnchor();
            sampleStart = ToSegmentTime(anchor, m_segmentStart);
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
    size_t deliveredSamples = 0;
    auto deliverToPin = [&](CMmtTlvOutputPin* pin) {
        if (!assEvents.empty()) {
            for (const auto& sampleText : assEvents) {
                pin->DeliverTextSample(start, stop, sampleText.c_str(), sampleText.size());
                ++deliveredSamples;
            }
        } else {
            pin->DeliverTextSample(start, stop, assText.c_str(), assText.size());
            ++deliveredSamples;
        }
        delivered = true;
    };

    for (auto* pin : m_pins) {
        if (pin->IsSubtitle() && pin->StreamIndex() == streamIndex) {
            deliverToPin(pin);
        }
    }

    int fallbackStreamIndex = -1;
    if (!delivered) {
        for (auto* pin : m_pins) {
            if (pin->IsSubtitle()) {
                fallbackStreamIndex = pin->StreamIndex();
                deliverToPin(pin);
                break;
            }
        }
    }

    static volatile LONG s_subtitleDeliverCueLogs = 0;
    LONG deliverLogNo = InterlockedIncrement(&s_subtitleDeliverCueLogs);
    if (deliverLogNo <= 160 || !delivered) {
        LogMsg(L"SUBTITLE DeliverCue #%ld: streamIndex=%d start=%I64d ms stop=%I64d ms assEvents=%zu assTextBytes=%zu delivered=%d samples=%zu pins=%zu fallbackStreamIndex=%d\n",
               deliverLogNo,
               streamIndex,
               start / 10000,
               stop / 10000,
               assEvents.size(),
               assText.size(),
               delivered ? 1 : 0,
               deliveredSamples,
               m_pins.size(),
               fallbackStreamIndex);
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
    for (const auto& decoder : m_latmPcmDecoders)
        if (decoder)
            decoder->Reset(true);

    m_seekTarget = pos;
    m_currentPts = pos;   // normalised position (0-based)
    m_currentDts = -1;
    m_subtitleResolver.Reset(pos > 0);
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

    REFERENCE_TIME seekTarget = m_seekTarget;
    REFERENCE_TIME sourceSeekTarget;
    if (IsMultiSegment()) {
        // Map the program seek target to the source position inside its segment.
        REFERENCE_TIME srcRel = 0;
        m_curSegment = SegmentAtProgram(seekTarget, srcRel);
        m_curSegmentBase = SegmentBase(m_curSegment);
        m_pendingSegmentJump.store(-1, std::memory_order_release);
        sourceSeekTarget = srcRel;
        LogMsg(L"MMT/TLV Splitter: EDL start program=%I64d ms -> seg=%d base=%I64d ms src=%I64d ms\n",
               seekTarget / 10000, m_curSegment, m_curSegmentBase / 10000, sourceSeekTarget / 10000);
    } else {
        sourceSeekTarget = seekTarget + m_virtualStart;
    }
    m_segmentStart = seekTarget;
    m_segmentTimeOffset.store(0, std::memory_order_release);
    m_subtitleResolver.Reset(sourceSeekTarget > 0);
    ClearPendingSubtitleCues();
    const bool waitForRap = sourceSeekTarget > 0;
    m_waitingForVideoRap.store(waitForRap, std::memory_order_release);
    for (auto* pin : m_pins) {
        if (pin->IsVideo())
            pin->SetWaitForVideoRap(waitForRap);
    }
    // Clear demuxer state for normal play; seek path already called resetStreams()
    if (sourceSeekTarget == 0) {
        m_demuxer.clear();
        m_handler.reset();
    }
    ApplySidecarMapTracks(sourceSeekTarget);

    if (sourceSeekTarget > 0 && m_fileSize > 0) {
        long long mappedOffset = -1;
        // Map RAP offsets are keyed by absolute PTS; the single-range path passes
        // relative time (and falls back to ratio). For the EDL path we query with
        // absolute time so segment starts land on a real RAP at/just before them.
        const REFERENCE_TIME mapQuery = IsMultiSegment()
            ? sourceSeekTarget + (m_mapFirstVideoPts >= 0 ? m_mapFirstVideoPts : 0)
            : sourceSeekTarget;
        bool usedMap = FindSidecarMapSeekOffset(mapQuery, mappedOffset);
        double ratio = 0.0;
        std::streamoff byteOffset = 0;
        if (usedMap) {
            byteOffset = static_cast<std::streamoff>(mappedOffset);
        } else {
            const REFERENCE_TIME sourceDuration = m_sourceDuration > 0 ? m_sourceDuration : (m_duration + m_virtualStart);
            ratio = (sourceDuration > 0)
                ? static_cast<double>(sourceSeekTarget) / sourceDuration
                : 0.0;
            if (ratio > 1.0) ratio = 1.0;
            byteOffset = static_cast<std::streamoff>(ratio * m_fileSize);
        }
        ifs.seekg(byteOffset);
        m_demuxByteOffset.store(static_cast<long long>(byteOffset), std::memory_order_release);
        LogMsg(L"MMT/TLV Splitter: DemuxLoop seek target=%I64d ms source=%I64d ms method=%s ratio=%.6f byte=%I64d/%I64d ok=%d\n",
               seekTarget / 10000,
               sourceSeekTarget / 10000,
               usedMap ? L"mmtsmap" : L"ratio",
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

        // EDL: a segment boundary was reached (a video sample past the current
        // segment end). Jump to the next segment, or stop after the last one.
        if (IsMultiSegment()) {
            const int jump = m_pendingSegmentJump.exchange(-1, std::memory_order_acq_rel);
            if (jump == -2) {
                LogMsg(L"MMT/TLV Splitter: EDL reached end of last segment\n");
                break; // -> EOS below
            }
            if (jump >= 0 && jump < static_cast<int>(m_editSegments.size())) {
                m_curSegment = jump;
                m_curSegmentBase = SegmentBase(jump);
                const REFERENCE_TIME srcRel = m_editSegments[jump].start;
                // Prefer a precise map RAP offset (absolute-time query); fall back
                // to a duration-ratio estimate. Both land at/just before srcRel, so
                // the RAP wait + EditMapPts drop trims to the exact segment start.
                long long mappedOffset = -1;
                std::streamoff byteOffset = 0;
                const REFERENCE_TIME mapQuery = srcRel + (m_mapFirstVideoPts >= 0 ? m_mapFirstVideoPts : 0);
                if (FindSidecarMapSeekOffset(mapQuery, mappedOffset)) {
                    byteOffset = static_cast<std::streamoff>(mappedOffset);
                } else if (m_sourceDuration > 0) {
                    byteOffset = static_cast<std::streamoff>(static_cast<double>(srcRel) / m_sourceDuration * m_fileSize);
                }
                ifs.clear();
                ifs.seekg(byteOffset);
                buf.clear();
                bufferOffset = static_cast<long long>(byteOffset);
                m_demuxByteOffset.store(bufferOffset, std::memory_order_release);
                m_demuxer.resetStreams();
                m_handler.reset();
                for (const auto& decoder : m_latmPcmDecoders)
                    if (decoder)
                        decoder->Reset();
                m_waitingForVideoRap.store(true, std::memory_order_release);
                for (auto* pin : m_pins)
                    if (pin->IsVideo())
                        pin->SetWaitForVideoRap(true);
                LogMsg(L"MMT/TLV Splitter: EDL jump to seg=%d base=%I64d ms src=%I64d ms byte=%I64d\n",
                       jump, m_curSegmentBase / 10000, srcRel / 10000, static_cast<long long>(byteOffset));
            }
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
            m_currentDts = -1;
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
// The enumeration is video assets first (group 0), then audio assets (group 1).
// Only one video pin exists - a second one would make the graph builder attach
// a second renderer - so switching video means re-pointing that pin at another
// hev1 asset, which is how a player switches video streams anyway.
STDMETHODIMP CMmtTlvSplitter::Count(DWORD* pcStreams)
{
    if (!pcStreams) return E_POINTER;
    auto videoStreams = m_handler.getVideoStreams();
    auto streams = m_handler.getAudioStreams();
    *pcStreams = static_cast<DWORD>(videoStreams.size() + streams.size());
    LogMsg(L"MMT/TLV StreamSelect: Count=%lu (video=%zu, audio=%zu), selectedVideoStreamIndex=%d, selectedStreamIndex=%d\n",
           *pcStreams, videoStreams.size(), streams.size(),
           m_handler.getSelectedVideoStreamIndex(),
           m_handler.getSelectedAudioStreamIndex());
    for (size_t i = 0; i < videoStreams.size(); ++i) {
        const auto& info = videoStreams[i];
        LogDetail(L"MMT/TLV StreamSelect: Count video[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, %dx%d\n",
               i, info.streamIndex, info.packetId, info.componentTag, info.width, info.height);
    }
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
    auto videoStreams = m_handler.getVideoStreams();
    if (lIndex >= 0 && static_cast<size_t>(lIndex) < videoStreams.size()) {
        const size_t listIndex = static_cast<size_t>(lIndex);
        const auto& video = videoStreams[listIndex];
        if (ppmt) *ppmt = nullptr;
        if (pdwFlags) {
            *pdwFlags = AMSTREAMSELECTINFO_EXCLUSIVE;
            if (m_handler.isSelectedVideoStream(listIndex))
                *pdwFlags |= AMSTREAMSELECTINFO_ENABLED;
        }
        if (plcid) *plcid = 0;
        if (pdwGroup) *pdwGroup = 0;
        if (ppszName) {
            WCHAR buf[256];
            if (video.width > 0 && video.height > 0) {
                StringCchPrintfW(buf, ARRAYSIZE(buf),
                                 L"Video %ld %dx%d (stream %d, component %d)",
                                 lIndex + 1, video.width, video.height,
                                 video.streamIndex, video.componentTag);
            } else {
                StringCchPrintfW(buf, ARRAYSIZE(buf),
                                 L"Video %ld (stream %d, component %d)",
                                 lIndex + 1, video.streamIndex, video.componentTag);
            }
            const size_t chars = wcslen(buf) + 1;
            *ppszName = static_cast<LPWSTR>(CoTaskMemAlloc(chars * sizeof(WCHAR)));
            if (!*ppszName)
                return E_OUTOFMEMORY;
            StringCchCopyW(*ppszName, chars, buf);
        }
        if (ppObject) *ppObject = nullptr;
        if (ppUnk) *ppUnk = nullptr;
        LogMsg(L"MMT/TLV StreamSelect: Info index=%ld, video streamIndex=%d, flags=0x%08X, group=0\n",
               lIndex, video.streamIndex, pdwFlags ? *pdwFlags : 0);
        return S_OK;
    }

    auto streams = m_handler.getAudioStreams();
    const long audioIndex = lIndex - static_cast<long>(videoStreams.size());
    if (audioIndex < 0 || static_cast<size_t>(audioIndex) >= streams.size())
        return E_INVALIDARG;
    lIndex = audioIndex;

    const auto& info = streams[static_cast<size_t>(lIndex)];
    if (ppmt) *ppmt = nullptr;
    if (pdwFlags) {
        *pdwFlags = AMSTREAMSELECTINFO_EXCLUSIVE;
        if (m_handler.isSelectedAudioStream(static_cast<size_t>(lIndex)))
            *pdwFlags |= AMSTREAMSELECTINFO_ENABLED;
    }
    if (plcid) *plcid = 0;
    if (pdwGroup) *pdwGroup = 1;
    if (ppszName) {
        const bool pcmOutput = ShouldDecodeLatmToPcm(info.latm, info.channels);
        const std::wstring sourceLabel = AudioTimelineLabel(info);
        WCHAR buf[256];
        StringCchPrintfW(buf, ARRAYSIZE(buf),
                         L"Audio %ld %s%s (stream %d, component %d)",
                         lIndex + 1, sourceLabel.c_str(),
                         pcmOutput ? L" -> PCM 2.0" : L"",
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

    auto videoStreams = m_handler.getVideoStreams();
    if (lIndex >= 0 && static_cast<size_t>(lIndex) < videoStreams.size()) {
        for (auto* pin : m_pins)
            if (pin->IsVideo() && pin->IsConnected())
                pin->DeliverBeginFlush();

        const bool ok = m_handler.selectVideoStreamByListIndex(static_cast<size_t>(lIndex));

        const auto& selected = videoStreams[static_cast<size_t>(lIndex)];
        if (ok && selected.width > 0 && selected.height > 0) {
            m_videoWidth = selected.width;
            m_videoHeight = selected.height;
        }

        for (auto* pin : m_pins) {
            if (pin->IsVideo()) {
                // The connected media type cannot change on the fly; the decoder
                // picks the new geometry up from the parameter sets the broadcast
                // stream repeats at every IRAP. Keep the pin's own idea of the
                // size current for any later reconnect.
                if (ok && selected.width > 0 && selected.height > 0)
                    pin->SetVideoInfo(selected.width, selected.height);
                // ResetForSeek() re-arms the RAP wait, so the newly selected
                // asset starts at an IRAP with its own parameter sets rather
                // than mid-GOP.
                pin->ResetForSeek();
                if (pin->IsConnected())
                    pin->DeliverEndFlush();
            }
        }

        LogMsg(L"MMT/TLV StreamSelect: Enable video index=%ld flags=0x%08X ok=%d\n",
               lIndex, dwFlags, ok ? 1 : 0);
        return ok ? S_OK : E_INVALIDARG;
    }

    auto streams = m_handler.getAudioStreams();
    lIndex -= static_cast<long>(videoStreams.size());
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
