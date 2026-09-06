// mmts_cue_probe.cpp
// Lists the subtitle TTML cues from the start of a .mmts - byte position, TTML
// begin/end and a text preview - to see how a cue is timed.
//
// A cue with no end time is held until the next one, which the splitter has to
// find by scanning ahead. The byte distance to the next cue printed here is
// what tells you whether that scan can reach it (FindNextSubtitleBegin() gives
// up after 64 MB and falls back to repeating the cue in chunks).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "demuxerHandler.h"
#include "mmtTlvDemuxer.h"
#include "mmtStream.h"
#include "mpuProcessorBase.h"
#include "stream.h"

using namespace MmtTlv;

static std::string AttrOf(const std::string& xml, const char* name)
{
    const std::string key = std::string(" ") + name + "=\"";
    size_t p = xml.find(key);
    if (p == std::string::npos) return "-";
    p += key.size();
    size_t q = xml.find('"', p);
    if (q == std::string::npos) return "-";
    return xml.substr(p, q - p);
}

// Rough text extraction: everything between > and < that is not markup.
static std::string TextOf(const std::string& xml, size_t maxLen = 40)
{
    std::string out;
    size_t p = xml.find("<body");
    if (p == std::string::npos) p = 0;
    bool inTag = false;
    for (size_t i = p; i < xml.size() && out.size() < maxLen; ++i) {
        const char c = xml[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag && static_cast<unsigned char>(c) > 0x20) out += c;
    }
    return out;
}

static long long g_readBytes = 0;

class CueProbe : public DemuxerHandler {
public:
    long long count = 0;
    void onSubtitleData(const MmtStream& stream, const MfuData& mfu) override
    {
        std::string xml(mfu.data.begin(), mfu.data.end());
        ++count;
        std::printf("#%lld at~%lldMB si=%d pid=0x%04X tag=%d size=%zu begin=%s end=%s text=%s\n",
                    count, g_readBytes / (1024 * 1024), mfu.streamIndex, stream.getPacketId(), stream.getComponentTag(),
                    mfu.data.size(),
                    AttrOf(xml, "begin").c_str(), AttrOf(xml, "end").c_str(),
                    TextOf(xml).c_str());
    }
};

int main(int argc, char* argv[])
{
    if (argc < 2) { std::printf("Usage: mmts_cue_probe <input.mmts> [maxMB]\n"); return 1; }
    const long long maxBytes = (argc > 2 ? std::atoll(argv[2]) : 800) * 1024 * 1024;

    std::ifstream ifs(argv[1], std::ios::binary);
    if (!ifs.is_open()) { std::printf("ERROR: cannot open\n"); return 1; }

    MmtTlvDemuxer demuxer;
    CueProbe probe;
    demuxer.setDemuxerHandler(probe);

    constexpr size_t kChunk = 1024 * 1024;
    std::vector<uint8_t> buf;
    long long read = 0;
    while (read < maxBytes) {
        size_t oldSz = buf.size();
        buf.resize(oldSz + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
        size_t got = static_cast<size_t>(ifs.gcount());
        buf.resize(oldSz + got);
        if (got == 0) break;
        read += static_cast<long long>(got);
        g_readBytes = read;
        Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            if (demuxer.demux(stream) == DemuxStatus::NotEnoughBuffer) break;
        }
        size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0) buf.erase(buf.begin(), buf.begin() + consumed);
    }
    std::printf("\nread %lld MB, %lld subtitle sample(s)\n", read / (1024 * 1024), probe.count);
    return 0;
}
