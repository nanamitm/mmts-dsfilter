#include "SubtitleFileIndex.h"
#include <iostream>
#include <stdexcept>
#include <filesystem>

using Bytes = std::vector<uint8_t>;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void be(Bytes& b, uint64_t value, int size) { for (int i = size - 1; i >= 0; --i) b.push_back(static_cast<uint8_t>(value >> (i * 8))); }
static void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }
static Bytes tlv(uint8_t type, const Bytes& payload) { Bytes b{0x7f, type}; be(b, payload.size(), 2); append(b, payload); return b; }
static Bytes mmtp(uint16_t pid, uint32_t sequence, uint8_t type, const Bytes& payload) {
    Bytes b{0, 0, 0x61, 0, type}; be(b, pid, 2); be(b, 0, 4); be(b, sequence, 4); append(b, payload); return tlv(3, b);
}
static Bytes ntp(uint32_t unixSeconds) {
    Bytes b(40 + 8 + 48, 0); b[0]=0x60; b[5]=56; b[6]=17;
    b[40+3]=123; b[40+5]=56;
    const uint32_t seconds=unixSeconds+2208988800U;
    for (int i=0;i<4;++i) b[40+8+40+i]=static_cast<uint8_t>(seconds >> ((3-i)*8));
    return tlv(2,b);
}
static Bytes mpt(uint32_t sequence, uint8_t version, uint8_t tmd=2, uint16_t tag=0x30) {
    Bytes descriptor{0x80,0x11,2}; be(descriptor,tag,2);
    Bytes control{0,0,'j','p','n',0,static_cast<uint8_t>(tmd<<4),0x10};
    if (tmd==2) {be(control,2208989800U,4);be(control,0,4);control.push_back(0);}
    Bytes data{0x80,0x20,static_cast<uint8_t>(control.size()+2),0,0x20};append(data,control);append(descriptor,data);
    Bytes asset{0};be(asset,0,4);asset.push_back(0);be(asset,0x73747070,4);asset.push_back(0);asset.push_back(1);asset.push_back(0);be(asset,0x100,2);be(asset,descriptor.size(),2);append(asset,descriptor);
    Bytes table{0x20,version};be(table,0,2);table.push_back(0);table.push_back(2);be(table,1,2);be(table,0,2);table.push_back(1);append(table,asset);
    Bytes pa{0,0,version};be(pa,table.size()+1,4);pa.push_back(0);append(pa,table);
    Bytes signaling{0,0};append(signaling,pa);
    return mmtp(0,sequence,2,signaling);
}
static Bytes subtitle(uint32_t sequence,const std::string& xml) {
    Bytes data{0,0,0,0,0};be(data,xml.size(),2);data.insert(data.end(),xml.begin(),xml.end());
    Bytes payload{0x20,0};be(payload,sequence,4);be(payload,0,4);append(payload,data);
    Bytes mpu;be(mpu,payload.size(),2);append(mpu,payload);return mmtp(0x100,sequence,0,mpu);
}
struct Fixture {
    Bytes bytes;
    std::filesystem::path path;
    explicit Fixture(const char* name):path(std::filesystem::path("obj")/name){}
    long long add(const Bytes& packet){const auto offset=bytes.size();append(bytes,packet);return static_cast<long long>(offset);}
    void save(){std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
};
static SubtitleFileIndex::Record resolve(SubtitleFileIndex& index,long long offset,const std::string& xml) {
    SubtitleFileIndex::Record record;
    require(index.Resolve(offset,0x100,DsSubtitle::Hash(reinterpret_cast<const uint8_t*>(xml.data()),xml.size()),record),"index must find exact MFU byte offset");
    return record;
}
int main(){
    try {
        const std::string a=R"(<tt><body><div begin="00:00:10.000" end="00:00:40.000"><p><span>A</span></p></div></body></tt>)";
        const std::string b=R"(<tt><body><div begin="00:00:20.000"><p><span>B</span></p></div></body></tt>)";
        const std::string clear="<tt></tt>";
        Fixture f("subtitle-index-test.mmts");f.add(ntp(1000));f.add(mpt(1,0));f.add(ntp(1005));const auto first=f.add(subtitle(1,a));f.add(ntp(1010));const auto second=f.add(subtitle(2,b));f.add(ntp(1015));f.add(subtitle(3,clear));f.add(ntp(1050));f.save();
        SubtitleFileIndex index(f.path.wstring(),1000LL*10000000,[]{return false;});
        const auto ra=resolve(index,first,a);
        require(ra.base==1000LL*10000000 && ra.start==1010LL*10000000,"use reference start time, not cue arrival");
        require(ra.stop==1015LL*10000000,"clear must override a previously received future replacement");
        const auto rb=resolve(index,second,b);
        require(rb.stop==1015LL*10000000 && *rb.stop<rb.start,"future cue cancelled by clear must never be displayed");
        require(resolve(index,first,a).stop==ra.stop,"backward seek must reuse recorded boundaries");
        Fixture replace("subtitle-replace-test.mmts");replace.add(ntp(1000));replace.add(mpt(1,0));replace.add(ntp(1005));const auto old=replace.add(subtitle(1,a));replace.add(ntp(1010));replace.add(subtitle(2,b));replace.add(ntp(1030));replace.save();
        SubtitleFileIndex replacements(replace.path.wstring(),1000LL*10000000,[]{return false;});
        require(resolve(replacements,old,a).stop==1020LL*10000000,"finite old cue must end at replacement begin");
        Fixture async("subtitle-async-test.mmts");async.add(ntp(1000));async.add(mpt(1,0,15));async.add(ntp(1005));const auto asyncOffset=async.add(subtitle(1,a));async.add(ntp(1008));async.add(subtitle(2,clear));async.save();
        SubtitleFileIndex asynchronous(async.path.wstring(),1000LL*10000000,[]{return false;});const auto ar=resolve(asynchronous,asyncOffset,a);
        require(ar.start==1005LL*10000000 && ar.stop==1008LL*10000000,"TMD=1111 must ignore begin/end");
        Fixture change("subtitle-control-test.mmts");change.add(ntp(1000));change.add(mpt(1,0));change.add(ntp(1005));const auto co=change.add(subtitle(1,a));change.add(ntp(1012));change.add(mpt(2,1,15));change.add(ntp(1045));change.save();
        SubtitleFileIndex changed(change.path.wstring(),1000LL*10000000,[]{return false;});require(resolve(changed,co,a).stop==1012LL*10000000,"descriptor update must initialize existing display");
        const std::string zero=R"(<tt><body><div begin="00:00:00.000" end="00:00:40.000"><p><span>Z</span></p></div></body></tt>)";
        Fixture z("subtitle-zero-test.mmts");z.add(ntp(1000));z.add(mpt(1,0));const auto zo=z.add(subtitle(1,zero));z.add(ntp(1015));z.add(subtitle(2,clear));z.save();
        SubtitleFileIndex zeros(z.path.wstring(),1000LL*10000000,[]{return false;});const auto zr=resolve(zeros,zo,zero);
        require(zr.start==1000LL*10000000 && zr.stop==1015LL*10000000,"zero begin must remain synchronized and end at clear");
        Fixture split("subtitle-buffer-boundary-test.mmts");split.add(ntp(1000));split.add(mpt(1,0));split.add(ntp(1005));
        for(int i=0;i<16;++i) split.add(tlv(0xff,Bytes(65000,0)));
        const size_t target=1024*1024-20;
        require(split.bytes.size()+4<target,"invalid test padding");split.add(tlv(0xff,Bytes(target-split.bytes.size()-4,0)));
        const auto so=split.add(subtitle(1,a));split.add(ntp(1015));split.add(subtitle(2,clear));split.save();
        SubtitleFileIndex splitIndex(split.path.wstring(),1000LL*10000000,[]{return false;});
        require(resolve(splitIndex,so,a).stop==1015LL*10000000,"MFU crossing reader chunks must retain its original packet offset");
        // A seek far ahead restarts the index near the document instead of
        // reading the gap; a seek back before the window restarts it there.
        Fixture far("subtitle-seek-far-test.mmts");far.add(ntp(1000));far.add(mpt(1,0));far.add(ntp(1005));const auto early=far.add(subtitle(1,a));far.add(ntp(1015));far.add(subtitle(2,clear));
        const size_t gapStart=far.bytes.size();
        for(int i=0;i<40;++i) far.add(tlv(0xff,Bytes(65000,0)));
        const size_t gap=far.bytes.size()-gapStart;
        far.add(ntp(2000));far.add(mpt(3,0));far.add(ntp(2005));const auto late=far.add(subtitle(4,b));far.add(ntp(2015));far.add(subtitle(5,clear));far.add(ntp(2050));far.save();
        const long long readThrough=256*1024;
        SubtitleFileIndex seeking(far.path.wstring(),1000LL*10000000,[]{return false;},readThrough,32*1024);
        require(resolve(seeking,early,a).stop==1015LL*10000000,"document before the gap resolves");
        const auto beforeSeek=seeking.BytesRead();
        const auto lr=resolve(seeking,late,b);
        require(lr.stop==2015LL*10000000,"document after a far seek resolves from the restarted window");
        require(seeking.BytesRead()-beforeSeek<static_cast<long long>(gap)/2,"far seek must not read the gap");
        require(resolve(seeking,early,a).stop==1015LL*10000000,"seek back before the window restarts it there");
        bool cancelled=true;SubtitleFileIndex stopped(f.path.wstring(),0,[&]{return cancelled;});SubtitleFileIndex::Record unused;
        require(!stopped.Resolve(first,0x100,0,unused),"stop must cancel lookahead");
        std::cout<<"Subtitle file index tests passed: finite replacement, clear, async, descriptor update, seek, far seek, cancellation\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
