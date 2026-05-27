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
    if (m_audioStreamListLocked)
        return;

    int streamIndex = static_cast<int>(stream.getStreamIndex());
    if (m_requireAdtsConvertibleAudio &&
        std::find(m_adtsConvertibleAudioStreams.begin(),
                  m_adtsConvertibleAudioStreams.end(),
                  streamIndex) == m_adtsConvertibleAudioStreams.end()) {
        return;
    }

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

bool CFilterDemuxerHandler::shouldProcessAudioStream(int streamIndex) const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (!m_requireAdtsConvertibleAudio)
        return true;

    if (m_primaryAudioStreamIndex != -1)
        return m_primaryAudioStreamIndex == streamIndex;

    return std::find(m_adtsConvertibleAudioStreams.begin(),
                     m_adtsConvertibleAudioStreams.end(),
                     streamIndex) != m_adtsConvertibleAudioStreams.end();
}

void CFilterDemuxerHandler::rememberAdtsConvertibleAudioStream(int streamIndex)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (std::find(m_adtsConvertibleAudioStreams.begin(),
                  m_adtsConvertibleAudioStreams.end(),
                  streamIndex) == m_adtsConvertibleAudioStreams.end()) {
        m_adtsConvertibleAudioStreams.push_back(streamIndex);
        LogMsg(L"MMT/TLV Audio ADTS conversion available: streamIndex=%d\n", streamIndex);
    }
}

void CFilterDemuxerHandler::rememberSubtitleStream(const MmtTlv::MmtStream& stream)
{
    std::lock_guard<std::mutex> lock(m_subtitleMutex);
    int streamIndex = static_cast<int>(stream.getStreamIndex());
    auto it = std::find_if(m_subtitleStreams.begin(), m_subtitleStreams.end(),
        [streamIndex](const SubtitleStreamInfo& info) {
            return info.streamIndex == streamIndex;
        });
    if (it != m_subtitleStreams.end()) {
        if (!it->hasData) {
            it->hasData = true;
            LogMsg(L"MMT/TLV Subtitle data seen streamIndex=%d, packetId=0x%04X, componentTag=%d\n",
                   it->streamIndex, it->packetId, it->componentTag);
        }
        return;
    }

    SubtitleStreamInfo info;
    info.streamIndex = streamIndex;
    info.packetId = stream.getPacketId();
    info.componentTag = stream.getComponentTag();
    info.hasData = true;
    m_subtitleStreams.push_back(info);
    LogMsg(L"MMT/TLV Subtitle discovered streamIndex=%d, packetId=0x%04X, componentTag=%d, data=1\n",
           info.streamIndex, info.packetId, info.componentTag);
}

void CFilterDemuxerHandler::onMpt(const MmtTlv::Mpt& mpt)
{
    std::vector<AudioStreamInfo> discovered;
    std::vector<SubtitleStreamInfo> discoveredSubtitles;
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
        } else if (asset.assetType == MmtTlv::AssetType::stpp) {
            SubtitleStreamInfo info;
            info.streamIndex = streamIndex;
            info.packetId = packetId;

            for (const auto& descriptor : asset.descriptors.list) {
                if (descriptor->getDescriptorTag() == MmtTlv::MhStreamIdentificationDescriptor::kDescriptorTag) {
                    const auto* streamId = static_cast<const MmtTlv::MhStreamIdentificationDescriptor*>(descriptor.get());
                    info.componentTag = streamId->componentTag;
                    break;
                }
            }

            discoveredSubtitles.push_back(info);
        }

        ++streamIndex;
    }

    if (!discovered.empty()) {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        if (m_requireAdtsConvertibleAudio) {
            discovered.erase(std::remove_if(discovered.begin(), discovered.end(),
                [this](const AudioStreamInfo& info) {
                    return std::find(m_adtsConvertibleAudioStreams.begin(),
                                     m_adtsConvertibleAudioStreams.end(),
                                     info.streamIndex) == m_adtsConvertibleAudioStreams.end();
                }),
                discovered.end());
        }
        if (discovered.empty())
            return;

        if (m_audioStreamListLocked) {
            bool missing = false;
            for (const auto& info : discovered) {
                auto it = std::find_if(m_audioStreams.begin(), m_audioStreams.end(),
                    [&info](const AudioStreamInfo& known) {
                        return known.streamIndex == info.streamIndex;
                    });
                if (it == m_audioStreams.end()) {
                    missing = true;
                    break;
                }
            }
            if (missing) {
                LogMsg(L"MMT/TLV Audio MPT changed after pin creation; keeping PreScan audio list (%zu stream(s))\n",
                       m_audioStreams.size());
            }
            return;
        }

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

        if (changed) {
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
    }

    if (!discoveredSubtitles.empty()) {
        std::lock_guard<std::mutex> lock(m_subtitleMutex);
        for (auto& info : discoveredSubtitles) {
            auto it = std::find_if(m_subtitleStreams.begin(), m_subtitleStreams.end(),
                [&info](const SubtitleStreamInfo& known) {
                    return known.streamIndex == info.streamIndex;
                });
            if (it != m_subtitleStreams.end())
                info.hasData = it->hasData;
        }

        bool changed = discoveredSubtitles.size() != m_subtitleStreams.size();
        if (!changed) {
            for (size_t i = 0; i < discoveredSubtitles.size(); ++i) {
                if (discoveredSubtitles[i].streamIndex != m_subtitleStreams[i].streamIndex ||
                    discoveredSubtitles[i].packetId != m_subtitleStreams[i].packetId ||
                    discoveredSubtitles[i].componentTag != m_subtitleStreams[i].componentTag ||
                    discoveredSubtitles[i].hasData != m_subtitleStreams[i].hasData) {
                    changed = true;
                    break;
                }
            }
        }

        if (changed) {
            m_subtitleStreams = discoveredSubtitles;
            LogMsg(L"MMT/TLV Subtitle MPT updated: assets=%u, subtitleStreams=%zu\n",
                   static_cast<unsigned>(mpt.assets.size()), m_subtitleStreams.size());
            for (size_t i = 0; i < m_subtitleStreams.size(); ++i) {
                const auto& info = m_subtitleStreams[i];
                LogMsg(L"MMT/TLV Subtitle MPT stream[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, data=%d\n",
                       i, info.streamIndex, info.packetId, info.componentTag, info.hasData ? 1 : 0);
            }
        }
    }
}

