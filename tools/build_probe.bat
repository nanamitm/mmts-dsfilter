@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1
cd /d "%~dp0"
if not exist obj mkdir obj
cl.exe /O2 /EHsc /std:c++20 /W0 /I"..\src" /I"..\..\dantto4k\src" /I"..\..\dantto4k\thirdparty\pugixml\src" mmts_probe2.cpp @sources.rsp /Fe:mmts_probe2.exe /Fo:obj\ /link /SUBSYSTEM:CONSOLE
cl.exe /O2 /EHsc /std:c++20 /W0 /I"..\src" /I"..\..\dantto4k\src" /I"..\..\dantto4k\thirdparty\pugixml\src" mmts_audio_split.cpp @sources.rsp /Fe:mmts_audio_split.exe /Fo:obj\ /link /SUBSYSTEM:CONSOLE
cl.exe /O2 /EHsc /std:c++20 /W0 /I"..\src" /I"..\..\dantto4k\src" mmts_track_probe.cpp @sources.rsp /Fe:mmts_track_probe.exe /Fo:obj\ /link /SUBSYSTEM:CONSOLE
