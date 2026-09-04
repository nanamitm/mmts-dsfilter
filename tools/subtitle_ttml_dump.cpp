// subtitle_ttml_dump.cpp
// Diagnostic: writes every subtitle TTML sample in an .mmts file out as XML and
// prints the B24 cell grid each paragraph asks for - region origin, per-span
// cell pitch, and the X every character lands on.
//
// The pitch is one font width plus one arib-tt:letter-spacing, and a region's
// extent is exactly the cell count times that pitch, so the "extent" column is
// a self-check: a paragraph whose laid-out width disagrees with its own extent
// is one the splitter will lay out wrong. That is the shape the ruby-offset
// defect had - ruby is positioned from its own region origin, so it stays put
// while a base line built on the wrong pitch drifts out from under it.
//
// Coordinates are printed in the splitter's ASS space (1920x1080), i.e. the
// TTML's 3840x2160 halved, so they can be compared with the layout log.
//
// Build: see build_probe.bat next to this file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "mmtTlvDemuxer.h"
#include "demuxerHandler.h"
#include "mmtStream.h"
#include "mpuProcessorBase.h"  // NOPTS_VALUE, MfuData
#include "stream.h"
#include "timebase.h"
#include "TtmlModel.h"

using namespace MmtTlv;

namespace {

constexpr double kAssScale = 1920.0 / 3840.0;

int64_t PtsToMs(uint64_t pts, const MmtStream& stream)
{
    if (pts == NOPTS_VALUE)
        return -1;

    const auto& tb = stream.getTimeBase();
    if (tb.den <= 0)
        return static_cast<int64_t>(pts) / 90;

    return static_cast<int64_t>(static_cast<double>(pts) * tb.num * 1000.0 / tb.den);
}

double FontWidthAss(const DsTtml::Span& span)
{
    if (!span.style.fontSize.has_value() || span.style.fontSize->first.unit != "px")
        return 0;

    return span.style.fontSize->first.value * kAssScale;
}

double LetterSpacingAss(const DsTtml::Span& span)
{
    if (!span.style.letterSpacing.has_value() || span.style.letterSpacing->unit != "px")
        return 0;

    return span.style.letterSpacing->value * kAssScale;
}

// One cell: what the pen advances by, which is not the width the glyph is drawn
// at. Mirrors AssCellAdvanceFromSpan() in the splitter.
double CellAdvanceAss(const DsTtml::Span& span)
{
    const double fontWidth = FontWidthAss(span);
    return fontWidth > 0 ? fontWidth + LetterSpacingAss(span) : 0;
}

size_t Utf8SequenceLength(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    return 4;
}

// Returns true when the paragraph's laid-out width matches its own extent.
bool PrintParagraph(const DsTtml::Paragraph& p, bool verbose)
{
    const bool hasOrigin = p.region.origin.has_value();
    const double originXAss = hasOrigin ? p.region.origin->first.value * kAssScale : 0;
    const double extentXAss = p.region.extent.has_value() ? p.region.extent->first.value * kAssScale : 0;

    double x = originXAss;
    for (const auto& span : p.spanTags) {
        const double advance = CellAdvanceAss(span);
        if (verbose) {
            std::printf("    span font=%.1f spacing=%.1f advance=%.1f text=%s\n",
                        FontWidthAss(span), LetterSpacingAss(span), advance, span.text.c_str());
        }

        for (size_t i = 0; i < span.text.size();) {
            const size_t length = Utf8SequenceLength(static_cast<unsigned char>(span.text[i]));
            if (verbose) {
                std::printf("      x=%7.1f %.*s\n", x, static_cast<int>(length), span.text.c_str() + i);
            }
            x += advance;
            i += length;
        }
    }

    const double laidOutWidth = x - originXAss;
    const bool matches = extentXAss <= 0 || std::fabs(laidOutWidth - extentXAss) < 1.0;

    std::printf("  p=%-10s originX=%7.1f laidOut=%7.1f extent=%7.1f  %s\n",
                p.id.empty() ? "(none)" : p.id.c_str(), originXAss, laidOutWidth, extentXAss,
                extentXAss <= 0 ? "no extent" : (matches ? "ok" : "MISMATCH"));
    return matches;
}

class DumpHandler : public DemuxerHandler {
public:
    DumpHandler(std::string outDir, bool verbose)
        : outDir_(std::move(outDir)), verbose_(verbose) {}

