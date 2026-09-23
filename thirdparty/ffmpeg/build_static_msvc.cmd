@echo off
rem Build the static FFmpeg libraries vendored in this directory.
rem
rem   build_static_msvc.cmd <ffmpeg-source-dir> [build-dir]
rem
rem MSYS_DIR selects the MSYS installation that provides bash, make and nasm
rem (default: C:\MSYS). See build_static_msvc.sh for the configuration.
rem
rem The libraries are built with Visual Studio 2022 (v143) by default, since a
rem static library can only be linked by the same or a newer MSVC toolset and
rem build.ps1 defaults to the Visual Studio 2022 toolset (v143). VS_VERSION
rem overrides the vswhere version range.

setlocal
if "%~1" == "" (
	echo usage: %~nx0 ^<ffmpeg-source-dir^> [build-dir]
	exit /b 1
)
if not defined MSYS_DIR set "MSYS_DIR=C:\MSYS"
if not defined VS_VERSION set "VS_VERSION=[17.0,18.0)"

for /f "usebackq delims=" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -version "%VS_VERSION%" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%i"
if not defined VS_DIR (
	echo Visual Studio with the x64 C++ tools was not found.
	exit /b 1
)
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem MSYS tools must come before any other sed/awk on PATH (e.g. Git's), but
rem MSVC's link.exe has to win over coreutils' link.exe.
set "PATH=%VCToolsInstallDir%bin\Hostx64\x64;%MSYS_DIR%\bin;%PATH%"
set "MSYSTEM=MSYS"
set "CHERE_INVOKING=1"
"%MSYS_DIR%\bin\bash.exe" "%~dp0build_static_msvc.sh" "%~f1" %2
