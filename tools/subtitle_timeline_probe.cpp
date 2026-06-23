// subtitle_timeline_probe.cpp
// Diagnostic: dumps video PTS anchor, EIT program-start, NTP wall-clock pairing,
// and subtitle TTML begin/end timestamps in chronological order, so a timeline
// discontinuity (e.g. introduced by an external cut/edit tool) can be spotted.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "mmtTlvDemuxer.h"
#include "demuxerHandler.h"
#include "mmtStream.h"
#include "mpuProcessorBase.h"  // NOPTS_VALUE, MfuData
#include "stream.h"
#include "timebase.h"
#include "timeUtil.h"
#include "mhEit.h"
#include "ntp.h"
#include "ttml.h"

using namespace MmtTlv;

static int64_t PtsToMs(uint64_t pts, const MmtStream& stream)
{
    if (pts == NOPTS_VALUE) return -1;
    const auto& tb = stream.getTimeBase();
    if (tb.den <= 0) return (int64_t)pts / 90;
    return (int64_t)((double)pts * tb.num * 1000.0 / tb.den);
}

static int64_t NtpToUnixMs(const NTPv4& ntp)
{
    const uint32_t NTP_1970 = 2208988800U;
    const auto& t = ntp.transmit_timestamp;
    double ms = (static_cast<double>(t.seconds) - NTP_1970) * 1000.0 +
                (static_cast<double>(t.fraction) / 4294967296.0) * 1000.0;
    return static_cast<int64_t>(ms);
}

class TimelineProbeHandler : public DemuxerHandler {
public:
    int64_t firstVideoPtsMs = -1;
    int64_t lastVideoPtsMs = -1;
    int64_t stopAtMs = -1; // when >=0, stop printing details after this anchor time
    int64_t lastEitStartSec = -1;
    int subtitleCount = 0;

    void onVideoData(const MmtStream& stream, const MfuData& mfu) override
    {
        if (mfu.pts == NOPTS_VALUE || !mfu.isLastFragment) return;
        int64_t ms = PtsToMs(mfu.pts, stream);
        if (firstVideoPtsMs < 0) {
            firstVideoPtsMs = ms;
            std::printf("[video] first pts = %lld ms (raw=%llu)\n", (long long)ms, (unsigned long long)mfu.pts);
        }
        lastVideoPtsMs = ms;
    }

    int64_t Anchor() const
    {
        if (lastVideoPtsMs < 0 || firstVideoPtsMs < 0) return -1;
        return lastVideoPtsMs - firstVideoPtsMs;
    }

    void onMhEit(const MhEit& mhEit) override
    {
        if (!mhEit.isPf() || mhEit.sectionNumber != 0) return;
        for (const auto& ev : mhEit.events) {
            if (!ev) continue;
            std::tm startTime = EITConvertStartTime(ev->startTime);
            if (!isValidEITStartTime(startTime)) continue;
            std::time_t startSec = std::mktime(&startTime);
            if (startSec < 0) continue;
            if ((long long)startSec == lastEitStartSec) return;
            lastEitStartSec = (long long)startSec;
            std::printf("[EIT] anchor=%lld ms  programStart unix=%lld sec (%04d-%02d-%02d %02d:%02d:%02d) eventId=%u\n",
                        (long long)Anchor(), (long long)startSec,
                        startTime.tm_year + 1900, startTime.tm_mon + 1, startTime.tm_mday,
                        startTime.tm_hour, startTime.tm_min, startTime.tm_sec,
                        ev->eventId);
            return;
        }
    }

    void onNtp(const NTPv4& ntp) override
    {
        int64_t unixMs = NtpToUnixMs(ntp);
        std::printf("[NTP] anchor=%lld ms  ntpUnix=%lld ms (%.3f sec)\n",
                    (long long)Anchor(), (long long)unixMs, unixMs / 1000.0);
    }

    void onSubtitleData(const MmtStream& stream, const MfuData& mfu) override
    {
        if (mfu.subtitleDataType != 0 || mfu.data.empty()) return;
        ++subtitleCount;

        std::string xml(mfu.data.begin(), mfu.data.end());
        TTML ttml;
        try {
            ttml = TTMLPaser::parse(xml);
        } catch (...) {
            std::printf("[SUB #%d] anchor=%lld ms  streamIndex=%d  <parse failed> bytes=%zu\n",
                        subtitleCount, (long long)Anchor(), mfu.streamIndex, mfu.data.size());
            return;
        }

        bool hasBegin = false, hasEnd = false;
        uint64_t begin = 0, end = 0;
        std::string text;
        for (const auto& div : ttml.divTags) {
            if (div.begin.has_value()) {
                begin = hasBegin ? (std::min)(begin, *div.begin) : *div.begin;
                hasBegin = true;
            }
            if (div.end.has_value()) {
                end = hasEnd ? (std::max)(end, *div.end) : *div.end;
                hasEnd = true;
            }
            for (const auto& p : div.pTags) {
                for (const auto& span : p.spanTags) {
                    if (!span.text.empty() && text.size() < 40) {
                        text += span.text;
                    }
                }
            }
        }

        std::printf("[SUB #%d] anchor=%lld ms  streamIndex=%d  ttmlBegin=%s%llu ms  ttmlEnd=%s%llu ms  text=\"%s\"\n",
                    subtitleCount, (long long)Anchor(), mfu.streamIndex,
                    hasBegin ? "" : "none/", (unsigned long long)begin,
                    hasEnd ? "" : "none/", (unsigned long long)end,
                    text.c_str());
    }
};

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::printf("Usage: subtitle_timeline_probe <input.mmts> [stopAtAnchorMs]\n");
        return 1;
    }
    const char* filename = argv[1];
    int64_t stopAtMs = argc >= 3 ? _atoi64(argv[2]) : -1;

    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs.is_open()) {
        std::printf("ERROR: cannot open %s\n", filename);
        return 1;
    }

    TimelineProbeHandler handler;
    handler.stopAtMs = stopAtMs;
    MmtTlvDemuxer demuxer;
    demuxer.setDemuxerHandler(handler);

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
        if (got == 0) break;
        totalRead += got;

        Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            auto status = demuxer.demux(stream);
            if (status == DemuxStatus::NotEnoughBuffer) break;
        }
        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) buf.erase(buf.begin(), buf.begin() + consumed);

        if (stopAtMs >= 0 && handler.Anchor() >= stopAtMs) {
            std::printf("[stop] reached anchor=%lld ms at byte offset %llu\n",
                        (long long)handler.Anchor(), (unsigned long long)totalRead);
            break;
        }
    }

    std::printf("Done. totalRead=%llu bytes, lastAnchor=%lld ms, subtitleSamples=%d\n",
                (unsigned long long)totalRead, (long long)handler.Anchor(), handler.subtitleCount);
    return 0;
}
