// mmts_compare_dump.cpp
//
// Single-pass companion tool for subtitle-timing verification (see
// docs discussion): demuxes input.mmts exactly once and produces, in the
// same pass:
//   1) output.ts   - a dantto4k-equivalent MPEG-2 TS (via dantto4k's
//                    RemuxerHandler, unmodified), to be fed to an
//                    arib-splitter-side probe ("Tool B").
//   2) schedule.csv - the mmts-dsfilter live filter's *actual* subtitle
//                    schedule ("Tool A"), computed with the same
//                    SubtitleTimingResolver the filter itself uses
//                    (src/SubtitleTimingResolver.h), fed through the
//                    filter's own CFilterDemuxerHandler so EIT
//                    program-start / NTP anchoring matches exactly.
//
// Reading input.mmts only once (instead of once per tool) matters because
// these source files are huge; see CFilterDemuxerHandler + RemuxerHandler
// composition below.
//
// Simplifications vs. the live filter (documented, not bugs):
//  - Glyph/DRCS resource availability never defers a cue; only plain TTML
//    text is needed for timing comparison, not ASS rendering fidelity.
//  - A cue with no explicit TTML end is always treated as "open until the
//    next cue on the same stream (or EOF) closes it", rather than also
//    trying the live filter's bounded (64MB) forward-seek lookahead. Both
//    paths produce the same result unless a resync happens to land in the
//    gap between the two cues, which is a narrow edge case.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mmtTlvDemuxer.h"
#include "demuxerHandler.h"
#include "mmtStream.h"
#include "config.h"
#include "mpuProcessorBase.h" // NOPTS_VALUE, MfuData
#include "remuxerHandler.h"
#include "TtmlModel.h"
#include "DsHandler.h"
#include "SubtitleTimingResolver.h"

using namespace MmtTlv;

