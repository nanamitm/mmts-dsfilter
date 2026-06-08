# mmts-dsfilter

DirectShow source/splitter filter for MMT/TLV `.mmts` files.

This filter was built for playback/debugging with MPC-BE and MPC-HC. It uses
the `dantto4k` demuxer sources for MMT/TLV parsing and exposes HEVC video, AAC
audio, and ARIB B24 subtitle streams as DirectShow output pins.

## Features

- Loads `.mmts` files through `IFileSourceFilter`.
- Registers `.mmts` as a DirectShow source filter extension.
- Exposes HEVC video output.
- Exposes each MPT `mp4a` audio stream as a separate audio output pin.
- Outputs ordinary AAC streams as ADTS/`MEDIASUBTYPE_RAW_AAC1`.
- Outputs 22.2ch audio streams as AAC LATM/LOAS using
  `MEDIASUBTYPE_MPEG_LOAS` / `WAVE_FORMAT_MPEG_LOAS` (`0x1602`) for LAV Audio
  Decoder.
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

`tools/mmts_make_index.exe` creates a lightweight `.mmtsidx` sidecar file that
the DirectShow filter can use as a virtual edit list. The first implementation
supports a virtual start offset, so playback can skip the beginning of a large
`.mmts` file without rewriting the media file:

```powershell
tools\mmts_make_index.exe <input.mmts> --start-sec 20
```

This writes `<input.mmts>idx` (for example `recording.mmtsidx`). When the
filter loads `recording.mmts`, it looks for `recording.mmtsidx`, verifies the
recorded source size, and exposes the file as if playback started at that
offset. The media file itself is not modified. This sidecar format is intended
to grow into a seek/RAP index and edit decision list for future MMTS editing
tools.

If a dantto4k-generated `.mmtsmap` exists next to the MMTS file
(`recording.mmtsmap`), the filter also loads it during prescan. The map is used
to supplement audio/subtitle track candidates and duration information, so
tracks that appear after the first prescan window can still get output pins
when playback starts. Seek/RAP points from the map are also used as preferred
byte offsets before falling back to duration-ratio seeking.

## GitHub Release Package

The `Build Release` GitHub Actions workflow builds the Release configuration
and creates a ZIP package containing:

```text
mmts-dsfilter.ax
mmts-dsfilter.ini
register-filter.ps1
unregister-filter.ps1
README.md
```

The workflow can be run manually from GitHub Actions to create an artifact. It
also publishes the ZIP as a GitHub Release asset when a `v*` tag is pushed, for
example:

```powershell
git tag v0.1.0
git push origin v0.1.0
```

The workflow checks out `dantto4k` from the same GitHub owner as this
repository, so forks should also provide a sibling `<owner>/dantto4k`
repository.

## Register

> **Administrator privileges are required.**

From a release package, right-click each script and choose
**Run with PowerShell as administrator**:

- `register-filter.ps1` — registers the filter
- `unregister-filter.ps1` — unregisters the filter

`regsvr32` will show a dialog confirming success or failure.

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
