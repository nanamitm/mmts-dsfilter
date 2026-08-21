// mmts_probe2.cpp
// Uses dantto4k's actual MmtTlvDemuxer to analyze an MMTS file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <algorithm>

#include "mmtTlvDemuxer.h"
#include "demuxerHandler.h"
#include "mmtStream.h"
#include "mpuProcessorBase.h"  // NOPTS_VALUE, MfuData
#include "stream.h"
#include "timebase.h"
#include "TtmlModel.h"

using namespace MmtTlv;

static int64_t ptsToMs(uint64_t pts90k, const MmtStream& stream) {
    if (pts90k == NOPTS_VALUE) return -1;
    const auto& tb = stream.getTimeBase();
    if (tb.den <= 0) {
        // fallback: assume 90kHz
        return (int64_t)pts90k / 90;
    }
    // pts is in tb.num/tb.den seconds
    // ms = pts * tb.num * 1000 / tb.den
    return (int64_t)((double)pts90k * tb.num * 1000.0 / tb.den);
}

class ProbeHandler : public DemuxerHandler {
public:
    struct StreamInfo {
        std::string type;
        uint64_t    frameCount = 0;
        int64_t     firstPtsMs = -1;
        int64_t     lastPtsMs  = -1;
        bool        seenKey    = false;
        uint64_t    firstPtsRaw = NOPTS_VALUE;
        uint64_t    lastPtsRaw  = NOPTS_VALUE;
        uint16_t    numOfAuErrors = 0;
    };
    std::map<int, StreamInfo> streams;
    int subtitleSamples = 0;

    // DsTtml::Parse() resolved the CSS variants, so a present pair is always a
    // pair of lengths. As before, the unit is not inspected here.
    static bool getLengthPair(const std::optional<DsTtml::LengthPair>& pair, double& x, double& y)
    {
        if (!pair.has_value())
            return false;

        x = pair->first.value;
        y = pair->second.value;
        return true;
    }

    void onVideoData(const MmtStream& stream, const MfuData& mfu) override {
        auto& st = streams[mfu.streamIndex];
        st.type = "Video";
        if (mfu.pts != NOPTS_VALUE) {
            int64_t ms = ptsToMs(mfu.pts, stream);
            if (st.firstPtsMs < 0) {
                st.firstPtsMs = ms;
                st.firstPtsRaw = mfu.pts;
                const auto& tb = stream.getTimeBase();
                printf("  [Video Diagnostic] firstPtsRaw=%llu, tb.num=%d, tb.den=%d\n",
                       (unsigned long long)mfu.pts, tb.num, tb.den);
            }
            if (mfu.isLastFragment) {
                st.frameCount++;
                if (ms > st.lastPtsMs) {
                    st.lastPtsMs = ms;
                    st.lastPtsRaw = mfu.pts;
                }
            }
        }
        if (mfu.keyframe) st.seenKey = true;
    }

    void onAudioData(const MmtStream& stream, const MfuData& mfu) override {
        auto& st = streams[mfu.streamIndex];
        st.type = "Audio";
        if (mfu.pts != NOPTS_VALUE) {
            int64_t ms = ptsToMs(mfu.pts, stream);
            if (st.firstPtsMs < 0) {
                st.firstPtsMs = ms;
                auto descOpt = stream.getMhAudioComponentDescriptor();
                int mode = -1;
                int type = -1;
                if (descOpt) {
                    mode = descOpt->get().getAudioMode();
                    type = descOpt->get().componentType;
                }
                printf("  [Audio Diagnostic] rate=%u, is22_2ch=%d, audioMode=%d, componentType=%d\n",
                       stream.getSamplingRate(), stream.is22_2chAudio(), mode, type);
            }
            if (mfu.isLastFragment) {
                st.frameCount++;
                if (ms > st.lastPtsMs) st.lastPtsMs = ms;
            }
        }
    }

    void onSubtitleData(const MmtStream& stream, const MfuData& mfu) override {
        auto& st = streams[mfu.streamIndex];
        st.type = "Subtitle";
        st.frameCount++;

        if (subtitleSamples >= 8)
            return;
        ++subtitleSamples;

        std::string xml(mfu.data.begin(), mfu.data.end());
        const DsTtml::Document ttml = DsTtml::Parse(xml);

        double maxOriginX = 0, maxOriginY = 0, maxExtentX = 0, maxExtentY = 0;
        double maxFontX = 0, maxFontY = 0;
        size_t divs = 0, paragraphs = 0, spans = 0;
        printf("  [Subtitle TTML #%d] streamIndex=%d bytes=%zu\n",
               subtitleSamples, mfu.streamIndex, mfu.data.size());

        for (const auto& div : ttml.divTags) {
            ++divs;
            for (const auto& p : div.pTags) {
                ++paragraphs;
                double x = 0, y = 0;
                if (getLengthPair(p.region.origin, x, y)) {
                    maxOriginX = std::max(maxOriginX, x);
                    maxOriginY = std::max(maxOriginY, y);
                }
                if (getLengthPair(p.region.extent, x, y)) {
                    maxExtentX = std::max(maxExtentX, x);
                    maxExtentY = std::max(maxExtentY, y);
                }
                for (const auto& span : p.spanTags) {
                    ++spans;
                    if (getLengthPair(span.style.fontSize, x, y)) {
                        maxFontX = std::max(maxFontX, x);
                        maxFontY = std::max(maxFontY, y);
                    }
                }
            }
        }

        printf("    divs=%zu p=%zu spans=%zu maxOrigin=%.1f,%.1f maxExtent=%.1f,%.1f maxFont=%.1f,%.1f\n",
               divs, paragraphs, spans, maxOriginX, maxOriginY, maxExtentX, maxExtentY,
               maxFontX, maxFontY);
        printf("    coordinateGuess=%s\n",
               (maxOriginX > 3840 || maxOriginY > 2160 || maxExtentX > 3840 || maxExtentY > 2160 ||
                maxFontX > 144 || maxFontY > 144) ? "possibly 8K/native" : "4K-logical-like");
    }