    int mismatches() const { return mismatches_; }

    void onSubtitleData(const MmtStream& stream, const MfuData& mfu) override {
        const int64_t ms = PtsToMs(mfu.pts, stream);
        std::printf("[sample %03d] pts=%lld ms bytes=%zu\n", index_, static_cast<long long>(ms),
                    mfu.data.size());

        if (!outDir_.empty()) {
            char path[MAX_PATH];
            std::snprintf(path, sizeof(path), "%s\\sub_%03d.xml", outDir_.c_str(), index_);
            std::ofstream ofs(path, std::ios::binary);
            if (ofs) {
                ofs.write(reinterpret_cast<const char*>(mfu.data.data()),
                          static_cast<std::streamsize>(mfu.data.size()));
                std::printf("  wrote %s\n", path);
            } else {
                std::printf("  WARNING: cannot write %s\n", path);
            }
        }

        ++index_;

        std::string xml(mfu.data.begin(), mfu.data.end());
        DsTtml::Document ttml;
        try {
            ttml = DsTtml::Parse(xml);
        } catch (const std::exception& e) {
            std::printf("  WARNING: TTML parse failed: %s\n", e.what());
            return;
        }

        for (const auto& div : ttml.divTags) {
            for (const auto& p : div.pTags) {
                if (!PrintParagraph(p, verbose_))
                    ++mismatches_;
            }
        }
    }

private:
    std::string outDir_;
    bool        verbose_ = false;
    int         index_ = 0;
    int         mismatches_ = 0;
};

void DemuxAll(std::ifstream& ifs, MmtTlvDemuxer& demuxer)
{
    constexpr size_t kChunk = 65536;
    std::vector<uint8_t> buf;

    for (;;) {
        const size_t old = buf.size();
        buf.resize(old + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data()) + old, kChunk);
        const size_t got = static_cast<size_t>(ifs.gcount());
        buf.resize(old + got);
        if (got == 0)
            break;

        Common::ReadStream stream(buf);
        while (!stream.isEof()) {
            if (demuxer.demux(stream) == DemuxStatus::NotEnoughBuffer)
                break;
        }

        const size_t consumed = buf.size() - stream.leftBytes();
        if (consumed > 0)
            buf.erase(buf.begin(), buf.begin() + consumed);
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::printf("Usage: subtitle_ttml_dump <input.mmts> [outdir] [--verbose]\n"
                    "  outdir      directory to write sub_NNN.xml into (omit to only print)\n"
                    "  --verbose   also print every character's X position\n");
        return 2;
    }

    std::string outDir;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--verbose")
            verbose = true;
        else
            outDir = argv[i];
    }

    // A directory that is not there yet is the common case: the caller just
    // names one next to the capture.
    if (!outDir.empty() && !CreateDirectoryA(outDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        std::printf("ERROR: cannot create %s\n", outDir.c_str());
        return 2;
    }

    std::ifstream ifs(argv[1], std::ios::binary);
    if (!ifs.is_open()) {
        std::printf("ERROR: cannot open %s\n", argv[1]);
        return 2;
    }

    MmtTlvDemuxer demuxer;
    DumpHandler handler(outDir, verbose);
    demuxer.setDemuxerHandler(handler);
    DemuxAll(ifs, demuxer);

    if (handler.mismatches() > 0) {
        std::printf("\n%d paragraph(s) do not fill their own region extent.\n", handler.mismatches());
        return 1;
    }

    std::printf("\nEvery paragraph fills its region extent.\n");
    return 0;
}
