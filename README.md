# mmts-dsfilter

DirectShow source/splitter filter for MMT/TLV `.mmts` files.

This filter was built for playback/debugging with MPC-BE and MPC-HC. It uses
the `dantto4k` demuxer sources for MMT/TLV parsing and exposes HEVC video plus
AAC audio streams as DirectShow output pins.

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

## Repository Layout

```text
src/          Filter implementation
msvc/         Visual Studio project and module definition
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

## Register

The helper scripts automatically elevate to administrator rights when needed.

To register a Release build:

```powershell
.\scripts\register-filter.ps1 -Configuration Release
```

To register a Debug build:

```powershell
.\scripts\register-filter.ps1 -Configuration Debug
```

To unregister:

```powershell
.\scripts\unregister-filter.ps1 -Configuration Release
```

You can also pass an explicit `.ax` path:

```powershell
.\scripts\register-filter.ps1 -FilterPath .\msvc\x64\Debug\mmts-dsfilter.ax
```

Manual registration is also possible from an elevated command prompt:

```cmd
regsvr32 msvc\x64\Debug\mmts-dsfilter.ax
```

To unregister:

```cmd
regsvr32 /u msvc\x64\Debug\mmts-dsfilter.ax
```

## Debug Logging

Essential lifecycle logs are emitted in all builds through `OutputDebugString`.
Verbose sample delivery, subtitle layout, TTML lookahead, and per-stream probe
logs are emitted only in Debug builds. Define `MMT_TLV_VERBOSE_LOG` for Release
builds if detailed logs are needed temporarily.

## Audio Notes

The splitter keeps one DirectShow audio output pin per playable audio stream so
MPC-BE can switch tracks normally. For 8K broadcasts, 22.2ch audio is carried as
AAC LATM/LOAS rather than the ADTS form used for ordinary AAC tracks. These
LATM streams are passed through without ADTS conversion and are advertised as:

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
