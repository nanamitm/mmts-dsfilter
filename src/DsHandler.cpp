#include "DsHandler.h"
#include "DebugLog.h"
#include "mmtStream.h"
#include "mpt.h"
#include "mhEit.h"
#include "ntp.h"
#include "mmtDescriptors.h"
#include "mhStreamIdentificationDescriptor.h"
#include "mhAudioComponentDescriptor.h"
#include "videoComponentDescriptor.h"
#include "mpuProcessorBase.h"  // NOPTS_VALUE
#include "timeUtil.h"
#include <windows.h>
#include <strsafe.h>
#include <algorithm>
#include <ctime>

#define LogMsg MmtTlvLogInfo
#define LogDetail MmtTlvLogDebug

static bool IsCaptionComponentTag(int componentTag)
{
    return componentTag >= 0x30 && componentTag <= 0x37;
}

static uint16_t AudioModeToChannels(uint8_t audioMode)
{
    switch (audioMode) {
    case 0b00001: return 1;   // single mono
    case 0b00010: return 2;   // dual mono
    case 0b00011: return 2;   // stereo
    case 0b00100: return 3;   // 2/1
    case 0b00101: return 3;   // 3ch
    case 0b00110: return 4;   // 2/2
    case 0b00111: return 4;   // 4ch
    case 0b01000: return 5;   // 5ch
    case 0b01001: return 6;   // 5.1ch
    case 0b01010: return 7;   // 3/3.1
    case 0b01011: return 7;   // 6.1ch
    case 0b01100:
    case 0b01101:
    case 0b01110:
    case 0b01111: return 8;   // 7.1ch variants
    case 0b10000: return 12;  // 10.2ch
    case 0b10001: return 24;  // 22.2ch
    default: return 2;
    }
}

// ARIB STD-B10 video_resolution / video_aspect_ratio, as carried by the MMT
// video component descriptor.
static void VideoResolutionSize(uint8_t videoResolution, uint8_t videoAspectRatio,
                                int* width, int* height)
{
    int h = 0;
    switch (videoResolution) {
    case 1: h = 180; break;
    case 2: h = 240; break;
    case 3: h = 480; break;
    case 4: h = 720; break;
    case 5: h = 1080; break;
    case 6: h = 2160; break;
    case 7: h = 4320; break;
    default: h = 0; break;
    }

    const bool wide = (videoAspectRatio == 2 || videoAspectRatio == 3);
    if (width)
        *width = static_cast<int>(h * (wide ? 16.0 / 9.0 : 4.0 / 3.0));
    if (height)
        *height = h;
}

static bool SameVideoIdentity(const CFilterDemuxerHandler::VideoStreamInfo& a,
                              const CFilterDemuxerHandler::VideoStreamInfo& b)
{
    if (a.packetId != b.packetId)
        return false;
    if (a.componentTag >= 0 && b.componentTag >= 0)
        return a.componentTag == b.componentTag;
    return true;
}

static long long Pcr27ToRefTime(int64_t pcr27)
{
    if (pcr27 < 0)
        return -1;
    return static_cast<long long>((static_cast<double>(pcr27) * 10000000.0) / 27000000.0);
}

static uint16_t AudioDescriptorChannels(const MmtTlv::MhAudioComponentDescriptor& audio)
{
    return AudioModeToChannels(audio.getAudioMode());
}

