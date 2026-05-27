// mmts_audio_split.cpp
// Detects MPT audio-layout changes in an MMTS file and optionally splits the
// file at the approximate byte offsets where those changes appear.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "adtsConverter.h"
#include "demuxerHandler.h"
#include "mhAudioComponentDescriptor.h"
#include "mhStreamIdentificationDescriptor.h"
#include "mmtStream.h"
#include "mmtTlvDemuxer.h"
#include "mpt.h"
#include "mpuProcessorBase.h"

using namespace MmtTlv;

struct AudioTrack {
    int streamIndex{-1};
    uint16_t packetId{0};
    int componentTag{-1};
    uint32_t samplingRate{0};

    std::string key() const
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%d:%04X:%d:%u",
                      streamIndex, packetId, componentTag, samplingRate);
        return buf;
    }
};

static std::string signatureOf(const std::vector<AudioTrack>& tracks)
{
    std::ostringstream oss;
    for (const auto& t : tracks)
        oss << t.key() << ";";
    return oss.str();
}

static std::string describeTracks(const std::vector<AudioTrack>& tracks)
{
    std::ostringstream oss;
    if (tracks.empty()) {
        oss << "(none)";
        return oss.str();
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (i) oss << ", ";
        oss << "streamIndex=" << tracks[i].streamIndex
            << " packetId=0x" << std::hex << std::uppercase << tracks[i].packetId
            << std::dec << " componentTag=" << tracks[i].componentTag
            << " rate=" << tracks[i].samplingRate;
    }
    return oss.str();
}

class AudioChangeHandler : public DemuxerHandler {
public:
    struct Change {
        uint64_t offset{};
        std::vector<AudioTrack> tracks;
    };

    uint64_t currentOffset{};
    std::vector<Change> changes;
    std::map<int, bool> adtsConvertible;

    void onMpt(const Mpt& mpt) override
    {
        std::vector<AudioTrack> tracks;
        int streamIndex = 0;
        for (const auto& asset : mpt.assets) {
            bool hasPacketLocation = false;
            uint16_t packetId = 0;
            for (const auto& locationInfo : asset.locationInfos) {
                if (locationInfo.locationType == 0) {
                    hasPacketLocation = true;
                    packetId = locationInfo.packetId;
                    break;
                }
            }

            if (hasPacketLocation && asset.assetType == AssetType::mp4a) {
                AudioTrack info;
                info.streamIndex = streamIndex;
                info.packetId = packetId;
                for (const auto& descriptor : asset.descriptors.list) {
                    switch (descriptor->getDescriptorTag()) {
                    case MhStreamIdentificationDescriptor::kDescriptorTag:
                    {
                        const auto* streamId =
                            static_cast<const MhStreamIdentificationDescriptor*>(descriptor.get());
                        info.componentTag = streamId->componentTag;
                        break;
                    }
                    case MhAudioComponentDescriptor::kDescriptorTag:
                    {
                        const auto* audio =
                            static_cast<const MhAudioComponentDescriptor*>(descriptor.get());
                        info.componentTag = audio->componentTag;
                        info.samplingRate = audio->getConvertedSamplingRate();
                        break;
                    }
                    }
                }
                tracks.push_back(info);
            }

            if (hasPacketLocation &&
                (asset.assetType == AssetType::hev1 ||
                 asset.assetType == AssetType::mp4a ||
                 asset.assetType == AssetType::stpp ||
                 asset.assetType == AssetType::aapp)) {
                ++streamIndex;
            }
        }

        const std::string sig = signatureOf(tracks);
        if (sig.empty() || sig == lastSignature)
            return;

        lastSignature = sig;
        changes.push_back({currentOffset, tracks});
        std::printf("change #%zu offset=%llu audio=%s\n",
                    changes.size(),
                    static_cast<unsigned long long>(currentOffset),
                    describeTracks(tracks).c_str());
    }

