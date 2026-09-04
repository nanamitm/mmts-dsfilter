# mmts-dsfilter

DirectShow source/splitter filter for MMT/TLV `.mmts` files and `.mmtsedit`
edit decision lists.

This filter was built for playback/debugging with MPC-BE and MPC-HC. It uses
the `dantto4k` demuxer sources for MMT/TLV parsing and exposes HEVC video, AAC
audio, and ARIB B24 subtitle streams as DirectShow output pins.

## Features

- Loads `.mmts` and `.mmtsedit` files through `IFileSourceFilter`.
- Registers `.mmts` and `.mmtsedit` as DirectShow source filter extensions.
- Exposes HEVC video output.
- Exposes each MPT `mp4a` audio stream as a separate audio output pin.
- Outputs ordinary AAC streams as ADTS/`MEDIASUBTYPE_RAW_AAC1`.
- Converts AAC LATM 22.2ch audio streams to stereo PCM output for playback.
- Preserves ordinary AAC 2ch/5.1ch streams as separate ADTS output pins when
  they coexist with a 22.2ch stream.
- Supports DirectShow seeking through `IMediaSeeking`.
- Queues downstream delivery to avoid renderer/decoder blocking the demuxer.
- Starts playback and seek recovery from HEVC RAP/IRAP frames.
- Converts ARIB B24 TTML subtitles to ASS with per-character cell layout.
- Renders cell backgrounds as merged rectangles (no edge artifacts).
- Renders DRCS (broadcaster-defined) glyphs as ASS vector drawings.

## Repository Layout

```text
src/          Filter implementation
msvc/         Visual Studio project and module definition
settings/     Sample INI configuration file
tools/        Local probe/debug helper sources
scripts/      Registration helper scripts
baseclasses/  DirectShow BaseClasses copy from Microsoft samples
```

Build outputs are intentionally ignored by Git.

## Dependencies

- Windows
- Visual Studio 2022 with C++ desktop workload
- Windows SDK
- vcpkg x64-windows dependencies used by `dantto4k`
- `dantto4k` checked out next to this repository:

```text
<workspace>\
  dantto4k\
  mmts-dsfilter\
```

The Visual Studio project currently references `..\..\dantto4k` from
`msvc/mmts-dsfilter.vcxproj`.

## Build

From the repository root:

```powershell
.\build.ps1 -Configuration Debug
```

or:

```powershell
.\build.ps1 -Configuration Release
```

The filter is produced as:

```text
msvc\x64\<Configuration>\mmts-dsfilter.ax
```

## Debug Helper Tools

`tools/mmts_audio_split.exe` scans MPT audio layout changes and reports whether
ordinary AAC streams can be converted to ADTS:

```powershell
tools\mmts_audio_split.exe <input.mmts> [--split] [--max-mb N] [--progress-mb N]
```

- `--max-mb N` limits scanning to the first `N` MB for quick investigation.
- `--progress-mb N` prints progress every `N` MB. The default is 512 MB.
- `--split` writes one file per detected audio-layout segment. It cannot be
  combined with `--max-mb`, because a partial scan may miss later layout
  changes.

`tools/subtitle_ttml_dump.exe` writes every subtitle TTML sample out as XML
and prints the B24 cell grid each paragraph asks for:

```powershell
tools\subtitle_ttml_dump.exe <input.mmts> [outdir] [--verbose]
```

- `outdir` receives `sub_NNN.xml` per sample. Omit it to only print.
- `--verbose` also prints the X position of every character.
- A cell is one font width plus one `arib-tt:letter-spacing`, and a region's
  extent is exactly the cell count times that, so the printed `laidOut` and
  `extent` columns must agree. They are what the splitter positions text
  from, so a `MISMATCH` (also the exit code) is a caption it will lay out
  wrong - ruby drifting off the character it annotates, for instance.

## Non-destructive edits (`.mmtsedit`)

The filter supports a non-destructive edit decision list in a `.mmtsedit` JSON
file placed next to the media (`recording.mmts` -> `recording.mmtsedit`).
The edited program is the concatenation of the listed source segments, played
without rewriting the media file:

