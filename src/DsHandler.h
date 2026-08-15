#pragma once
#include <functional>
#include <vector>
#include <mutex>
#include <cstdint>
#include "demuxerHandler.h"
#include "adtsConverter.h"

namespace MmtTlv { class MmtStream; struct MfuData; class MhEit; class NTPv4; }

// Bridges dantto4k demuxer callbacks to DirectShow sample delivery.
// Uses long long (= REFERENCE_TIME = 100ns units) to avoid windows.h dependency here.
class CFilterDemuxerHandler : public MmtTlv::DemuxerHandler {
public:
    struct AudioStreamInfo {
        int streamIndex{-1};
        uint16_t packetId{0};
        int componentTag{-1};
        uint32_t samplingRate{0};
        uint16_t channels{2};
        bool latm{false};
        std::vector<uint8_t> extraData;
    };

    struct VideoStreamInfo {
        int streamIndex{-1};
        uint16_t packetId{0};
        int componentTag{-1};
        int width{0};
        int height{0};
        bool hasData{false};
    };

    struct SubtitleStreamInfo {
        int streamIndex{-1};
        uint16_t packetId{0};
        int componentTag{-1};
        bool hasData{false};
    };

    using SampleCallback = std::function<void(
        int streamIndex,
        bool keyframe,
        long long pts,   // DirectShow REFERENCE_TIME (100ns units), -1 = unknown
        long long dts,
        bool isFirstFragment,
        bool isLastFragment,
        const uint8_t* data,
        size_t size)>;

    using SubtitleResourceCallback = std::function<void(
        int streamIndex,
        int dataType,
        int subsampleNumber,
        int lastSubsampleNumber,
        long long pts,
        long long dts,
        const uint8_t* data,
        size_t size)>;

    using ProgramStartCallback = std::function<void(long long programStartRt)>;
    using NtpCallback = std::function<void(long long ntpRt)>;

    void setVideoCallback(SampleCallback cb) { m_videoCallback = std::move(cb); }
    void setAudioCallback(SampleCallback cb) { m_audioCallback = std::move(cb); }
    void setSubtitleCallback(SampleCallback cb) { m_subtitleCallback = std::move(cb); }
    void setSubtitleResourceCallback(SubtitleResourceCallback cb) { m_subtitleResourceCallback = std::move(cb); }
    void setProgramStartCallback(ProgramStartCallback cb) { m_programStartCallback = std::move(cb); }
    void setNtpCallback(NtpCallback cb) { m_ntpCallback = std::move(cb); }

    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override;
    void onAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override;
    void onSubtitleData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override;
    void onMhEit(const MmtTlv::MhEit& mhEit) override;
    void onNtp(const MmtTlv::NTPv4& ntp) override;
    void onMpt(const MmtTlv::Mpt& mpt) override;

    void reset() { m_basePts = -1; m_programStartTimeSec = -1; }
    void resetAudioSelection();
    std::vector<VideoStreamInfo> getVideoStreams() const;
    int getSelectedVideoStreamIndex() const;
    bool isSelectedVideoStream(size_t listIndex) const;
    bool selectVideoStreamByListIndex(size_t listIndex);
    bool selectVideoStreamByStreamIndex(int streamIndex);
    std::vector<AudioStreamInfo> getAudioStreams() const;
    std::vector<SubtitleStreamInfo> getSubtitleStreams() const;
    void setKnownVideoStreams(const std::vector<VideoStreamInfo>& streams);
    void setKnownAudioStreams(const std::vector<AudioStreamInfo>& streams);
    void setKnownSubtitleStreams(const std::vector<SubtitleStreamInfo>& streams);
    std::vector<AudioStreamInfo> getAdtsConvertibleAudioStreams() const;
    std::vector<AudioStreamInfo> getPlayableAudioStreams() const;
    void setRequireAdtsConvertibleAudio(bool require);
    void setAudioStreamListLocked(bool locked);
    int getSelectedAudioStreamIndex() const;
    bool isSelectedAudioStream(size_t listIndex) const;
    bool selectAudioStreamByListIndex(size_t listIndex);
    bool selectAudioStreamByStreamIndex(int streamIndex);

private:
    long long toRefTime(int64_t pts, const MmtTlv::MmtStream& stream);
    void rememberVideoStream(const MmtTlv::MmtStream& stream);
    bool shouldProcessVideoStream(int streamIndex) const;
    // Callers must hold m_videoMutex.
    void selectDefaultVideoStreamLocked();
    void rememberAudioStream(const MmtTlv::MmtStream& stream);
    void rememberLatmConfig(int streamIndex, const uint8_t* data, size_t size);
    void rememberAdtsConvertibleAudioStream(int streamIndex);
    void rememberSubtitleStream(const MmtTlv::MmtStream& stream);
    bool shouldProcessAudioStream(int streamIndex) const;

    SampleCallback m_videoCallback;
    SampleCallback m_audioCallback;
    SampleCallback m_subtitleCallback;
    SubtitleResourceCallback m_subtitleResourceCallback;
    ProgramStartCallback m_programStartCallback;
    NtpCallback m_ntpCallback;
    ADTSConverter  m_adtsConverter;
    long long m_basePts{-1};  // first valid PTS seen, in 100ns units
    long long m_programStartTimeSec{-1};
    // A BS4K package can carry more than one hev1 asset (e.g. a 4K main video
    // plus a 1080p simulcast). They share one DirectShow video pin, so exactly
    // one of them may be delivered - mixing their MFU fragments produces
    // spliced, undecodable access units.
    mutable std::mutex m_videoMutex;
    std::vector<VideoStreamInfo> m_videoStreams;
    bool m_hasSelectedVideoStream{false};
    uint16_t m_selectedVideoPacketId{0};
    int m_selectedVideoComponentTag{-1};
    bool m_hasSelectedAudioStream{false};
    uint16_t m_selectedAudioPacketId{0};
    int m_selectedAudioComponentTag{-1};
    mutable std::mutex m_audioMutex;
    std::vector<AudioStreamInfo> m_audioStreams;
    std::vector<int> m_adtsConvertibleAudioStreams;
    bool m_requireAdtsConvertibleAudio{false};
    bool m_audioStreamListLocked{false};
    mutable std::mutex m_subtitleMutex;
    std::vector<SubtitleStreamInfo> m_subtitleStreams;
};
