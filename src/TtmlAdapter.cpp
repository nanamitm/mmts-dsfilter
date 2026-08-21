// The only translation unit that sees dantto4k's TTML AST. Keep it that way:
// if dantto4k's TTML layer is redesigned, this file is the whole port surface.
#include "TtmlModel.h"

#include "ttml.h"

namespace {

std::optional<DsTtml::Length> ToLength(const TTMLCssValue& value)
{
    if (!value.is<TTMLCssValueLength>())
        return std::nullopt;

    const TTMLCssValueLength length = value.getValue<TTMLCssValueLength>();
    return DsTtml::Length{length.value, length.unit};
}

std::optional<DsTtml::Length> ToOptLength(const std::optional<TTMLCssValue>& value)
{
    if (!value.has_value())
        return std::nullopt;

    return ToLength(*value);
}

std::optional<DsTtml::LengthPair> ToLengthPair(const std::optional<TTMLCssValuePair>& pair)
{
    if (!pair.has_value())
        return std::nullopt;

    const std::optional<DsTtml::Length> first = ToLength(pair->first);
    const std::optional<DsTtml::Length> second = ToLength(pair->second);
    if (!first.has_value() || !second.has_value())
        return std::nullopt;

    return DsTtml::LengthPair{*first, *second};
}

std::optional<DsTtml::Color> ToColor(const std::optional<TTMLCssValue>& value)
{
    if (!value.has_value() || !value->is<TTMLCssValueColor>())
        return std::nullopt;

    const TTMLCssValueColor color = value->getValue<TTMLCssValueColor>();
    return DsTtml::Color{color.r, color.g, color.b, color.a};
}

DsTtml::Style ToStyle(const TTMLStyle& style)
{
    DsTtml::Style out;
    out.fontSize = ToLengthPair(style.fontSize);
    out.lineHeight = ToOptLength(style.lineHeight);
    out.color = ToColor(style.color);
    out.backgroundColor = ToColor(style.backgroundColor);
    return out;
}

DsTtml::Region ToRegion(const TTMLRegion& region)
{
    DsTtml::Region out;
    out.extent = ToLengthPair(region.extent);
    out.origin = ToLengthPair(region.origin);
    return out;
}

}

namespace DsTtml {

Document Parse(const std::string& xml)
{
    const TTML source = TTMLPaser::parse(xml);

    Document document;
    for (const auto& sourceDiv : source.divTags) {
        Division& div = document.divTags.emplace_back();
        div.begin = sourceDiv.begin;
        div.end = sourceDiv.end;

        for (const auto& sourceP : sourceDiv.pTags) {
            Paragraph& p = div.pTags.emplace_back();
            p.id = sourceP.id;
            p.region = ToRegion(sourceP.region);

            for (const auto& sourceSpan : sourceP.spanTags) {
                Span& span = p.spanTags.emplace_back();
                span.text = sourceSpan.text;
                span.style = ToStyle(sourceSpan.style);
            }
        }
    }

    return document;
}

}