static bool SameAudioIdentity(const CFilterDemuxerHandler::AudioStreamInfo& a,
                              const CFilterDemuxerHandler::AudioStreamInfo& b)
{
    if (a.packetId != b.packetId)
        return false;
    if (a.componentTag >= 0 && b.componentTag >= 0)
        return a.componentTag == b.componentTag;
    return true;
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

void CFilterDemuxerHandler::selectDefaultVideoStreamLocked()
{
    if (m_videoStreams.empty())
        return;

    // ARIB assigns component_tag 0 to the main video; a simulcast or secondary
    // view gets a higher tag. Prefer the lowest tag, falling back to MPT order
    // when no tag is known.
    auto best = m_videoStreams.begin();
    for (auto it = m_videoStreams.begin(); it != m_videoStreams.end(); ++it) {
        const int bestTag = best->componentTag < 0 ? 0x7FFFFFFF : best->componentTag;
        const int tag = it->componentTag < 0 ? 0x7FFFFFFF : it->componentTag;
        if (tag < bestTag)
            best = it;
    }

    if (m_hasSelectedVideoStream &&
        m_selectedVideoPacketId == best->packetId &&
        m_selectedVideoComponentTag == best->componentTag) {
        return;
    }

    m_selectedVideoPacketId = best->packetId;
    m_selectedVideoComponentTag = best->componentTag;
    m_hasSelectedVideoStream = true;
    LogMsg(L"MMT/TLV Video selected packetId=0x%04X componentTag=%d streamIndex=%d %dx%d\n",
           best->packetId, best->componentTag, best->streamIndex, best->width, best->height);
}

void CFilterDemuxerHandler::rememberVideoStream(const MmtTlv::MmtStream& stream)
{
    std::lock_guard<std::mutex> lock(m_videoMutex);

    VideoStreamInfo info;
    info.streamIndex = static_cast<int>(stream.getStreamIndex());
    info.packetId = stream.getPacketId();
    info.componentTag = stream.getComponentTag();
    info.hasData = true;
    if (auto desc = stream.getVideoComponentDescriptor()) {
        VideoResolutionSize(desc->get().videoResolution, desc->get().videoAspectRatio,
                            &info.width, &info.height);
    }

    auto it = std::find_if(m_videoStreams.begin(), m_videoStreams.end(),
        [&info](const VideoStreamInfo& known) {
            return SameVideoIdentity(known, info);
        });
    if (it != m_videoStreams.end()) {
        it->streamIndex = info.streamIndex;
        it->hasData = true;
        if (info.height > 0) {
            it->width = info.width;
            it->height = info.height;
        }
        return;
    }

    m_videoStreams.push_back(info);
    LogDetail(L"MMT/TLV Video discovered streamIndex=%d, packetId=0x%04X, componentTag=%d, %dx%d\n",
              info.streamIndex, info.packetId, info.componentTag, info.width, info.height);
    if (!m_hasSelectedVideoStream)
        selectDefaultVideoStreamLocked();
}

bool CFilterDemuxerHandler::shouldProcessVideoStream(int streamIndex) const
{
    std::lock_guard<std::mutex> lock(m_videoMutex);
    if (!m_hasSelectedVideoStream)
        return true;

    auto it = std::find_if(m_videoStreams.begin(), m_videoStreams.end(),
        [streamIndex](const VideoStreamInfo& info) {
            return info.streamIndex == streamIndex;
        });
    if (it == m_videoStreams.end())
        return true;

    if (it->packetId != m_selectedVideoPacketId)
        return false;
    if (it->componentTag >= 0 && m_selectedVideoComponentTag >= 0)
        return it->componentTag == m_selectedVideoComponentTag;
    return true;
}

std::vector<CFilterDemuxerHandler::VideoStreamInfo> CFilterDemuxerHandler::getVideoStreams() const
{
    std::lock_guard<std::mutex> lock(m_videoMutex);
    return m_videoStreams;
}

int CFilterDemuxerHandler::getSelectedVideoStreamIndex() const
{
    std::lock_guard<std::mutex> lock(m_videoMutex);
    if (!m_hasSelectedVideoStream)
        return -1;
    for (const auto& info : m_videoStreams) {
        if (info.packetId == m_selectedVideoPacketId &&
            (info.componentTag < 0 || m_selectedVideoComponentTag < 0 ||
             info.componentTag == m_selectedVideoComponentTag)) {
            return info.streamIndex;
        }
    }
    return -1;
}

bool CFilterDemuxerHandler::isSelectedVideoStream(size_t listIndex) const
{
    std::lock_guard<std::mutex> lock(m_videoMutex);
    if (!m_hasSelectedVideoStream || listIndex >= m_videoStreams.size())
        return false;
    const auto& info = m_videoStreams[listIndex];
    if (info.packetId != m_selectedVideoPacketId)
        return false;
    if (info.componentTag >= 0 && m_selectedVideoComponentTag >= 0)
        return info.componentTag == m_selectedVideoComponentTag;
    return true;
}

bool CFilterDemuxerHandler::selectVideoStreamByListIndex(size_t listIndex)
{
    std::lock_guard<std::mutex> lock(m_videoMutex);
    if (listIndex >= m_videoStreams.size())
        return false;

    const auto& info = m_videoStreams[listIndex];
    m_selectedVideoPacketId = info.packetId;
    m_selectedVideoComponentTag = info.componentTag;
    m_hasSelectedVideoStream = true;
    LogMsg(L"MMT/TLV Video selection changed: listIndex=%zu packetId=0x%04X componentTag=%d streamIndex=%d\n",
           listIndex, info.packetId, info.componentTag, info.streamIndex);
    return true;
}

bool CFilterDemuxerHandler::selectVideoStreamByStreamIndex(int streamIndex)
{
    std::lock_guard<std::mutex> lock(m_videoMutex);
    auto it = std::find_if(m_videoStreams.begin(), m_videoStreams.end(),
        [streamIndex](const VideoStreamInfo& info) {
            return info.streamIndex == streamIndex;
        });
    if (it == m_videoStreams.end())
        return false;

    m_selectedVideoPacketId = it->packetId;
    m_selectedVideoComponentTag = it->componentTag;
    m_hasSelectedVideoStream = true;
    return true;
}

void CFilterDemuxerHandler::rememberAudioStream(const MmtTlv::MmtStream& stream)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (m_audioStreamListLocked)
        return;

    int streamIndex = static_cast<int>(stream.getStreamIndex());
    if (m_requireAdtsConvertibleAudio &&
        !stream.is22_2chAudio() &&
        std::find(m_adtsConvertibleAudioStreams.begin(),
                  m_adtsConvertibleAudioStreams.end(),
                  streamIndex) == m_adtsConvertibleAudioStreams.end()) {
        return;
    }

    AudioStreamInfo info;
    info.streamIndex = streamIndex;
    info.packetId = stream.getPacketId();
    info.componentTag = stream.getComponentTag();
    info.samplingRate = stream.getSamplingRate();
    if (auto desc = stream.getMhAudioComponentDescriptor())
        info.channels = AudioDescriptorChannels(desc->get());
    else
        info.channels = stream.is22_2chAudio() ? 24 : 2;
    info.latm = stream.is22_2chAudio();

    auto it = std::find_if(m_audioStreams.begin(), m_audioStreams.end(),
        [&info](const AudioStreamInfo& known) {
            return SameAudioIdentity(known, info);
        });
    if (it != m_audioStreams.end()) {
        it->streamIndex = info.streamIndex;
        it->samplingRate = info.samplingRate;
        it->channels = info.channels;
        it->latm = info.latm;
        if (it->latm && it->extraData.empty())
            LogDetail(L"MMT/TLV Audio LATM stream known without extra data: streamIndex=%d\n", streamIndex);
        return;
    }

    m_audioStreams.push_back(info);
    LogDetail(L"MMT/TLV Audio discovered streamIndex=%d, packetId=0x%04X, componentTag=%d, samplingRate=%u, format=%s, channels=%u\n",
           info.streamIndex, info.packetId, info.componentTag, info.samplingRate,
           info.latm ? L"LATM" : L"ADTS", info.channels);
}

