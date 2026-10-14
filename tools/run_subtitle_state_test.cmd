@echo off
setlocal
rem Find Visual Studio rather than assume an edition or version. vcvars64.bat
rem itself runs vswhere.exe from PATH, so the installer's folder goes there.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo Visual Studio C++ x64 tools are required
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /EHsc /std:c++20 /utf-8 /I"..\src" /I"..\..\dantto4k\src" subtitle_state_test.cpp ..\src\TtmlAdapter.cpp ..\..\dantto4k\src\ttml\parser.cpp ..\..\dantto4k\src\ttml\resolver.cpp ..\..\dantto4k\src\ttml\style.cpp ..\..\dantto4k\src\pugixml.cpp /Fo:obj\ /Fe:subtitle_state_test.exe
if errorlevel 1 exit /b 1
rem By full path: with NoDefaultCurrentDirectoryInExePath set, cmd does not
rem look in the current directory for a bare name.
"%~dp0subtitle_state_test.exe"