    void onAudioData(const MmtStream& stream, const MfuData& mfu) override
    {
        const int streamIndex = static_cast<int>(stream.getStreamIndex());
        if (adtsConvertible.find(streamIndex) != adtsConvertible.end())
            return;

        std::vector<uint8_t> out;
        ADTSConverter converter;
        adtsConvertible[streamIndex] =
            converter.convert(mfu.data.data(), mfu.data.size(), out) && !out.empty();
    }

private:
    std::string lastSignature;
};

static void demuxFile(const std::filesystem::path& path, AudioChangeHandler& handler)
{
    constexpr size_t kChunk = 1024 * 1024;
    std::ifstream ifs(path, std::ios::binary);
    MmtTlvDemuxer demuxer;
    demuxer.setDemuxerHandler(handler);

    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    uint64_t totalRead = 0;

    while (ifs) {
        const size_t oldSize = buf.size();
        buf.resize(oldSize + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSize), kChunk);
        const size_t got = static_cast<size_t>(ifs.gcount());
        buf.resize(oldSize + got);
        if (got == 0)
            break;
        totalRead += got;

        Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            handler.currentOffset = totalRead - stream.leftBytes();
            const auto status = demuxer.demux(stream);
            if (status == DemuxStatus::NotEnoughBuffer)
                break;
        }

        const size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0)
            buf.erase(buf.begin(), buf.begin() + consumed);
    }
}

static bool copyRange(const std::filesystem::path& src,
                      const std::filesystem::path& dst,
                      uint64_t begin,
                      uint64_t end)
{
    constexpr size_t kChunk = 1024 * 1024;
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in || !out)
        return false;

    in.seekg(static_cast<std::streamoff>(begin));
    uint64_t remaining = end - begin;
    std::vector<char> buf(kChunk);
    while (remaining > 0) {
        const size_t toRead = static_cast<size_t>((std::min<uint64_t>)(remaining, buf.size()));
        in.read(buf.data(), toRead);
        const size_t got = static_cast<size_t>(in.gcount());
        if (got == 0)
            break;
        out.write(buf.data(), got);
        remaining -= got;
    }
    return true;
}

static void splitAtChanges(const std::filesystem::path& path,
                           const std::vector<AudioChangeHandler::Change>& changes)
{
    if (changes.size() <= 1) {
        std::printf("split: no audio-layout change found\n");
        return;
    }

    const uint64_t fileSize = std::filesystem::file_size(path);
    std::vector<uint64_t> cuts;
    cuts.push_back(0);
    for (size_t i = 1; i < changes.size(); ++i) {
        if (changes[i].offset > 0 && changes[i].offset < fileSize)
            cuts.push_back(changes[i].offset);
    }
    cuts.push_back(fileSize);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    const auto parent = path.parent_path();
    const auto stem = path.stem().wstring();
    const auto ext = path.extension().wstring();
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        wchar_t name[512];
        std::swprintf(name, 512, L"%s_part%03zu%s", stem.c_str(), i + 1, ext.c_str());
        const auto outPath = parent / name;
        if (!copyRange(path, outPath, cuts[i], cuts[i + 1])) {
            std::wprintf(L"split: failed to write %ls\n", outPath.c_str());
            return;
        }
        std::wprintf(L"split: wrote %ls bytes=%llu..%llu\n",
                     outPath.c_str(),
                     static_cast<unsigned long long>(cuts[i]),
                     static_cast<unsigned long long>(cuts[i + 1]));
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("Usage: mmts_audio_split <input.mmts> [--split]\n");
        return 1;
    }

    const std::filesystem::path input = std::filesystem::u8path(argv[1]);
    const bool doSplit = argc >= 3 && std::string(argv[2]) == "--split";

    AudioChangeHandler handler;
    demuxFile(input, handler);

    std::printf("summary: audio-layout changes=%zu\n", handler.changes.size());
    for (const auto& [streamIndex, ok] : handler.adtsConvertible) {
        std::printf("summary: streamIndex=%d adtsConvertible=%d\n",
                    streamIndex, ok ? 1 : 0);
    }

    if (doSplit)
        splitAtChanges(input, handler.changes);

    return 0;
}