void CFilterDemuxerHandler::rememberLatmConfig(int streamIndex, const uint8_t* data, size_t size)
{
    if (!data || size < 9)
        return;

    const uint16_t syncWord = static_cast<uint16_t>((data[0] << 3) | ((data[1] & 0xE0) >> 5));
    if (syncWord != 0x2B7)
        return;

    std::lock_guard<std::mutex> lock(m_audioMutex);
    auto it = std::find_if(m_audioStreams.begin(), m_audioStreams.end(),
        [streamIndex](const AudioStreamInfo& info) {
            return info.streamIndex == streamIndex;
        });
    if (it == m_audioStreams.end() || !it->latm || !it->extraData.empty())
        return;

    constexpr size_t kStreamMuxConfigProbeBytes = 6;
    it->extraData.assign(data + 3, data + 3 + kStreamMuxConfigProbeBytes);

    WCHAR hex[64]{};
    WCHAR* cursor = hex;
    size_t remaining = ARRAYSIZE(hex);
    for (uint8_t b : it->extraData) {
        HRESULT hr = StringCchPrintfW(cursor, remaining, L"%02X ", b);
        if (FAILED(hr))
            break;
        size_t used = wcslen(cursor);
        cursor += used;
        remaining -= used;
    }
    LogDetail(L"MMT/TLV Audio LATM extra data captured: streamIndex=%d, bytes=%zu, data=%s\n",
              streamIndex, it->extraData.size(), hex);
}

