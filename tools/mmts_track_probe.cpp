// mmts_track_probe.cpp
// Reports audio/subtitle MPT layout changes in an MMTS file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "demuxerHandler.h"
#include "mhAudioComponentDescriptor.h"
#include "mhStreamIdentificationDescriptor.h"
#include "mmtTlvDemuxer.h"
#include "mpt.h"

using namespace MmtTlv;

struct TrackInfo {
    std::string type;
    int streamIndex{-1};
    uint16_t packetId{0};
    int componentTag{-1};
    uint32_t samplingRate{0};

    std::string key() const
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s:%d:%04X:%d:%u",
                      type.c_str(), streamIndex, packetId, componentTag, samplingRate);
        return buf;
    }
};

static std::string signatureOf(const std::vector<TrackInfo>& tracks)
{
    std::ostringstream oss;
    for (const auto& t : tracks)
        oss << t.key() << ";";
    return oss.str();
}

static std::string describeTracks(const std::vector<TrackInfo>& tracks, const char* type)
{
    std::ostringstream oss;
    bool any = false;
    for (const auto& t : tracks) {
        if (t.type != type)
            continue;
        if (any)
            oss << ", ";
        any = true;
        oss << "streamIndex=" << t.streamIndex
            << " packetId=0x" << std::hex << std::uppercase << t.packetId
            << std::dec << " componentTag=" << t.componentTag;
        if (t.type == "audio")
            oss << " rate=" << t.samplingRate;
    }
    return any ? oss.str() : "(none)";
}

class TrackProbeHandler : public DemuxerHandler {
public:
    uint64_t currentOffset{};
    std::vector<TrackInfo> lastTracks;

    void onMpt(const Mpt& mpt) override
    {
        std::vector<TrackInfo> tracks;
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
            if (!hasPacketLocation)
                continue;

            const bool isCounted =
                asset.assetType == AssetType::hev1 ||
                asset.assetType == AssetType::mp4a ||
                asset.assetType == AssetType::stpp ||
                asset.assetType == AssetType::aapp;
            if (!isCounted)
                continue;

            if (asset.assetType == AssetType::mp4a || asset.assetType == AssetType::stpp) {
                TrackInfo info;
                info.type = (asset.assetType == AssetType::mp4a) ? "audio" : "subtitle";
                info.streamIndex = streamIndex;
                info.packetId = packetId;

                for (const auto& descriptor : asset.descriptors.list) {
                    if (descriptor->getDescriptorTag() == MhStreamIdentificationDescriptor::kDescriptorTag) {
                        const auto* streamId =
                            static_cast<const MhStreamIdentificationDescriptor*>(descriptor.get());
                        info.componentTag = streamId->componentTag;
                    } else if (asset.assetType == AssetType::mp4a &&
                               descriptor->getDescriptorTag() == MhAudioComponentDescriptor::kDescriptorTag) {
                        const auto* audio =
                            static_cast<const MhAudioComponentDescriptor*>(descriptor.get());
                        info.componentTag = audio->componentTag;
                        info.samplingRate = audio->getConvertedSamplingRate();
                    }
                }

                tracks.push_back(info);
            }

            ++streamIndex;
        }

        const std::string sig = signatureOf(tracks);
        if (sig.empty() || sig == lastSignature)
            return;

        lastSignature = sig;
        lastTracks = tracks;
        std::printf("change offset=%llu audio=%s subtitle=%s\n",
                    static_cast<unsigned long long>(currentOffset),
                    describeTracks(tracks, "audio").c_str(),
                    describeTracks(tracks, "subtitle").c_str());
    }

private:
    std::string lastSignature;
};

static bool parseUint64(const wchar_t* text, uint64_t& value)
{
    if (!text || !*text)
        return false;
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(text, &end, 10);
    if (!end || *end != L'\0')
        return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

static void printUsage()
{
    std::printf("Usage: mmts_track_probe <input.mmts> [--max-mb N] [--progress-mb N]\n");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        printUsage();
        return 1;
    }

    const std::filesystem::path input(argv[1]);
    uint64_t maxBytes = 0;
    uint64_t progressBytes = 512ULL * 1024 * 1024;

    for (int i = 2; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--max-mb") == 0) {
            uint64_t mb = 0;
            if (++i >= argc || !parseUint64(argv[i], mb)) {
                printUsage();
                return 1;
            }
            maxBytes = mb * 1024 * 1024;
        } else if (std::wcscmp(argv[i], L"--progress-mb") == 0) {
            uint64_t mb = 0;
            if (++i >= argc || !parseUint64(argv[i], mb)) {
                printUsage();
                return 1;
            }
            progressBytes = mb * 1024 * 1024;
        } else {
            printUsage();
            return 1;
        }
    }

    std::ifstream ifs(input, std::ios::binary);
    if (!ifs) {
        std::wprintf(L"error: cannot open %ls\n", input.c_str());
        return 1;
    }

    const uint64_t fileSize = std::filesystem::file_size(input);
    const uint64_t scanLimit = maxBytes == 0 ? fileSize : (std::min)(maxBytes, fileSize);
    uint64_t nextProgress = progressBytes;

    TrackProbeHandler handler;
    MmtTlvDemuxer demuxer;
    demuxer.setDemuxerHandler(handler);

    constexpr size_t kChunk = 1024 * 1024;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    uint64_t totalRead = 0;

    while (ifs && totalRead < scanLimit) {
        const size_t oldSize = buf.size();
        const uint64_t left = scanLimit - totalRead;
        const size_t toRead = static_cast<size_t>((std::min<uint64_t>)(kChunk, left));
        buf.resize(oldSize + toRead);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSize), toRead);
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

        if (progressBytes > 0 && totalRead >= nextProgress) {
            std::printf("progress: %.1f/%.1f MB (%.1f%%)\n",
                        totalRead / 1048576.0,
                        scanLimit / 1048576.0,
                        scanLimit > 0 ? (totalRead * 100.0 / scanLimit) : 100.0);
            while (nextProgress <= totalRead)
                nextProgress += progressBytes;
        }
    }

    size_t audioCount = 0;
    size_t subtitleCount = 0;
    for (const auto& t : handler.lastTracks) {
        if (t.type == "audio")
            ++audioCount;
        else if (t.type == "subtitle")
            ++subtitleCount;
    }

    std::printf("summary: final audio=%zu subtitle=%zu\n", audioCount, subtitleCount);
    std::printf("summary: final audio tracks=%s\n",
                describeTracks(handler.lastTracks, "audio").c_str());
    std::printf("summary: final subtitle tracks=%s\n",
                describeTracks(handler.lastTracks, "subtitle").c_str());
    return 0;
}