```json
{
  "version": 1,
  "source": "recording.mmts",
  "sourceSize": 123456789,
  "map": "recording.mmtsmap",
  "timeline": [
    { "sourceStartMs": 25000, "sourceEndMs": 1800000 },
    { "sourceStartMs": 1860000, "sourceEndMs": 3600000 }
  ]
}
```

Opening `recording.mmts` plays the original file and does not look for an edit
file. To apply edits, open `recording.mmtsedit` directly. The filter then opens
the same-named `recording.mmts`, verifies the recorded source size, and exposes
a virtual timeline whose duration is the sum of the segment durations. A single
segment behaves as a simple in/out trim; multiple segments are concatenated,
with the demuxer jumping across the cut gaps (each segment starts at the nearest
RAP). Create and edit these files with the `mmts-edit-gui` tool.

> The earlier start-only `.mmtsidx` format has been removed; use `.mmtsedit`
> (a single segment is equivalent to the old start offset plus an end).

If a dantto4k-generated `.mmtsmap` exists next to the MMTS file
(`recording.mmtsmap`), the filter also loads it during prescan. The map is used
to supplement audio/subtitle track candidates and duration information, so
tracks that appear after the first prescan window can still get output pins
when playback starts. Seek/RAP points from the map are also used as preferred
byte offsets before falling back to duration-ratio seeking.

`MMTSMAP3` files generated by dantto4k `v20260615` or later include audio
`audioMode` and channel-count metadata. The filter uses this metadata to expose
later-appearing 5.1ch and 22.2ch tracks with clearer pin names. When a stream
changes format across MPT sections, the pin name shows the source timeline, for
example `Audio 1 AAC 2.0 -> LATM 22.2 -> AAC 5.1`. A LATM 22.2ch input is
decoded and downmixed to `PCM 48000Hz 2.0`; the pin name keeps the original
source format visible while the DirectShow media type reports the actual PCM
output format.

## GitHub Release Package

The `Build Release` GitHub Actions workflow builds the Release configuration
and creates a ZIP package containing:

```text
mmts-dsfilter.ax
avcodec-<soname>.dll
avutil-<soname>.dll
swresample-<soname>.dll
mmts-dsfilter.ini
Install_mmts-dsfilter_64.cmd
Uninstall_mmts-dsfilter_64.cmd
README.md
```

The ffmpeg DLL names carry the SONAME of whatever version vcpkg installed, so
they change when ffmpeg is bumped (ffmpeg 8 ships `avutil-60.dll`, ffmpeg 9
ships `avutil-61.dll`). Both the post-build copy and the packaging step match
them by wildcard.

The workflow can be run manually from GitHub Actions to create an artifact. It
also publishes the ZIP as a GitHub Release asset when a `v*` tag is pushed, for
example:

```powershell
git tag v0.1.0
git push origin v0.1.0
```

The workflow checks out `dantto4k` from the same GitHub owner as this
repository, so forks should also provide a sibling `<owner>/dantto4k`
repository. The checkout is pinned to a dantto4k tag (`ref:` in
`.github/workflows/release.yml`) because this project compiles dantto4k
sources directly; bump it deliberately after verifying the build against the
newer dantto4k release. Local builds are not pinned -- they use whatever is
checked out in the sibling `dantto4k` directory.

## Register

> **Administrator privileges are required.**

From a release package, right-click each script and choose
**Run as administrator**:

- `Install_mmts-dsfilter_64.cmd` — registers the filter
- `Uninstall_mmts-dsfilter_64.cmd` — unregisters the filter

The scripts call `regsvr32` silently and then show a success/failure message.
Do not delete `mmts-dsfilter.ax`, the bundled FFmpeg DLLs, or
`mmts-dsfilter.ini` after installation. The installer does not copy files
anywhere; the filter runs from the release-package folder.

For a development build, use an elevated command prompt:

```cmd
regsvr32 msvc\x64\Release\mmts-dsfilter.ax
```

To unregister:

```cmd
regsvr32 /u msvc\x64\Release\mmts-dsfilter.ax
```