bool CFilterDemuxerHandler::shouldProcessAudioStream(int streamIndex) const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (!m_requireAdtsConvertibleAudio)
        return true;

    // Each DirectShow audio output pin represents one stream.  MPC-BE may switch
    // between those pins without calling IAMStreamSelect::Enable, so demux-time
    // filtering by the selected stream identity can starve the selected pin.
    auto known = std::find_if(m_audioStreams.begin(), m_audioStreams.end(),
        [streamIndex](const AudioStreamInfo& info) {
            return info.streamIndex == streamIndex;
        });
    if (known != m_audioStreams.end())
        return true;

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
        LogDetail(L"MMT/TLV Audio ADTS conversion available: streamIndex=%d\n", streamIndex);
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
            LogDetail(L"MMT/TLV Subtitle data seen streamIndex=%d, packetId=0x%04X, componentTag=%d\n",
                   it->streamIndex, it->packetId, it->componentTag);
        }
        return;
    }

    SubtitleStreamInfo info;
    info.streamIndex = streamIndex;
    info.packetId = stream.getPacketId();
    info.componentTag = stream.getComponentTag();
    info.hasData = true;

    if (IsCaptionComponentTag(info.componentTag)) {
        const int companionTag = info.componentTag + 8;
        auto companion = std::find_if(m_subtitleStreams.begin(), m_subtitleStreams.end(),
            [companionTag](const SubtitleStreamInfo& known) {
                return !known.hasData && known.componentTag == companionTag;
            });
        if (companion != m_subtitleStreams.end()) {
            LogMsg(L"MMT/TLV Subtitle replacing management stream: oldStreamIndex=%d oldPacketId=0x%04X oldComponentTag=%d -> streamIndex=%d packetId=0x%04X componentTag=%d\n",
                   companion->streamIndex,
                   companion->packetId,
                   companion->componentTag,
                   info.streamIndex,
                   info.packetId,
                   info.componentTag);
            *companion = info;
            return;
        }
    }

    m_subtitleStreams.push_back(info);
    LogDetail(L"MMT/TLV Subtitle discovered streamIndex=%d, packetId=0x%04X, componentTag=%d, data=1\n",
           info.streamIndex, info.packetId, info.componentTag);
}

