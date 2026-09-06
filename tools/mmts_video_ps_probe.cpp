// mmts_video_ps_probe.cpp
// Walks a whole .mmts with ONE continuous demuxer (like playback does) and
// reports, per complete video access unit, which HEVC parameter sets are
// in-band and whether they change. Used to investigate why video stops after
// a mid-recording channel switch.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "demuxerHandler.h"
#include "mmtTlvDemuxer.h"
#include "mmtStream.h"
#include "mpuProcessorBase.h"
#include "stream.h"

using namespace MmtTlv;

static std::string HexOf(const std::vector<uint8_t>& v, size_t maxBytes = 16)
{
    std::string s;
    char buf[4];
    for (size_t i = 0; i < v.size() && i < maxBytes; ++i) {
        snprintf(buf, sizeof(buf), "%02X", v[i]);
        s += buf;
    }
    if (v.size() > maxBytes) s += "..";
    return s;
}

class PsProbe : public DemuxerHandler {
public:
    struct Acc {
        std::vector<uint8_t> data;
        bool active = false;
    };
    std::map<int, Acc> acc;

    uint16_t lastPacketId = 0;
    bool havePacketId = false;
    std::vector<uint8_t> lastVps, lastSps, lastPps;
    uint64_t auCount = 0;
    uint64_t auSincePacketIdChange = 0;
    uint64_t auWithParamSets = 0;

    void onVideoData(const MmtStream& stream, const MfuData& mfu) override
    {
        const uint16_t pid = stream.getPacketId();
        const int si = mfu.streamIndex;

        if (!havePacketId || pid != lastPacketId) {
            printf("\n=== video packetId %s0x%04X (streamIndex=%d) at AU #%llu ===\n",
                   havePacketId ? "changed -> " : "= ", pid, si,
                   (unsigned long long)auCount);
            havePacketId = true;
            lastPacketId = pid;
            auSincePacketIdChange = 0;
        }

        Acc& a = acc[si];
        if (mfu.isFirstFragment) { a.data.clear(); a.active = true; }
        a.data.insert(a.data.end(), mfu.data.begin(), mfu.data.end());
        if (!mfu.isLastFragment) return;
        if (a.data.empty()) return;

        ++auCount;
        ++auSincePacketIdChange;
        analyzeAu(a.data, pid, si, mfu.keyframe);
        a.data.clear();
    }

    void analyzeAu(const std::vector<uint8_t>& au, uint16_t pid, int si, bool key)
    {
        std::vector<uint8_t> vps, sps, pps;
        std::vector<int> types;
        const uint8_t* p = au.data();
        const size_t sz = au.size();
        size_t i = 0;
        size_t prevStart = SIZE_MAX;
        int prevType = -1;
        auto flush = [&](size_t end) {
            if (prevStart == SIZE_MAX) return;
            std::vector<uint8_t> nalu(p + prevStart, p + end);
            if (prevType == 32 && vps.empty()) vps = nalu;
            else if (prevType == 33 && sps.empty()) sps = nalu;
            else if (prevType == 34 && pps.empty()) pps = nalu;
        };
        while (i + 4 <= sz) {
            size_t start = 0;
            if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) start = i + 3;
            else if (i + 5 <= sz && p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1) start = i + 4;
            if (start == 0) { ++i; continue; }
            flush(i);
            prevType = (p[start] >> 1) & 0x3F;
            prevStart = start;
            types.push_back(prevType);
            i = start + 2;
        }
        flush(sz);

        const bool hasPs = !vps.empty() || !sps.empty() || !pps.empty();
        if (hasPs) ++auWithParamSets;

        const bool spsChanged = !sps.empty() && sps != lastSps;
        const bool ppsChanged = !pps.empty() && pps != lastPps;
        const bool vpsChanged = !vps.empty() && vps != lastVps;

        // Report the first few AUs after a packetId change, plus any parameter
        // set change anywhere in the file.
        if (auSincePacketIdChange <= 3 || spsChanged || ppsChanged || vpsChanged) {
            printf("AU #%llu (pid=0x%04X si=%d key=%d) size=%zu nals=",
                   (unsigned long long)auCount, pid, si, key ? 1 : 0, au.size());
            for (size_t k = 0; k < types.size() && k < 12; ++k) printf("%d ", types[k]);
            printf("\n   VPS=%s%s SPS=%s%s PPS=%s%s\n",
                   vps.empty() ? "-" : HexOf(vps).c_str(), vpsChanged ? " (CHANGED)" : "",
                   sps.empty() ? "-" : HexOf(sps).c_str(), spsChanged ? " (CHANGED)" : "",
                   pps.empty() ? "-" : HexOf(pps).c_str(), ppsChanged ? " (CHANGED)" : "");
        }
        if (!vps.empty()) lastVps = vps;
        if (!sps.empty()) lastSps = sps;
        if (!pps.empty()) lastPps = pps;
    }
};

int main(int argc, char* argv[])
{
    if (argc < 2) { printf("Usage: mmts_video_ps_probe <input.mmts>\n"); return 1; }

    std::ifstream ifs(argv[1], std::ios::binary);
    if (!ifs.is_open()) { printf("ERROR: cannot open %s\n", argv[1]); return 1; }

    MmtTlvDemuxer demuxer;
    PsProbe probe;
    demuxer.setDemuxerHandler(probe);

    constexpr size_t kChunk = 65536;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    for (;;) {
        size_t oldSz = buf.size();
        buf.resize(oldSz + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
        size_t got = static_cast<size_t>(ifs.gcount());
        buf.resize(oldSz + got);
        if (got == 0) break;

        Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            auto status = demuxer.demux(stream);
            if (status == DemuxStatus::NotEnoughBuffer) break;
        }
        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) buf.erase(buf.begin(), buf.begin() + consumed);
    }

    printf("\n=== summary ===\n");
    printf("total video AUs = %llu, AUs carrying parameter sets = %llu\n",
           (unsigned long long)probe.auCount,
           (unsigned long long)probe.auWithParamSets);
    return 0;
}
