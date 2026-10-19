#include "DsHandler.h"
#include "mpt.h"
#include "mhEit.h"
#include "videoComponentDescriptor.h"
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
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
int main() {
    try {
        testServiceId({0,2}, 2);
        testServiceId({1,0,2}, 2);
        testServiceId({0,1,0,2}, 2);
        testServiceId({1,2,3,4,0x12,0x34}, 0x1234);
        std::cout << "Transport state tests passed: package service identity\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
