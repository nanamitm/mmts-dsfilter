#include "DsHandler.h"
#include "mmtStream.h"
#include "mpt.h"
#include "mmtDescriptors.h"
#include "mhStreamIdentificationDescriptor.h"
#include "mhAudioComponentDescriptor.h"
#include "mpuProcessorBase.h"  // NOPTS_VALUE
#include <windows.h>
#include <strsafe.h>
#include <algorithm>

static void LogMsg(const WCHAR* format, ...)
{
    WCHAR buf[512];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), format, args);
    va_end(args);
    OutputDebugStringW(buf);
}

long long CFilterDemuxerHandler::toRefTime(int64_t pts, const MmtTlv::MmtStream& stream)
{
    if (pts == static_cast<int64_t>(MmtTlv::NOPTS_VALUE)) {
        return -1;
    }

    const auto& tb = stream.getTimeBase();
    if (tb.den <= 0) {
        // fallback: assume 90kHz (100ns units = pts * 10000000.0 / 90000.0)
        double val = (static_cast<double>(pts) * 10000000.0) / 90000.0;
        return static_cast<long long>(val);
    }

    // Use double math to prevent 64-bit integer overflow when pts is a very large NTP timestamp
    double val = (static_cast<double>(pts) * 10000000.0 * tb.num) / tb.den;
    return static_cast<long long>(val);
}

void CFilterDemuxerHandler::rememberAudioStream(const MmtTlv::MmtStream& stream)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    int streamIndex = static_cast<int>(stream.getStreamIndex());
    auto it = std::find_if(m_audioStreams.begin(), m_audioStreams.end(),
        [streamIndex](const AudioStreamInfo& info) {
            return info.streamIndex == streamIndex;
        });
    if (it != m_audioStreams.end())
        return;

    AudioStreamInfo info;
    info.streamIndex = streamIndex;
    info.packetId = stream.getPacketId();
    info.componentTag = stream.getComponentTag();
    info.samplingRate = stream.getSamplingRate();
    m_audioStreams.push_back(info);
    LogMsg(L"MMT/TLV Audio discovered streamIndex=%d, packetId=0x%04X, componentTag=%d, samplingRate=%u\n",
           info.streamIndex, info.packetId, info.componentTag, info.samplingRate);
}

void CFilterDemuxerHandler::onMpt(const MmtTlv::Mpt& mpt)
{
    std::vector<AudioStreamInfo> discovered;
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

        const bool isSupportedStream =
            asset.assetType == MmtTlv::AssetType::hev1 ||
            asset.assetType == MmtTlv::AssetType::mp4a ||
            asset.assetType == MmtTlv::AssetType::stpp ||
            asset.assetType == MmtTlv::AssetType::aapp;

        if (!isSupportedStream)
            continue;

        if (asset.assetType == MmtTlv::AssetType::mp4a) {
            AudioStreamInfo info;
            info.streamIndex = streamIndex;
            info.packetId = packetId;

            for (const auto& descriptor : asset.descriptors.list) {
                switch (descriptor->getDescriptorTag()) {
                case MmtTlv::MhStreamIdentificationDescriptor::kDescriptorTag:
                {
                    const auto* streamId = static_cast<const MmtTlv::MhStreamIdentificationDescriptor*>(descriptor.get());
                    info.componentTag = streamId->componentTag;
                    break;
                }
                case MmtTlv::MhAudioComponentDescriptor::kDescriptorTag:
                {
                    const auto* audio = static_cast<const MmtTlv::MhAudioComponentDescriptor*>(descriptor.get());
                    info.componentTag = audio->componentTag;
                    info.samplingRate = audio->getConvertedSamplingRate();
                    break;
                }
                }
            }

            discovered.push_back(info);
        }

        ++streamIndex;
    }

    if (discovered.empty())
        return;

    std::lock_guard<std::mutex> lock(m_audioMutex);
    bool changed = discovered.size() != m_audioStreams.size();
    if (!changed) {
        for (size_t i = 0; i < discovered.size(); ++i) {
            if (discovered[i].streamIndex != m_audioStreams[i].streamIndex ||
                discovered[i].packetId != m_audioStreams[i].packetId ||
                discovered[i].componentTag != m_audioStreams[i].componentTag) {
                changed = true;
                break;
            }
        }
    }

    if (!changed)
        return;

    m_audioStreams = discovered;
    if (m_primaryAudioStreamIndex == -1 && !m_audioStreams.empty()) {
        m_primaryAudioStreamIndex = m_audioStreams.front().streamIndex;
    }

    LogMsg(L"MMT/TLV Audio MPT updated: assets=%u, audioStreams=%zu, defaultStreamIndex=%d\n",
           static_cast<unsigned>(mpt.assets.size()),
           m_audioStreams.size(),
           m_primaryAudioStreamIndex);
    for (size_t i = 0; i < m_audioStreams.size(); ++i) {
        const auto& info = m_audioStreams[i];
        LogMsg(L"MMT/TLV Audio MPT stream[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, samplingRate=%u\n",
               i, info.streamIndex, info.packetId, info.componentTag, info.samplingRate);
    }
}

