#pragma once
// dsfilter-owned TTML model.
//
// dantto4k's TTML AST (TTMLPTag / TTMLSpanTag / TTMLCssValue ...) is reachable
// from TtmlAdapter.cpp only. Everything else in this project works against the
// types below, so a dantto4k TTML redesign is a one-file port instead of a
// change scattered across the splitter and the diagnostic tools.
//
// The shape is deliberately flatter than the source AST: the CSS variants are
// resolved at the boundary, so a value present here is always of the type its
// field name implies, and no call site has to catch std::bad_variant_access.
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <utility>

namespace DsTtml {

struct Length {
    float value{};
    std::string unit;
};

using LengthPair = std::pair<Length, Length>;

struct Color {
    uint8_t r{};
    uint8_t g{};
    uint8_t b{};
    uint8_t a{};
};

struct Style {
    // Populated only when the source value really was a <length> (a pair needs
    // both components to be lengths) or a <color>; a keyword or a bare number
    // reads as absent, which is how the previous getValue<>() + catch did it.
    std::optional<LengthPair> fontSize;
    std::optional<Length> lineHeight;
    // arib-tt:letter-spacing. B24 advances the pen by fontSize + letterSpacing
    // per cell, so this is part of the layout, not a typographic nicety.
    std::optional<Length> letterSpacing;
    std::optional<Color> color;
    std::optional<Color> backgroundColor;
};

struct Span {
    std::string text;
    Style style;
};

struct Region {
    std::optional<LengthPair> extent;
    std::optional<LengthPair> origin;
};

// Member names follow the source AST (spanTags / pTags / divTags) so that the
// existing call sites keep reading the same way.
struct Paragraph {
    std::string id;  // diagnostic only, carried through for the layout logs
    Region region;
    std::list<Span> spanTags;
};

struct Division {
    std::optional<uint64_t> begin;  // milliseconds
    std::optional<uint64_t> end;    // milliseconds
    std::list<Paragraph> pTags;
};

struct Document {
    std::list<Division> divTags;
};

// Parses a TTML document. Propagates whatever the underlying parser throws,
// matching the previous direct TTMLPaser::parse() behaviour.
Document Parse(const std::string& xml);

}
