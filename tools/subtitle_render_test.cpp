// Compile the production rendering helpers in this translation unit so the
// assertions exercise ASS output rather than a separate imitation of it.
#include <streams.h>
#include "DsHandler.h"
#include "MmtTlvOutputPin.h"
#include "SubtitleFileIndex.h"
// Dependencies are already included; expose only the filter's own state here.
#define private public
#include "MmtTlvSplitter.h"
#undef private
#include "../src/MmtTlvSplitter.cpp"
#include <iostream>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main()
{
    try {
        const auto document = DsTtml::Parse(R"(<tt xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><styling><style xml:id="s" tts:fontSize="96px 96px" tts:backgroundColor="#000000FF"/></styling><layout><region xml:id="r" tts:origin="100px 100px" tts:extent="192px 96px"/></layout></head><body><div><p region="r"><span style="s" begin="00:00:01.000" end="00:00:02.000">A</span><span style="s" begin="00:00:03.000" end="00:00:04.000">B</span></p></div></body></tt>)");
        const auto slices = DsTtml::Slices(document);
        require(slices.size() == 2, "render must receive separate time slices");
        TtmlDebugStats stats;
        const auto cue = ExtractTtmlPlainText(slices[1].document, stats, 0);
        require(cue.text == "B", "inactive span must not enter rendered plain text");
        std::vector<std::string> events;
        MmtsCaptionSettings settings;
        const auto& p = slices[1].document.divTags.front().pTags.front();
        AppendAssCellLayoutEvents(0, p, 1.0, 0, false, 0, settings, events, true);
        bool positioned = false;
        for (const auto& event : events) {
            require(event.find("}A") == std::string::npos, "inactive glyph must not be painted");
            require(event.find("m 50 ") == std::string::npos, "inactive background must not be painted");
            if (event.find("\\pos(98,") != std::string::npos && event.back() == 'B') positioned = true;
        }
        require(positioned, "B must preserve its original cell position after inactive A");
        auto invisibleGlyph = document;
        invisibleGlyph.divTags.front().pTags.front().spanTags.front().text = "\xEE\x80\x80"; // U+E000
        const auto glyphSlices = DsTtml::Slices(invisibleGlyph);
        require(!ExtractTtmlPlainText(glyphSlices[1].document, stats, 0).missingGlyph,
                "an expired external glyph must not defer a later visible span");
        HRESULT hr = S_OK;
        CMmtTlvSplitter filter(nullptr, &hr);
        require(SUCCEEDED(hr), "filter construction failed");
        filter.m_firstPts = 1000LL * 10000000;
        filter.m_currentDts = 5LL * 10000000;
        filter.m_subtitleFileIndex = std::make_unique<SubtitleFileIndex>(L"", filter.m_firstPts, [] { return false; });
        CFilterDemuxerHandler::SubtitleStreamInfo info;
        info.streamIndex = 0;
        info.packetId = 0x100;
        info.componentTag = 0x30;
        info.control = MmtTlv::AdditionalAribSubtitleInfo{};
        info.control->tmd = 2;
        SubtitleFileIndex::Record record;
        record.arrival = 1005LL * 10000000;
        record.base = 1000LL * 10000000;
        record.start = 1010LL * 10000000;
        record.stop = 1015LL * 10000000;
        record.control = info.control;
        const std::string xml = R"(<tt><body><div begin="00:00:10.000" end="00:00:40.000"><p><span>A</span></p></div></body></tt>)";
        require(filter.ProcessSubtitleDocument(info, reinterpret_cast<const uint8_t*>(xml.data()), xml.size(), 1, &record), "regular document should render");
        require(filter.m_scheduledSubtitleSamples.size() == 1, "future ASS must wait outside the output queue");
        require(filter.m_scheduledSubtitleSamples.front().start == 10LL * 10000000 &&
                filter.m_scheduledSubtitleSamples.front().stop == 15LL * 10000000,
                "production delivery must preserve reference start and indexed clear end");
        filter.PumpScheduledSubtitleSamples(9LL * 10000000);
        require(filter.m_scheduledSubtitleSamples.size() == 1, "future caption dispatched too early");
        filter.PumpScheduledSubtitleSamples(10LL * 10000000);
        require(filter.m_scheduledSubtitleSamples.empty(), "due caption not dispatched");
        const std::string gaiji = R"(<tt xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><styling><style xml:id="s" tts:fontSize="96px 96px"/></styling><layout><region xml:id="r" tts:origin="100px 100px" tts:extent="96px 96px"/></layout></head><body><div begin="00:00:10.000"><p region="r"><span style="s">&#xE000;</span></p></div></body></tt>)";
        CMmtTlvSplitter::DeferredSubtitleSample waiting;
        waiting.info = info;
        waiting.indexed = record;
        waiting.data.assign(gaiji.begin(), gaiji.end());
        filter.m_deferredSubtitleSamples.push_back(waiting);
        filter.ProcessDeferredSubtitleSamples();
        require(filter.m_deferredSubtitleSamples.size() == 1, "missing glyph should wait within the original display interval");
        filter.m_currentDts = 15LL * 10000000;
        filter.ProcessDeferredSubtitleSamples();
        require(filter.m_deferredSubtitleSamples.empty() && filter.m_scheduledSubtitleSamples.empty(),
                "expired glyph-waiting document must not resurrect");
        filter.m_deferredSubtitleSamples.push_back(waiting);
        auto clearRecord = record;
        clearRecord.clear = true;
        const std::string clear = "<tt/>";
        filter.ProcessSubtitleDocument(info, reinterpret_cast<const uint8_t*>(clear.data()), clear.size(), 2, &clearRecord);
        require(filter.m_deferredSubtitleSamples.empty(), "clear packet must invalidate deferred glyphs");
        std::cout << "Subtitle production ASS rendering tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