namespace {

constexpr int64_t kDefaultSubtitleDurationRt = 25 * 1000000LL; // 2.5 sec, matches the filter

std::string ExtractPlainTextAndTiming(const uint8_t* data, size_t size,
                                      bool& hasBegin, int64_t& beginRt,
                                      bool& hasEnd, int64_t& endRt)
{
    hasBegin = false;
    hasEnd = false;
    beginRt = 0;
    endRt = 0;
    if (!data || size == 0)
        return {};

    std::string xml(reinterpret_cast<const char*>(data), size);
    DsTtml::Document ttml;
    try {
        ttml = DsTtml::Parse(xml);
    } catch (...) {
        return {};
    }

    uint64_t beginMs = 0, endMs = 0;
    std::string text;
    for (const auto& div : ttml.divTags) {
        if (div.begin.has_value()) {
            beginMs = hasBegin ? (std::min)(beginMs, *div.begin) : *div.begin;
            hasBegin = true;
        }
        if (div.end.has_value()) {
            endMs = hasEnd ? (std::max)(endMs, *div.end) : *div.end;
            hasEnd = true;
        }
        for (const auto& p : div.pTags) {
            bool wroteLine = false;
            for (const auto& span : p.spanTags) {
                if (!span.text.empty()) {
                    text += span.text;
                    wroteLine = true;
                }
            }
            if (wroteLine)
                text += "\n";
        }
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    beginRt = static_cast<int64_t>(beginMs) * 10000; // ms -> REFERENCE_TIME (100ns)
    endRt = static_cast<int64_t>(endMs) * 10000;
    return text;
}

struct ScheduledCue {
    int streamIndex = -1;
    int64_t startRt = 0;
    int64_t stopRt = 0;
    std::string text;
};

// Replays the same TTML-anchoring state machine the live filter runs
// (CMmtTlvSplitter), driven by CFilterDemuxerHandler callbacks, to produce
// a final (start, stop, text) schedule. See SubtitleTimingResolver.h for
// the shared offset/resync logic.
class SubtitleScheduleCollector {
public:
    void OnVideo(long long pts, long long dts)
    {
        if (pts >= 0 && m_firstPts < 0)
            m_firstPts = pts;
        long long anchor = (dts >= 0) ? dts : pts;
        if (anchor < 0)
            return;
        m_currentAnchor = (m_firstPts >= 0) ? (anchor - m_firstPts) : anchor;
        if (m_currentAnchor < 0)
            m_currentAnchor = 0;
    }

    void OnProgramStart(long long programStartRt)
    {
        m_resolver.OnProgramStart(programStartRt);
        DrainDeferred();
    }

    void OnNtp(long long ntpRt)
    {
        m_resolver.OnNtpAnchor(ntpRt, m_currentAnchor);
        DrainDeferred();
    }

    void OnSubtitle(int streamIndex, const std::string& text,
                    bool hasBegin, int64_t begin, bool hasEnd, int64_t end)
    {
        if (text.empty())
            return;
        if (hasBegin && m_resolver.AwaitingProgramStart() && m_resolver.ProgramStart() < 0) {
            m_deferred.push_back({streamIndex, text, hasBegin, begin, hasEnd, end});
            return;
        }
        ResolveAndEmit(streamIndex, text, hasBegin, begin, hasEnd, end);
    }

    void Finish()
    {
        for (auto& kv : m_openByStream) {
            ScheduledCue& cue = kv.second;
            if (m_currentAnchor > cue.startRt) {
                cue.stopRt = m_currentAnchor;
                m_cues.push_back(cue);
            }
        }
        m_openByStream.clear();
    }

    std::vector<ScheduledCue>& Cues() { return m_cues; }

private:
    struct DeferredCue {
        int streamIndex;
        std::string text;
        bool hasBegin;
        int64_t begin;
        bool hasEnd;
        int64_t end;
    };

    void DrainDeferred()
    {
        if (m_deferred.empty())
            return;
        std::vector<DeferredCue> remaining;
        for (auto& d : m_deferred) {
            if (d.hasBegin && m_resolver.AwaitingProgramStart() && m_resolver.ProgramStart() < 0) {
                remaining.push_back(d);
                continue;
            }
            ResolveAndEmit(d.streamIndex, d.text, d.hasBegin, d.begin, d.hasEnd, d.end);
        }
        m_deferred.swap(remaining);
    }

    void CloseOpenCue(int streamIndex, int64_t stopRt)
    {
        auto it = m_openByStream.find(streamIndex);
        if (it == m_openByStream.end())
            return;
        if (stopRt > it->second.startRt) {
            it->second.stopRt = stopRt;
            m_cues.push_back(it->second);
        }
        m_openByStream.erase(it);
    }

    void ResolveAndEmit(int streamIndex, const std::string& text,
                        bool hasBegin, int64_t begin, bool hasEnd, int64_t end)
    {
        int64_t startRt;
        int64_t stopRt = -1;
        bool openEnded = false;

        if (hasBegin) {
            const int64_t sourceBegin = m_resolver.SourceTime(begin);
            const int64_t offset = m_resolver.ResolveOffset(begin, sourceBegin, m_currentAnchor,
                                                             m_currentAnchor, m_currentAnchor, 0, 0);
            startRt = sourceBegin - offset;
            if (hasEnd)
                stopRt = m_resolver.SourceTime(end) - offset;
            else
                openEnded = true;
        } else {
            startRt = m_currentAnchor;
        }
        if (startRt < 0)
            startRt = 0;

        CloseOpenCue(streamIndex, startRt);

        if (!hasBegin) {
            m_cues.push_back({streamIndex, startRt, startRt + kDefaultSubtitleDurationRt, text});
            return;
        }
        if (openEnded) {
            m_openByStream[streamIndex] = {streamIndex, startRt, -1, text};
            return;
        }
        if (stopRt <= startRt)
            stopRt = startRt + kDefaultSubtitleDurationRt;
        m_cues.push_back({streamIndex, startRt, stopRt, text});
    }

    SubtitleTimingResolver m_resolver;
    long long m_firstPts = -1;
    int64_t m_currentAnchor = 0;
    std::vector<DeferredCue> m_deferred;
    std::map<int, ScheduledCue> m_openByStream;
    std::vector<ScheduledCue> m_cues;
};

// Forwards every MmtTlv::DemuxerHandler event to dantto4k's RemuxerHandler
// (TS muxing) and the subset CFilterDemuxerHandler needs (subtitle timing),
// so a single demux pass over input.mmts feeds both outputs.
class CompositeDemuxerHandler : public DemuxerHandler {
public:
    CompositeDemuxerHandler(RemuxerHandler& remuxer, CFilterDemuxerHandler& filterHandler)
        : m_remuxer(remuxer), m_filterHandler(filterHandler) {}

    void onVideoData(const MmtStream& s, const MfuData& m) override
    {
        m_remuxer.onVideoData(s, m);
        m_filterHandler.onVideoData(s, m);
    }
    void onAudioData(const MmtStream& s, const MfuData& m) override { m_remuxer.onAudioData(s, m); }
    void onSubtitleData(const MmtStream& s, const MfuData& m) override
    {
        m_remuxer.onSubtitleData(s, m);
        m_filterHandler.onSubtitleData(s, m);
    }
    void onEcm(const Ecm& v) override { m_remuxer.onEcm(v); }
    void onMhBit(const MhBit& v) override { m_remuxer.onMhBit(v); }
    void onMhCdt(const MhCdt& v) override { m_remuxer.onMhCdt(v); }
    void onMhEit(const MhEit& v) override
    {
        m_remuxer.onMhEit(v);
        m_filterHandler.onMhEit(v);
    }
    void onMhSdtActual(const MhSdt& v) override { m_remuxer.onMhSdtActual(v); }
    void onMhTot(const MhTot& v) override { m_remuxer.onMhTot(v); }
    void onMpt(const Mpt& v) override { m_remuxer.onMpt(v); }
    void onPlt(const Plt& v) override { m_remuxer.onPlt(v); }
    void onNit(const Nit& v) override { m_remuxer.onNit(v); }
    void onNtp(const NTPv4& v) override
    {
        m_remuxer.onNtp(v);
        m_filterHandler.onNtp(v);
    }
    void onPacketDrop(uint16_t packetId, const MmtStream* s) override { m_remuxer.onPacketDrop(packetId, s); }

private:
    RemuxerHandler& m_remuxer;
    CFilterDemuxerHandler& m_filterHandler;
};

void WriteCsvField(std::ofstream& out, const std::string& field)
{
    out << '"';
    for (char c : field) {
        if (c == '"')
            out << "\"\"";
        else if (c == '\n')
            out << "\\n";
        else if (c == '\r')
            continue;
        else
            out << c;
    }
    out << '"';
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 4) {
        std::printf("Usage: mmts_compare_dump <input.mmts> <output.ts> <schedule_A.csv> [subtitleDebugLog]\n");
        return 1;
    }
    const char* inputPath = argv[1];
    const char* tsPath = argv[2];
    const char* csvPath = argv[3];
    if (argc >= 5)
        config.subtitleDebugLogPath = argv[4];

    std::ifstream ifs(inputPath, std::ios::binary);
    if (!ifs.is_open()) {
        std::printf("ERROR: cannot open input %s\n", inputPath);
        return 1;
    }
    std::ofstream tsOut(tsPath, std::ios::binary);
    if (!tsOut.is_open()) {
        std::printf("ERROR: cannot open output %s\n", tsPath);
        return 1;
    }

    MmtTlvDemuxer demuxer;
    RemuxerHandler remuxer(demuxer);
    remuxer.setOutputCallback([&](const uint8_t* data, size_t size) {
        tsOut.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    });

    CFilterDemuxerHandler filterHandler;
    SubtitleScheduleCollector collector;
    filterHandler.setVideoCallback(
        [&](int, bool, long long pts, long long dts, bool, bool, const uint8_t*, size_t) {
            collector.OnVideo(pts, dts);
        });
    filterHandler.setSubtitleCallback(
        [&](int streamIndex, bool, long long, long long, bool, bool, const uint8_t* d, size_t sz) {
            bool hasBegin = false, hasEnd = false;
            int64_t begin = 0, end = 0;
            std::string text = ExtractPlainTextAndTiming(d, sz, hasBegin, begin, hasEnd, end);
            collector.OnSubtitle(streamIndex, text, hasBegin, begin, hasEnd, end);
        });
    filterHandler.setProgramStartCallback([&](long long programStartRt) {
        collector.OnProgramStart(programStartRt);
    });
    filterHandler.setNtpCallback([&](long long ntpRt) {
        collector.OnNtp(ntpRt);
    });

    CompositeDemuxerHandler composite(remuxer, filterHandler);
    demuxer.setDemuxerHandler(composite);

    constexpr size_t kChunk = 1024 * 1024;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    uint64_t totalRead = 0;

    while (ifs) {
        size_t oldSz = buf.size();
        buf.resize(oldSz + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
        size_t got = static_cast<size_t>(ifs.gcount());
        buf.resize(oldSz + got);
        if (got == 0)
            break;
        totalRead += got;

        Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            auto status = demuxer.demux(stream);
            if (status == DemuxStatus::NotEnoughBuffer)
                break;
        }
        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0)
            buf.erase(buf.begin(), buf.begin() + consumed);

        if (totalRead % (1024ULL * 1024 * 1024) < kChunk) {
            std::printf("[progress] %.1f GB scanned\n", totalRead / 1073741824.0);
        }
    }

    collector.Finish();
    demuxer.clear();
    tsOut.close();

    std::ofstream csv(csvPath, std::ios::binary);
    if (!csv.is_open()) {
        std::printf("ERROR: cannot open %s\n", csvPath);
        return 1;
    }
    std::stable_sort(collector.Cues().begin(), collector.Cues().end(),
                     [](const ScheduledCue& a, const ScheduledCue& b) { return a.startRt < b.startRt; });

    csv << "index,streamIndex,startMs,endMs,durationMs,text\n";
    int index = 0;
    for (const auto& cue : collector.Cues()) {
        const int64_t startMs = cue.startRt / 10000;
        const int64_t endMs = cue.stopRt / 10000;
        csv << (++index) << ',' << cue.streamIndex << ',' << startMs << ',' << endMs << ','
            << (endMs - startMs) << ',';
        WriteCsvField(csv, cue.text);
        csv << '\n';
    }

    std::printf("Done. totalRead=%llu bytes, cues=%zu, ts=%s, csv=%s\n",
                (unsigned long long)totalRead, collector.Cues().size(), tsPath, csvPath);
    return 0;
}