void CFilterDemuxerHandler::onMpt(const MmtTlv::Mpt& mpt)
{
    std::vector<VideoStreamInfo> discoveredVideos;
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

        if (asset.assetType == MmtTlv::AssetType::hev1) {
            VideoStreamInfo info;
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
                case MmtTlv::VideoComponentDescriptor::kDescriptorTag:
                {
                    const auto* video = static_cast<const MmtTlv::VideoComponentDescriptor*>(descriptor.get());
                    info.componentTag = video->componentTag;
                    VideoResolutionSize(video->videoResolution, video->videoAspectRatio,
                                        &info.width, &info.height);
                    break;
                }
                }
            }

            discoveredVideos.push_back(info);
        } else if (asset.assetType == MmtTlv::AssetType::mp4a) {
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
                    info.channels = AudioDescriptorChannels(*audio);
                    info.latm = audio->is22_2chAudio();
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

    if (!discoveredVideos.empty()) {
        std::lock_guard<std::mutex> lock(m_videoMutex);
        for (auto& info : discoveredVideos) {
            auto it = std::find_if(m_videoStreams.begin(), m_videoStreams.end(),
                [&info](const VideoStreamInfo& known) {
                    return SameVideoIdentity(known, info);
                });
            if (it != m_videoStreams.end()) {
                // The demuxer counts a stream index per packet location while
                // this walk counts one per asset, so once data has been seen the
                // index observed there is the authoritative one.
                info.streamIndex = it->streamIndex;
                info.hasData = it->hasData;
            }
        }

        bool changed = discoveredVideos.size() != m_videoStreams.size();
        if (!changed) {
            for (size_t i = 0; i < discoveredVideos.size(); ++i) {
                if (discoveredVideos[i].streamIndex != m_videoStreams[i].streamIndex ||
                    discoveredVideos[i].packetId != m_videoStreams[i].packetId ||
                    discoveredVideos[i].componentTag != m_videoStreams[i].componentTag ||
                    discoveredVideos[i].width != m_videoStreams[i].width ||
                    discoveredVideos[i].height != m_videoStreams[i].height) {
                    changed = true;
                    break;
                }
            }
        }

        if (changed) {
            m_videoStreams = discoveredVideos;
            LogMsg(L"MMT/TLV Video MPT updated: assets=%u, videoStreams=%zu\n",
                   static_cast<unsigned>(mpt.assets.size()), m_videoStreams.size());
            for (size_t i = 0; i < m_videoStreams.size(); ++i) {
                const auto& info = m_videoStreams[i];
                LogDetail(L"MMT/TLV Video MPT stream[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, %dx%d, data=%d\n",
                          i, info.streamIndex, info.packetId, info.componentTag,
                          info.width, info.height, info.hasData ? 1 : 0);
            }
        }

        // Only pick a default; an explicit selection must survive MPT updates.
        if (!m_hasSelectedVideoStream)
            selectDefaultVideoStreamLocked();
    }

    if (!discovered.empty()) {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        for (auto& info : discovered) {
            auto it = std::find_if(m_audioStreams.begin(), m_audioStreams.end(),
                [&info](const AudioStreamInfo& known) {
                    return SameAudioIdentity(known, info);
                });
            if (it != m_audioStreams.end()) {
                info.streamIndex = it->streamIndex;
                if (!it->extraData.empty())
                    info.extraData = it->extraData;
            }
        }

        if (m_requireAdtsConvertibleAudio) {
            discovered.erase(std::remove_if(discovered.begin(), discovered.end(),
                [this](const AudioStreamInfo& info) {
                    if (info.latm)
                        return false;
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
                        return SameAudioIdentity(known, info);
                    });
                if (it == m_audioStreams.end()) {
                    missing = true;
                    break;
                }
            }
            if (missing) {
                LogDetail(L"MMT/TLV Audio MPT changed after pin creation; keeping PreScan audio list (%zu stream(s))\n",
                       m_audioStreams.size());
            }
            return;
        }

        bool changed = discovered.size() != m_audioStreams.size();
        if (!changed) {
            for (size_t i = 0; i < discovered.size(); ++i) {
                if (discovered[i].streamIndex != m_audioStreams[i].streamIndex ||
                    discovered[i].packetId != m_audioStreams[i].packetId ||
                    discovered[i].componentTag != m_audioStreams[i].componentTag ||
                    discovered[i].channels != m_audioStreams[i].channels ||
                    discovered[i].latm != m_audioStreams[i].latm ||
                    discovered[i].extraData != m_audioStreams[i].extraData) {
                    changed = true;
                    break;
                }
            }
        }

        if (changed) {
            m_audioStreams = discovered;
            if (!m_hasSelectedAudioStream && !m_audioStreams.empty()) {
                m_selectedAudioPacketId = m_audioStreams.front().packetId;
                m_selectedAudioComponentTag = m_audioStreams.front().componentTag;
                m_hasSelectedAudioStream = true;
            }

            LogMsg(L"MMT/TLV Audio MPT updated: assets=%u, audioStreams=%zu, selectedPacketId=0x%04X, selectedComponentTag=%d\n",
                   static_cast<unsigned>(mpt.assets.size()),
                   m_audioStreams.size(),
                   m_hasSelectedAudioStream ? m_selectedAudioPacketId : 0,
                   m_hasSelectedAudioStream ? m_selectedAudioComponentTag : -1);
            for (size_t i = 0; i < m_audioStreams.size(); ++i) {
                const auto& info = m_audioStreams[i];
                LogDetail(L"MMT/TLV Audio MPT stream[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, samplingRate=%u, format=%s, channels=%u, extra=%zu\n",
                       i, info.streamIndex, info.packetId, info.componentTag, info.samplingRate,
                       info.latm ? L"LATM" : L"ADTS", info.channels, info.extraData.size());
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
                LogDetail(L"MMT/TLV Subtitle MPT stream[%zu]: streamIndex=%d, packetId=0x%04X, componentTag=%d, data=%d\n",
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
    m_hasSelectedAudioStream = false;
    m_selectedAudioPacketId = 0;
    m_selectedAudioComponentTag = -1;
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
    if (!m_hasSelectedAudioStream && !m_audioStreams.empty()) {
        m_selectedAudioPacketId = m_audioStreams.front().packetId;
        m_selectedAudioComponentTag = m_audioStreams.front().componentTag;
        m_hasSelectedAudioStream = true;
        LogMsg(L"MMT/TLV Audio default packetId=0x%04X componentTag=%d streamIndex=%d\n",
               m_selectedAudioPacketId,
               m_selectedAudioComponentTag,
               m_audioStreams.front().streamIndex);
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
        if (!info.latm &&
            std::find(m_adtsConvertibleAudioStreams.begin(),
                      m_adtsConvertibleAudioStreams.end(),
                      info.streamIndex) != m_adtsConvertibleAudioStreams.end()) {
            streams.push_back(info);
        }
    }
    return streams;
}

std::vector<CFilterDemuxerHandler::AudioStreamInfo> CFilterDemuxerHandler::getPlayableAudioStreams() const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    std::vector<AudioStreamInfo> streams;

    for (const auto& info : m_audioStreams) {
        if (!info.latm &&
            std::find(m_adtsConvertibleAudioStreams.begin(),
                      m_adtsConvertibleAudioStreams.end(),
                      info.streamIndex) != m_adtsConvertibleAudioStreams.end()) {
            streams.push_back(info);
        }
    }

    for (const auto& info : m_audioStreams) {
        if (info.latm)
            streams.push_back(info);
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
    if (!m_hasSelectedAudioStream)
        return -1;
    auto it = std::find_if(m_audioStreams.begin(), m_audioStreams.end(),
        [this](const AudioStreamInfo& info) {
            return info.packetId == m_selectedAudioPacketId &&
                   (m_selectedAudioComponentTag < 0 ||
                    info.componentTag < 0 ||
                    info.componentTag == m_selectedAudioComponentTag);
        });
    return it != m_audioStreams.end() ? it->streamIndex : -1;
}

bool CFilterDemuxerHandler::isSelectedAudioStream(size_t listIndex) const
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (!m_hasSelectedAudioStream || listIndex >= m_audioStreams.size())
        return false;

    const auto& info = m_audioStreams[listIndex];
    return info.packetId == m_selectedAudioPacketId &&
           (m_selectedAudioComponentTag < 0 ||
            info.componentTag < 0 ||
            info.componentTag == m_selectedAudioComponentTag);
}

bool CFilterDemuxerHandler::selectAudioStreamByListIndex(size_t listIndex)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (listIndex >= m_audioStreams.size())
        return false;

    const auto& info = m_audioStreams[listIndex];
    m_selectedAudioPacketId = info.packetId;
    m_selectedAudioComponentTag = info.componentTag;
    m_hasSelectedAudioStream = true;
    LogMsg(L"MMT/TLV Audio selected by IAMStreamSelect: list=%zu, packetId=0x%04X, componentTag=%d, streamIndex=%d\n",
           listIndex, m_selectedAudioPacketId, m_selectedAudioComponentTag, info.streamIndex);
    return true;
}

bool CFilterDemuxerHandler::selectAudioStreamByStreamIndex(int streamIndex)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    auto it = std::find_if(m_audioStreams.begin(), m_audioStreams.end(),
        [streamIndex](const AudioStreamInfo& info) {
            return info.streamIndex == streamIndex;
        });
    if (it == m_audioStreams.end())
        return false;

    m_selectedAudioPacketId = it->packetId;
    m_selectedAudioComponentTag = it->componentTag;
    m_hasSelectedAudioStream = true;
    LogMsg(L"MMT/TLV Audio selected by streamIndex=%d packetId=0x%04X componentTag=%d\n",
           streamIndex, m_selectedAudioPacketId, m_selectedAudioComponentTag);
    return true;
}

void CFilterDemuxerHandler::onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu)
{
    if (!m_videoCallback || mfu.data.empty())
        return;

    rememberVideoStream(stream);

    // The video pin assembles one access unit at a time, so fragments from a
    // second hev1 asset would clear the accumulator mid-AU and splice their NAL
    // units into the primary stream. Deliver the selected stream only.
    if (!shouldProcessVideoStream(static_cast<int>(stream.getStreamIndex())))
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
        if (!m_hasSelectedAudioStream) {
            m_selectedAudioPacketId = stream.getPacketId();
            m_selectedAudioComponentTag = stream.getComponentTag();
            m_hasSelectedAudioStream = true;
            LogMsg(L"MMT/TLV Audio selected packetId=0x%04X componentTag=%d streamIndex=%d\n",
                   m_selectedAudioPacketId, m_selectedAudioComponentTag, streamIndex);
        }
    }

    if (!shouldProcessAudioStream(streamIndex))
        return;

    if (stream.is22_2chAudio()) {
        rememberLatmConfig(streamIndex, mfu.data.data(), mfu.data.size());

        long long pts = toRefTime(static_cast<int64_t>(mfu.pts), stream);
        long long dts = toRefTime(static_cast<int64_t>(mfu.dts), stream);

        m_audioCallback(streamIndex,
                        false, pts, dts, mfu.isFirstFragment, mfu.isLastFragment,
                        mfu.data.data(), mfu.data.size());
        return;
    }

    // Convert LOAS/LATM → ADTS for ordinary AAC streams.
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
            LogDetail(L"MMT/TLV Audio ADTS convert failed #%ld: streamIndex=%d, packetId=0x%04X, size=%zu, samplingRate=%u, firstBytes=%s\n",
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
    if (mfu.data.empty())
        return;

    rememberSubtitleStream(stream);

    long long pts = toRefTime(static_cast<int64_t>(mfu.pts), stream);
    long long dts = toRefTime(static_cast<int64_t>(mfu.dts), stream);

    static volatile LONG s_subtitleMfuLogCount = 0;
    LONG logNo = InterlockedIncrement(&s_subtitleMfuLogCount);
    if (logNo <= 120 || mfu.subtitleDataType != 0) {
        WCHAR hex[96] = {};
        WCHAR* cursor = hex;
        size_t remaining = ARRAYSIZE(hex);
        const size_t previewSize = (std::min)(mfu.data.size(), static_cast<size_t>(16));
        for (size_t i = 0; i < previewSize && remaining > 4; ++i) {
            HRESULT hr = StringCchPrintfW(cursor, remaining, L"%02X ", mfu.data[i]);
            if (FAILED(hr))
                break;
            size_t used = wcslen(cursor);
            cursor += used;
            remaining -= used;
        }
        WCHAR ptsText[32] = {};
        if (pts >= 0)
            StringCchPrintfW(ptsText, ARRAYSIZE(ptsText), L"%I64d ms", pts / 10000);
        else
            StringCchCopyW(ptsText, ARRAYSIZE(ptsText), L"none");
        LogMsg(L"MMT/TLV Subtitle MFU #%ld: streamIndex=%u packetId=0x%04X componentTag=%d type=%u subsample=%u/%u pts=%s size=%zu first=%s\n",
               logNo,
               stream.getStreamIndex(),
               stream.getPacketId(),
               stream.getComponentTag(),
               mfu.subtitleDataType,
               mfu.subtitleSubsampleNumber,
               mfu.subtitleLastSubsampleNumber,
               ptsText,
               mfu.data.size(),
               hex);
    }

    if (mfu.subtitleDataType != 0) {
        if (m_subtitleResourceCallback) {
            m_subtitleResourceCallback(static_cast<int>(stream.getStreamIndex()),
                                       mfu.subtitleDataType,
                                       mfu.subtitleSubsampleNumber,
                                       mfu.subtitleLastSubsampleNumber,
                                       pts, dts,
                                       mfu.data.data(), mfu.data.size());
        }
        return;
    }

    if (!m_subtitleCallback)
        return;

    m_subtitleCallback(static_cast<int>(stream.getStreamIndex()),
                       true, pts, dts, true, true,
                       mfu.data.data(), mfu.data.size());
}

