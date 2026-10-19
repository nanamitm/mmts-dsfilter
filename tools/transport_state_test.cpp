#include "DsHandler.h"
#include "mpt.h"
#include "mhEit.h"
#include "plt.h"
#include "mmtStream.h"
#include "mhStreamIdentificationDescriptor.h"
#include "videoComponentDescriptor.h"
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// Directly constructed descriptors still need the tags normally set by unpack().
template<class T> class TestDescriptor : public T {
public:
    TestDescriptor() : T() { this->descriptorTag = T::kDescriptorTag; this->descriptorLength = 0; }
};
class PresentEvent : public MmtTlv::MhEit {
public:
    PresentEvent(uint16_t service) {
        tableId = MmtTlv::MmtTableId::MhEitPf;
        sectionNumber = 0; serviceId = service;
        auto event = std::make_shared<Event>();
        event->eventId = 1; event->startTime = (60000LL << 24) | 0x120000;
        events.push_back(event);
    }
};
static void testServiceId(const std::vector<uint8_t>& package, uint16_t service) {
    CFilterDemuxerHandler handler;
    MmtTlv::Mpt mpt{}; mpt.mmtPackageIdByte = package; handler.onMpt(mpt);
    unsigned callbacks = 0;
    handler.setProgramStartCallback([&](long long, uint32_t id) {
        require((id >> 16) == service, "event must belong to the package service"); ++callbacks;
    });
    handler.onMhEit(PresentEvent(service));
    require(callbacks == 1, "accept the service in the low 16 bits of package_id");
    handler.onMhEit(PresentEvent(static_cast<uint16_t>(service + 1)));
    require(callbacks == 1, "ignore another service's present event");
}
static void addAsset(MmtTlv::Mpt& m, uint32_t type, uint16_t pid, uint16_t tag) {
    MmtTlv::Mpt::Asset asset{}; asset.assetType = type;
    MmtTlv::MmtGeneralLocationInfo location{}; location.locationType = 0; location.packetId = pid;
    asset.locationInfos.push_back(location);
    auto id = std::make_unique<TestDescriptor<MmtTlv::MhStreamIdentificationDescriptor>>(); id->componentTag = tag;
    asset.descriptors.list.push_back(std::move(id)); m.assets.push_back(std::move(asset));
}
static void fillPackage(MmtTlv::Mpt& m, uint16_t service, uint16_t base) {
    m.mmtPackageIdByte = {0, static_cast<uint8_t>(service)};
    addAsset(m, MmtTlv::AssetType::hev1, base, 0);
    addAsset(m, MmtTlv::AssetType::mp4a, base + 1, 0x10);
    addAsset(m, MmtTlv::AssetType::stpp, base + 2, 0x30);
}
static void announce(CFilterDemuxerHandler& handler, std::initializer_list<uint16_t> services) {
    MmtTlv::Plt plt{};
    for (auto service : services) {
        MmtTlv::Plt::Entry entry{}; entry.mmtPackageIdByte = {0, static_cast<uint8_t>(service)};
        entry.locationInfos.locationType = 0; plt.entries.push_back(std::move(entry));
    }
    handler.onPlt(plt);
}
static void feedVideo(CFilterDemuxerHandler& handler, uint16_t pid) {
    MmtTlv::MmtStream stream(pid); MmtTlv::MfuData mfu{};
    mfu.pts = mfu.dts = 90000; mfu.data = {0,0,1,0x26,1,0x80};
    mfu.isFirstFragment = mfu.isLastFragment = true; handler.onVideoData(stream, mfu);
}
static void testPackages() {
    CFilterDemuxerHandler handler; MmtTlv::Mpt a{}, b{};
    fillPackage(a, 1, 0x100); fillPackage(b, 2, 0x200);
    unsigned video = 0, subtitles = 0, controls = 0, audio = 0, programs = 0;
    handler.setVideoCallback([&](int,bool,long long,long long,bool,bool,const uint8_t*,size_t){++video;});
    handler.setAudioCallback([&](int,bool,long long,long long,bool,bool,const uint8_t*,size_t){++audio;});
    handler.setSubtitleCallback([&](int,uint16_t,int,long long,long long,const uint8_t*,size_t){++subtitles;});
    handler.setSubtitleControlCallback([&](const auto&){++controls;});
    handler.setProgramStartCallback([&](long long,uint32_t){++programs;});
    handler.onMpt(a); const auto firstControls = controls;
    handler.setAudioStreamListLocked(true); handler.onMpt(b);
    require(handler.getVideoStreams().size() == 1 && handler.getVideoStreams()[0].packetId == 0x100,
            "another package must not replace the selected video");
    require(handler.getAudioStreams()[0].packetId == 0x101 && handler.getSubtitleStreams()[0].packetId == 0x102 && controls == firstControls,
            "other-service MPT must not replace audio/subtitles or clear their controls");
    feedVideo(handler,0x200); feedVideo(handler,0x100); require(video == 1,"only selected-service video is delivered");
    for (uint16_t base : {0x100,0x200}) {
        MmtTlv::MmtStream aud(base+1), sub(base+2); MmtTlv::MfuData mfu{};
        mfu.data = {0x56,0xE0,12,0x40,0,0x11,0x90,0,8,0,0x20,0,0,0,0};
        mfu.isFirstFragment = mfu.isLastFragment = true; handler.onAudioData(aud,mfu);
        mfu.data = {'<','t','t','/','>'}; handler.onSubtitleData(sub,mfu);
    }
    require(audio == 1 && subtitles == 1,"other services' audio and captions must not leak to selected pins");
    handler.onMhEit(PresentEvent(2)); handler.onMhEit(PresentEvent(1)); require(programs == 1,"EIT must remain scoped to the selected service");
    handler.reset(); handler.onMpt(b); feedVideo(handler,0x200); require(video == 1,"seek reset must keep service selection");
    announce(handler,{2,1}); handler.onMpt(b); require(handler.getVideoStreams()[0].packetId == 0x100,"PLT order must not change an available selection");
    CFilterDemuxerHandler prescanCopy; prescanCopy.copyServiceSelectionFrom(handler); prescanCopy.onMpt(b);
    require(prescanCopy.getVideoStreams().empty(),"playback must preserve the service chosen by pre-scan");
    announce(handler,{2}); handler.onMpt(b); feedVideo(handler,0x100); feedVideo(handler,0x200);
    require(video == 2 && handler.getVideoStreams()[0].packetId == 0x200,"PLT-confirmed channel change must follow the new service");
    announce(handler,{}); feedVideo(handler,0x200); handler.onMpt(b); require(video == 2,"empty PLT suppresses stale media and MPTs");
    announce(handler,{1}); handler.onMpt(a); feedVideo(handler,0x100); require(video == 3,"new PLT/MPT restores service playback");
    MmtTlv::Mpt empty{}; empty.mmtPackageIdByte = {0,1}; handler.onMpt(empty);
    require(handler.getVideoStreams().empty() && handler.getSubtitleStreams().empty(),"selected-service removal must clear video and caption declarations");
    handler.resetAudioSelection(); handler.onMpt(b); feedVideo(handler,0x200); require(video == 4,"new file must reset service ownership");
}
static void testVideoAttributes() {
    static constexpr uint32_t expected[][2] = {
        {0,1},{15,1},{24000,1001},{24,1},{25,1},{30000,1001},{30,1},
        {50,1},{60000,1001},{60,1},{100,1},{120000,1001},{120,1}
    };
    for (uint8_t code = 1; code <= 12; ++code) {
        CFilterDemuxerHandler handler; MmtTlv::Mpt mpt{}; mpt.mmtPackageIdByte = {0,1};
        addAsset(mpt,MmtTlv::AssetType::hev1,0x100,0);
        auto descriptor = std::make_unique<TestDescriptor<MmtTlv::VideoComponentDescriptor>>();
        descriptor->componentTag=0; descriptor->videoResolution=5; descriptor->videoAspectRatio=1; descriptor->videoFrameRate=code;
        auto* changed=descriptor.get(); mpt.assets.back().descriptors.list.push_back(std::move(descriptor)); handler.onMpt(mpt);
        auto info=handler.getSelectedVideoStream();
        require(info && info->frameRateNumerator==expected[code][0] && info->frameRateDenominator==expected[code][1],"preserve all defined rational video rates");
        require(info->width==1440 && info->height==1080 && info->aspectX==4 && info->aspectY==3,"preserve 4:3 display aspect separately from resolution");
        changed->videoFrameRate=9; changed->videoAspectRatio=3; handler.onMpt(mpt); info=handler.getSelectedVideoStream();
        require(info && info->frameRateNumerator==60 && info->aspectX==16 && info->aspectY==9,"periodic MPT must update video attributes");
    }
}
int main() {
    try {
        testServiceId({0,2}, 2); testServiceId({1,0,2}, 2);
        testServiceId({0,1,0,2}, 2); testServiceId({1,2,3,4,0x12,0x34}, 0x1234);
        testPackages(); testVideoAttributes();
        std::cout << "Transport state tests passed: package identity, service isolation, PLT transitions and seek selection\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