## Subtitle Settings

Place `mmts-dsfilter.ini` next to `mmts-dsfilter.ax`. The release package
includes a sample INI with all options documented.

```ini
[MMTS]
; Font family used in generated ASS subtitles.
;FontName=MS Gothic

CaptionTransparency=0
BackgroundTransparency=50
ShowBackground=1
ShowRubyBackground=1
OutlineWidth=0
DelayMs=0
DebugLogPath=
VerboseLog=0
DumpSubtitleData=0
DumpSubtitleDir=
DumpSubtitleMaxFiles=200
```

| Key | Default | Description |
|-----|---------|-------------|
| `FontName` | `MS Gothic` | Font family for ASS subtitles. Cell positions are fixed per character so changing the font does not shift later characters. |
| `CaptionTransparency` | `0` | Caption text transparency: 0 = opaque, 100 = fully transparent. |
| `BackgroundTransparency` | `50` | Cell background transparency: 0 = opaque, 100 = fully transparent. |
| `ShowBackground` | `1` | Show cell background rectangles (1) or hide them (0). Background height follows TTML `lineHeight`, then region extent, then font size. If TTML provides no background color a dark gray cell background is used. |
| `ShowRubyBackground` | `1` | Show cell background rectangles behind ruby (furigana) text. |
| `OutlineWidth` | `0` | Text outline width: 0 = none, 1–10 = ASS `\bord` value. |
| `DelayMs` | `0` | Subtitle display offset in milliseconds. Negative values show earlier. |
| `DebugLogPath` | *(empty)* | File path for debug logs. Empty disables file logging. |
| `VerboseLog` | `0` | Set to `1` to emit verbose debug logs in Release builds. Debug builds always emit verbose logs. |
| `DumpSubtitleData` | `0` | Set to `1` to save raw TTML samples and referenced subtitle resources for investigation. |
| `DumpSubtitleDir` | *(empty)* | Directory for subtitle dumps. Empty writes to `subtitle_dump` next to the filter. |
| `DumpSubtitleMaxFiles` | `200` | Maximum number of subtitle samples/resources to dump. |

### DRCS (Broadcaster-Defined Characters)

When a broadcaster transmits a custom glyph (DRCS), the filter registers the
subtitle glyph resource and renders the glyph as an ASS vector drawing scaled
to fit the subtitle cell. Set `DumpSubtitleData=1` to capture raw TTML samples
and subtitle resources for investigation.

## Debug Logging

`OutputDebugString` output is quiet by default in Release builds. Set
`VerboseLog=1` in `mmts-dsfilter.ini` to emit lifecycle and verbose debug logs
to DebugView or an attached debugger. Debug builds always emit these logs.
File logging is disabled by default; set `DebugLogPath` to a log file path only
for temporary investigations. With `DebugLogPath` set and `VerboseLog=0`,
lifecycle logs are written to the file without also going to `OutputDebugString`.

## Audio Notes

The splitter keeps one DirectShow audio output pin per MPT `mp4a` audio stream
found during pre-scan so MPC-BE can switch tracks normally. Ordinary AAC streams
are delivered only after LATM/LOAS to ADTS conversion succeeds; candidate pins
are still created before that conversion is confirmed so late or fragmented
initial audio samples do not hide the track. For 8K broadcasts, 22.2ch audio is
carried as AAC LATM/LOAS rather than the ADTS form used for ordinary AAC tracks.
These LATM streams are passed through without ADTS conversion and are advertised
as:

```text
majortype:  MEDIATYPE_Audio
subtype:    MEDIASUBTYPE_MPEG_LOAS {00001602-0000-0010-8000-00AA00389B71}
formattype: FORMAT_WaveFormatEx
wFormatTag: WAVE_FORMAT_MPEG_LOAS (0x1602)
```

Some downstream filters return failures on non-selected audio branches while
another audio track is active. The splitter treats those failures as non-fatal
and continues delivering samples to all connected audio pins.

## Notes

This project includes a copy of Microsoft DirectShow BaseClasses under
`baseclasses/`; see `baseclasses/LICENSE` for that component.
