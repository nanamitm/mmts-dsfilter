// The only translation unit that sees dantto4k's TTML layer. Keep it that way:
// if dantto4k's TTML layer is redesigned, this file is the whole port surface.
#include "TtmlModel.h"

#include "ttml/parser.h"
#include "ttml/resolver.h"

#include <stdexcept>
#include <variant>

namespace {

using arib::ttml::LengthPair;
using arib::ttml::StyleColorValue;
using arib::ttml::StyleLineHeightValue;
using arib::ttml::StyleProperties;
using arib::ttml::SyncMode;

// dantto4k's parser accepts only "px" lengths, so anything that reaches us as a
// length is in px. The unit is still carried because the splitter checks it.
constexpr const char* kPx = "px";

std::optional<DsTtml::LengthPair> ToLengthPair(const std::optional<LengthPair>& pair)
{
    if (!pair.has_value())
        return std::nullopt;

    return DsTtml::LengthPair{
        DsTtml::Length{static_cast<float>(pair->x), kPx},
        DsTtml::Length{static_cast<float>(pair->y), kPx},
    };
}

std::optional<DsTtml::Length> ToLength(const std::optional<double>& value)
{
    if (!value.has_value())
        return std::nullopt;

    return DsTtml::Length{static_cast<float>(*value), kPx};
}

std::optional<DsTtml::Length> ToLineHeight(const std::optional<StyleLineHeightValue>& value)
{
    if (!value.has_value())
        return std::nullopt;

    // "normal" carries no length. The old adapter mapped a keyword to absent
    // through its is<TTMLCssValueLength>() check, so do the same here.
    if (const double* length = std::get_if<double>(&*value))
        return DsTtml::Length{static_cast<float>(*length), kPx};

    return std::nullopt;
}

std::optional<DsTtml::Color> ToColor(const std::optional<StyleColorValue>& value)
{
    if (!value.has_value())
        return std::nullopt;

    return DsTtml::Color{value->r(), value->g(), value->b(), value->a()};
}

DsTtml::Style ToStyle(const StyleProperties& style)
{
    DsTtml::Style out;
    out.fontSize = ToLengthPair(style.font_size);
    out.lineHeight = ToLineHeight(style.line_height);
    out.letterSpacing = ToLength(style.letter_spacing);
    out.color = ToColor(style.color);
    out.backgroundColor = ToColor(style.background_color);
    return out;
}

DsTtml::Region ToRegion(const arib::ttml::resolved::Paragraph& paragraph)
{
    // The old parser only read the region attribute of <p>. dantto4k now
    // resolves it on <span> as well, so fall back to the first span that has
    // one - that can only add layout where there previously was none.
    const arib::ttml::resolved::Region* region =
        paragraph.region.has_value() ? &*paragraph.region : nullptr;
    if (!region) {
        for (const auto& span : paragraph.spans) {
            if (span.region.has_value()) {
                region = &*span.region;
                break;
            }
        }
    }

    DsTtml::Region out;
    if (region) {
        out.extent = ToLengthPair(region->style.extent);
        out.origin = ToLengthPair(region->style.origin);
    }
    return out;
}

std::string ToText(const std::vector<arib::ttml::ast::SpanContent>& content)
{
    // A <br/> becomes '\n', which EscapeAssText() turns into an ASS "\N" and
    // CountAssTextLines() counts. The old parser took only pugixml's first text
    // node, so text after a break used to be dropped outright.
    std::string text;
    for (const auto& part : content) {
        if (const std::string* run = std::get_if<std::string>(&part))
            text += *run;
        else
            text.push_back('\n');
    }
    return text;
}

// Sync mode is what yields the begin/end the model exposes, but it also parses
// the timing attributes strictly. Fall back to Async - which skips timing
// entirely - so a document with an unparsable timestamp still renders its text
// instead of throwing the whole cue away.
std::optional<arib::ttml::resolved::Document> ResolveDocument(const std::string& xml)
{
    for (const SyncMode mode : {SyncMode::Sync, SyncMode::Async}) {
        auto parsed = arib::ttml::parse(xml, mode);
        if (parsed.has_error() || !parsed.document)
            continue;

        auto resolved = arib::ttml::resolve(*parsed.document, mode);
        if (resolved.has_error() || !resolved.document)
            continue;

        return std::move(*resolved.document);
    }

    return std::nullopt;
}

}

namespace DsTtml {

Document Parse(const std::string& xml)
{
    const auto source = ResolveDocument(xml);
    if (!source.has_value())
        throw std::runtime_error("failed to parse TTML document");

    Document document;
    if (!source->division.has_value())
        return document;

    const auto& sourceDiv = *source->division;
    Division& div = document.divTags.emplace_back();
    if (sourceDiv.timing.begin.has_value())
        div.begin = static_cast<uint64_t>(sourceDiv.timing.begin->count());
    if (sourceDiv.timing.end.has_value())
        div.end = static_cast<uint64_t>(sourceDiv.timing.end->count());

    for (const auto& sourceP : sourceDiv.paragraphs) {
        Paragraph& p = div.pTags.emplace_back();
        p.id = sourceP.id.value_or(std::string());
        p.region = ToRegion(sourceP);

        for (const auto& sourceSpan : sourceP.spans) {
            Span& span = p.spanTags.emplace_back();
            span.text = ToText(sourceSpan.content);
            span.style = ToStyle(sourceSpan.style);
        }
    }

    return document;
}

}
