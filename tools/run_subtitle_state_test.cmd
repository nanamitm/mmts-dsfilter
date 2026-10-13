@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /EHsc /std:c++20 /utf-8 /I"..\src" /I"..\..\dantto4k\src" subtitle_state_test.cpp ..\src\TtmlAdapter.cpp ..\..\dantto4k\src\ttml\parser.cpp ..\..\dantto4k\src\ttml\resolver.cpp ..\..\dantto4k\src\ttml\style.cpp ..\..\dantto4k\src\pugixml.cpp /Fo:obj\ /Fe:subtitle_state_test.exe
if errorlevel 1 exit /b 1
subtitle_state_test.exe
