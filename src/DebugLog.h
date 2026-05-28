#pragma once

#include <windows.h>
#include <strsafe.h>
#include <cstdarg>

inline void MmtTlvLogInfo(const WCHAR* format, ...)
{
    WCHAR buf[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), format, args);
    va_end(args);
    OutputDebugStringW(buf);
}

inline void MmtTlvLogDebug(const WCHAR* format, ...)
{
#if defined(_DEBUG) || defined(DEBUG) || defined(MMT_TLV_VERBOSE_LOG)
    WCHAR buf[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), format, args);
    va_end(args);
    OutputDebugStringW(buf);
#else
    UNREFERENCED_PARAMETER(format);
#endif
}
