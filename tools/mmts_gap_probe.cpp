// mmts_gap_probe.cpp
// Reports gaps in the recorded video/audio timeline of a .mmts,
// to tell a playback problem apart from a hole in the recording itself.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "demuxerHandler.h"
#include "mmtTlvDemuxer.h"
#include "mmtStream.h"
#include "mpuProcessorBase.h"
#include "stream.h"

using namespace MmtTlv;

static int64_t ptsToMs(uint64_t pts, const MmtStream& stream)
{
    if (pts == NOPTS_VALUE) return -1;
    const auto& tb = stream.getTimeBase();
    if (tb.den <= 0) return static_cast<int64_t>(pts) / 90;
    return static_cast<int64_t>(static_cast<double>(pts) * tb.num * 1000.0 / tb.den);
}

class GapProbe : public DemuxerHandler {
public:
    int64_t firstVideoMs = -1;
    int64_t lastVideoMs = -1;
    int64_t lastAudioMs = -1;
    uint16_t lastVideoPid = 0;
    long long videoAus = 0;

    void onVideoData(const MmtStream& stream, const MfuData& mfu) override
    {
        if (!mfu.isLastFragment) return;
        const int64_t ms = ptsToMs(mfu.pts, stream);
        if (ms < 0) return;
        ++videoAus;
        if (firstVideoMs < 0) { firstVideoMs = ms; lastVideoMs = ms; lastVideoPid = stream.getPacketId(); return; }

        const int64_t delta = ms - lastVideoMs;
        if (delta > 200) {
            printf("VIDEO gap: %lld ms -> %lld ms (%.3f s missing) pid 0x%04X -> 0x%04X\n",
                   (long long)(lastVideoMs - firstVideoMs), (long long)(ms - firstVideoMs),
                   delta / 1000.0, lastVideoPid, stream.getPacketId());
        }
        if (ms > lastVideoMs) lastVideoMs = ms;
        lastVideoPid = stream.getPacketId();
    }

    void onAudioData(const MmtStream& stream, const MfuData& mfu) override
    {
        if (!mfu.isLastFragment) return;
        const int64_t ms = ptsToMs(mfu.pts, stream);
        if (ms < 0) return;
        if (lastAudioMs < 0) { lastAudioMs = ms; return; }
        const int64_t delta = ms - lastAudioMs;
        if (delta > 200 && firstVideoMs >= 0) {
            printf("AUDIO gap: %lld ms -> %lld ms (%.3f s missing) pid 0x%04X\n",
                   (long long)(lastAudioMs - firstVideoMs), (long long)(ms - firstVideoMs),
                   delta / 1000.0, stream.getPacketId());
        }
        if (ms > lastAudioMs) lastAudioMs = ms;
    }
};

int main(int argc, char* argv[])
{
    if (argc < 2) { printf("Usage: mmts_gap_probe <input.mmts>\n"); return 1; }
    std::ifstream ifs(argv[1], std::ios::binary);
    if (!ifs.is_open()) { printf("ERROR: cannot open\n"); return 1; }

    MmtTlvDemuxer demuxer;
    GapProbe probe;
    demuxer.setDemuxerHandler(probe);

    constexpr size_t kChunk = 65536;
    std::vector<uint8_t> buf;
    for (;;) {
        size_t oldSz = buf.size();
        buf.resize(oldSz + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
        size_t got = static_cast<size_t>(ifs.gcount());
        buf.resize(oldSz + got);
        if (got == 0) break;
        Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            if (demuxer.demux(stream) == DemuxStatus::NotEnoughBuffer) break;
        }
        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) buf.erase(buf.begin(), buf.begin() + consumed);
    }

    printf("\nvideo AUs=%lld, span=%lld ms\n", probe.videoAus,
           (long long)(probe.lastVideoMs - probe.firstVideoMs));
    return 0;
}
