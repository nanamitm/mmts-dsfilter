#pragma once

#include <windows.h>
#include <strsafe.h>
#include <cstdarg>
#include <new>

inline void MmtTlvLogFileLine(const WCHAR* text)
{
    WCHAR path[MAX_PATH] = {};
    DWORD len = GetTempPathW(ARRAYSIZE(path), path);
    if (len == 0 || len >= ARRAYSIZE(path))
        return;

    HRESULT hr = StringCchCatW(path, ARRAYSIZE(path), L"mmts_dsfilter_debug.log");
    if (FAILED(hr))
        return;

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
        char stackBuf[2048];
        char* out = stackBuf;
        if (bytes > static_cast<int>(sizeof(stackBuf))) {
            out = new (std::nothrow) char[bytes];
        }
        if (out) {
            int writtenBytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, bytes, nullptr, nullptr);
            if (writtenBytes > 1) {
                DWORD ignored = 0;
                WriteFile(file, out, static_cast<DWORD>(writtenBytes - 1), &ignored, nullptr);
            }
            if (out != stackBuf)
                delete[] out;
        }
    }

    CloseHandle(file);
}

inline void MmtTlvLogInfo(const WCHAR* format, ...)
{
    WCHAR buf[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), format, args);
    va_end(args);
    OutputDebugStringW(buf);
#if defined(MMT_TLV_FILE_LOG)
    MmtTlvLogFileLine(buf);
#endif
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
#if defined(MMT_TLV_FILE_LOG)
    MmtTlvLogFileLine(buf);
#endif
#else
    UNREFERENCED_PARAMETER(format);
#endif
}
