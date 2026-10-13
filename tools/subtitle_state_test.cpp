#include "TtmlModel.h"
#include <stdexcept>
#include <iostream>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    try {
        const auto doc = DsTtml::Parse(R"(<tt><body><div><p begin="00:00:10.000" end="00:00:12.000"><span>A</span></p><p begin="00:00:20.000" end="00:00:22.000"><span>B</span></p></div></body></tt>)");
        const auto slices = DsTtml::Slices(doc);
        require(slices.size() == 2, "paragraphs must have separate display intervals");
        require(slices[0].begin == 10000 && slices[0].end == 12000, "A timing lost");
        require(slices[1].begin == 20000 && slices[1].end == 22000, "B timing lost");
        const auto spans = DsTtml::Slices(DsTtml::Parse(R"(<tt><body><div begin="00:00:05.000" end="00:00:30.000"><p><span begin="00:00:02.000" end="00:00:04.000">A</span><span begin="00:00:10.000" end="00:00:12.000">B</span></p></div></body></tt>)"));
        require(spans.size() == 2 && spans[0].begin == 7000 && spans[1].begin == 15000, "span relative timing lost");
        const auto& secondP = spans[1].document.divTags.front().pTags.front();
        require(!secondP.spanTags.front().visible && secondP.spanTags.back().visible, "inactive span must preserve B's cell position");
        require(DsTtml::Parse("<tt></tt>").divTags.empty(), "clear packet rejected");
        require(DsTtml::Slices(DsTtml::Parse(R"(<tt><body><div begin="00:00:00.000"><p><span>A</span></p></div></body></tt>)")).front().begin == 0, "zero begin rejected");
        const auto async = DsTtml::Slices(DsTtml::Parse(R"(<tt><body><div begin="invalid"><p begin="00:00:10.000"><span>A</span></p></div></body></tt>)", false));
        require(async.front().begin == 0 && !async.front().end, "async must ignore timing");
        bool rejected = false;
        try { DsTtml::Parse(R"(<tt><body><div begin="invalid"><p><span>A</span></p></div></body></tt>)"); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "invalid sync timing must not silently switch modes");
        std::cout << "Subtitle model timing tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