void CFilterDemuxerHandler::onMhEit(const MmtTlv::MhEit& mhEit)
{
    if (!mhEit.isPf() || mhEit.sectionNumber != 0)
        return;

    for (const auto& mhEvent : mhEit.events) {
        if (!mhEvent)
            continue;

        std::tm startTime = EITConvertStartTime(mhEvent->startTime);
        if (!isValidEITStartTime(startTime))
            continue;

        const std::time_t startSec = std::mktime(&startTime);
        if (startSec < 0)
            continue;

        const long long programStartSec = static_cast<long long>(startSec);
        if (programStartSec == m_programStartTimeSec)
            return;

        m_programStartTimeSec = programStartSec;
        const long long programStartRt = programStartSec * 10000000LL;
        LogMsg(L"MMT/TLV EIT program start: unix=%I64d, rt=%I64d ms, eventId=%u\n",
               programStartSec,
               programStartRt / 10000,
               mhEvent->eventId);
        if (m_programStartCallback)
            m_programStartCallback(programStartRt);
        return;
    }
}

void CFilterDemuxerHandler::onNtp(const MmtTlv::NTPv4& ntp)
{
    if (!m_ntpCallback)
        return;

    const long long ntpRt = Pcr27ToRefTime(ntp.transmit_timestamp.toPcrValue());
    if (ntpRt < 0)
        return;

    m_ntpCallback(ntpRt);
}