std::vector<CFilterDemuxerHandler::AudioStreamInfo> CFilterDemuxerHandler::getAudioStreams() const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    return m_audioStreams;
}

std::vector<CFilterDemuxerHandler::SubtitleStreamInfo> CFilterDemuxerHandler::getSubtitleStreams() const
{
    std::lock_guard<std::mutex> lock(m_subtitleMutex);
    return m_subtitleStreams;
}

void CFilterDemuxerHandler::resetAudioSelection()
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_primaryAudioStreamIndex = -1;
    m_audioStreams.clear();
    m_adtsConvertibleAudioStreams.clear();
    m_requireAdtsConvertibleAudio = false;
    m_audioStreamListLocked = false;
    {
        std::lock_guard<std::mutex> subLock(m_subtitleMutex);
        m_subtitleStreams.clear();
    }
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

void CFilterDemuxerHandler::setRequireAdtsConvertibleAudio(bool require)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_requireAdtsConvertibleAudio = require;
    if (require) {
        for (const auto& info : m_audioStreams) {
            if (std::find(m_adtsConvertibleAudioStreams.begin(),
                          m_adtsConvertibleAudioStreams.end(),
                          info.streamIndex) == m_adtsConvertibleAudioStreams.end()) {
                m_adtsConvertibleAudioStreams.push_back(info.streamIndex);
            }
        }
    }
}

void CFilterDemuxerHandler::setAudioStreamListLocked(bool locked)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_audioStreamListLocked = locked;
    LogMsg(L"MMT/TLV Audio stream list %s\n", locked ? L"locked" : L"unlocked");
}

std::vector<CFilterDemuxerHandler::AudioStreamInfo> CFilterDemuxerHandler::getAdtsConvertibleAudioStreams() const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    std::vector<AudioStreamInfo> streams;
    for (const auto& info : m_audioStreams) {
        if (std::find(m_adtsConvertibleAudioStreams.begin(),
                      m_adtsConvertibleAudioStreams.end(),
                      info.streamIndex) != m_adtsConvertibleAudioStreams.end()) {
            streams.push_back(info);
        }
    }
    return streams;
}

void CFilterDemuxerHandler::setKnownSubtitleStreams(const std::vector<SubtitleStreamInfo>& streams)
{
    std::lock_guard<std::mutex> lock(m_subtitleMutex);
    m_subtitleStreams = streams;
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
    const int streamIndex = static_cast<int>(stream.getStreamIndex());

    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        if (m_primaryAudioStreamIndex == -1) {
            m_primaryAudioStreamIndex = streamIndex;
            LogMsg(L"MMT/TLV Audio selected streamIndex=%d\n", m_primaryAudioStreamIndex);
        }
    }

    if (!shouldProcessAudioStream(streamIndex))
        return;

    // Convert LOAS/LATM → ADTS
    std::vector<uint8_t> adts;
    if (!m_adtsConverter.convert(mfu.data.data(), mfu.data.size(), adts) || adts.empty()) {
        static volatile LONG s_audioConvertFails = 0;
        LONG failNo = InterlockedIncrement(&s_audioConvertFails);
        if (failNo <= 20 || (failNo % 100) == 0) {
            WCHAR hex[64]{};
            WCHAR* cursor = hex;
            size_t remaining = ARRAYSIZE(hex);
            const size_t previewSize = (std::min)(mfu.data.size(), static_cast<size_t>(8));
            for (size_t i = 0; i < previewSize && remaining > 4; ++i) {
                HRESULT hr = StringCchPrintfW(cursor, remaining, L"%02X ", mfu.data[i]);
                if (FAILED(hr))
                    break;
                size_t used = wcslen(cursor);
                cursor += used;
                remaining -= used;
            }
            LogMsg(L"MMT/TLV Audio ADTS convert failed #%ld: streamIndex=%d, packetId=0x%04X, size=%zu, samplingRate=%u, firstBytes=%s\n",
                   failNo,
                   static_cast<int>(stream.getStreamIndex()),
                   stream.getPacketId(),
                   mfu.data.size(),
                   stream.getSamplingRate(),
                   hex);
        }
        return;
    }
    rememberAdtsConvertibleAudioStream(streamIndex);

    long long pts = toRefTime(static_cast<int64_t>(mfu.pts), stream);
    long long dts = toRefTime(static_cast<int64_t>(mfu.dts), stream);

    m_audioCallback(streamIndex,
                    false, pts, dts, mfu.isFirstFragment, mfu.isLastFragment,
                    adts.data(), adts.size());
}

void CFilterDemuxerHandler::onSubtitleData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu)
{
    if (!m_subtitleCallback || mfu.data.empty())
        return;

    rememberSubtitleStream(stream);

    long long pts = toRefTime(static_cast<int64_t>(mfu.pts), stream);
    long long dts = toRefTime(static_cast<int64_t>(mfu.dts), stream);

    m_subtitleCallback(static_cast<int>(stream.getStreamIndex()),
                       true, pts, dts, true, true,
                       mfu.data.data(), mfu.data.size());
}