std::vector<CFilterDemuxerHandler::AudioStreamInfo> CFilterDemuxerHandler::getAudioStreams() const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    return m_audioStreams;
}

void CFilterDemuxerHandler::resetAudioSelection()
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_primaryAudioStreamIndex = -1;
    m_audioStreams.clear();
}

void CFilterDemuxerHandler::setKnownAudioStreams(const std::vector<AudioStreamInfo>& streams)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_audioStreams = streams;
    if (m_primaryAudioStreamIndex == -1 && !m_audioStreams.empty()) {
        m_primaryAudioStreamIndex = m_audioStreams.front().streamIndex;
        LogMsg(L"MMT/TLV Audio default streamIndex=%d\n", m_primaryAudioStreamIndex);
    }
}

int CFilterDemuxerHandler::getSelectedAudioStreamIndex() const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    return m_primaryAudioStreamIndex;
}

bool CFilterDemuxerHandler::selectAudioStreamByListIndex(size_t listIndex)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (listIndex >= m_audioStreams.size())
        return false;

    m_primaryAudioStreamIndex = m_audioStreams[listIndex].streamIndex;
    LogMsg(L"MMT/TLV Audio selected by IAMStreamSelect: list=%zu, streamIndex=%d\n",
           listIndex, m_primaryAudioStreamIndex);
    return true;
}

void CFilterDemuxerHandler::onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu)
{
    if (!m_videoCallback || mfu.data.empty())
        return;

    long long pts = toRefTime(static_cast<int64_t>(mfu.pts), stream);
    long long dts = toRefTime(static_cast<int64_t>(mfu.dts), stream);

    m_videoCallback(static_cast<int>(stream.getStreamIndex()),
                    mfu.keyframe, pts, dts, mfu.isFirstFragment, mfu.isLastFragment,
                    mfu.data.data(), mfu.data.size());
}

void CFilterDemuxerHandler::onAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu)
{
    if (!m_audioCallback || mfu.data.empty())
        return;

    rememberAudioStream(stream);

    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        if (m_primaryAudioStreamIndex == -1) {
            m_primaryAudioStreamIndex = static_cast<int>(stream.getStreamIndex());
            LogMsg(L"MMT/TLV Audio selected streamIndex=%d\n", m_primaryAudioStreamIndex);
        }
    }

    // Convert LOAS/LATM → ADTS
    std::vector<uint8_t> adts;
    if (!m_adtsConverter.convert(mfu.data.data(), mfu.data.size(), adts) || adts.empty())
        return;

    long long pts = toRefTime(static_cast<int64_t>(mfu.pts), stream);
    long long dts = toRefTime(static_cast<int64_t>(mfu.dts), stream);

    m_audioCallback(static_cast<int>(stream.getStreamIndex()),
                    false, pts, dts, mfu.isFirstFragment, mfu.isLastFragment,
                    adts.data(), adts.size());
}