    void print(const char* label) const {
        printf("%s:\n", label);
        if (streams.empty()) {
            printf("  (no callbacks fired)\n");
            return;
        }
        for (auto& [idx, st] : streams) {
            printf("  [%d] %-7s frames=%6llu  firstPts=%8lld ms  lastPts=%8lld ms  key=%s\n",
                   idx, st.type.c_str(),
                   (unsigned long long)st.frameCount,
                   st.firstPtsMs, st.lastPtsMs,
                   st.seenKey ? "YES" : "no");
        }
    }
};

static void demuxChunks(std::ifstream& ifs, MmtTlvDemuxer& demuxer, size_t maxBytes)
{
    constexpr size_t kChunk = 65536;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    size_t totalRead = 0;

    while (maxBytes == 0 || totalRead < maxBytes) {
        size_t oldSz = buf.size();
        size_t toRead = kChunk;
        if (maxBytes > 0 && totalRead + toRead > maxBytes)
            toRead = maxBytes - totalRead;
        buf.resize(oldSz + toRead);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), toRead);
        size_t got = (size_t)ifs.gcount();
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
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printf("Usage: mmts_probe2 <input.mmts>\n");
        return 1;
    }
    const char* filename = argv[1];

    printf("=== MMTS File Probe (dantto4k API) ===\n");
    printf("File: %s\n\n", filename);

    std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) { printf("ERROR: Cannot open file\n"); return 1; }
    std::streamsize fileSize = ifs.tellg();
    printf("File size: %.2f MB  (%lld bytes)\n\n", fileSize/1048576.0, (long long)fileSize);

    constexpr std::streamsize kTailSize = 20 * 1024 * 1024;
    std::streamsize tailOffset = fileSize > kTailSize ? fileSize - kTailSize : 0;

    // ---- Phase 1: beginning -------------------------------------------------
    printf("=== Phase 1: First 20MB (extract extradata + firstPts) ===\n");
    MmtTlvDemuxer dem1;
    ProbeHandler  h1;
    dem1.setDemuxerHandler(h1);
    ifs.seekg(0);
    demuxChunks(ifs, dem1, 20 * 1024 * 1024);
    h1.print("Phase 1 result");

    // ---- Phase 2: same demuxer, resetStreams, tail scan --------------------
    printf("\n=== Phase 2: Last 20MB, SAME demuxer + resetStreams ===\n");
    printf("(tail offset: %.2f MB)\n", tailOffset/1048576.0);
    dem1.resetStreams();
    ProbeHandler h2;
    dem1.setDemuxerHandler(h2);
    ifs.clear();
    ifs.seekg(tailOffset);
    demuxChunks(ifs, dem1, 0);
    h2.print("Phase 2 result (resetStreams)");

    // ---- Phase 3: fresh demuxer, tail scan --------------------------------
    printf("\n=== Phase 3: Last 20MB, FRESH demuxer ===\n");
    MmtTlvDemuxer dem3;
    ProbeHandler  h3;
    dem3.setDemuxerHandler(h3);
    ifs.clear();
    ifs.seekg(tailOffset);
    demuxChunks(ifs, dem3, 0);
    h3.print("Phase 3 result (fresh demuxer)");

    // ---- Duration calculation -----------------------------------------------
    printf("\n=== Duration ===\n");
    int64_t firstPtsMs = -1, lastPtsMs = -1;
    for (auto& [idx, st] : h1.streams)
        if (st.type == "Video" && st.firstPtsMs >= 0) { firstPtsMs = st.firstPtsMs; break; }
    for (auto& [idx, st] : h2.streams)
        if (st.type == "Video" && st.lastPtsMs >= 0) { lastPtsMs = st.lastPtsMs; break; }

    printf("firstPts (Phase1 video): %lld ms\n", firstPtsMs);
    printf("lastPts  (Phase2 video): %lld ms\n", lastPtsMs);
    if (firstPtsMs >= 0 && lastPtsMs >= firstPtsMs)
        printf("Duration: %lld ms  (%.2f s)\n", lastPtsMs - firstPtsMs, (lastPtsMs - firstPtsMs)/1000.0);
    else
        printf("Duration: CANNOT COMPUTE\n");

    printf("\nDone.\n");
    return 0;
}
