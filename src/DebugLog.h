#pragma once

#include <windows.h>
#include <strsafe.h>
#include <cstdarg>
#include <new>
#include <mutex>
#include <string>

struct MmtTlvDebugLogConfig {
    std::wstring filePath;
    bool verbose = false;
    long long playbackTraceCenterMs = -1;
    long long playbackTraceWindowMs = 3000;
};

inline MmtTlvDebugLogConfig& MmtTlvDebugLogState()
{
    static MmtTlvDebugLogConfig state;
    return state;
}

inline std::mutex& MmtTlvDebugLogMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline void MmtTlvConfigureDebugLog(const std::wstring& filePath, bool verbose,
                                    long long playbackTraceCenterMs = -1,
                                    long long playbackTraceWindowMs = 3000)
{
    std::lock_guard<std::mutex> lock(MmtTlvDebugLogMutex());
    MmtTlvDebugLogState().filePath = filePath;
    MmtTlvDebugLogState().verbose = verbose;
    MmtTlvDebugLogState().playbackTraceCenterMs = playbackTraceCenterMs;
    MmtTlvDebugLogState().playbackTraceWindowMs = playbackTraceWindowMs > 0 ? playbackTraceWindowMs : 3000;
}

inline bool MmtTlvIsPlaybackTraceTimeMs(long long timeMs)
{
    std::lock_guard<std::mutex> lock(MmtTlvDebugLogMutex());
    const auto& state = MmtTlvDebugLogState();
    if (state.playbackTraceCenterMs < 0 || timeMs < 0)
        return false;
    const long long delta = timeMs - state.playbackTraceCenterMs;
    return delta >= -state.playbackTraceWindowMs && delta <= state.playbackTraceWindowMs;
}

inline bool MmtTlvIsVerboseLogEnabled()
{
#if defined(_DEBUG) || defined(DEBUG)
    return true;
#else
    std::lock_guard<std::mutex> lock(MmtTlvDebugLogMutex());
    return MmtTlvDebugLogState().verbose;
#endif
}

inline std::wstring MmtTlvDebugLogPath()
{
    std::lock_guard<std::mutex> lock(MmtTlvDebugLogMutex());
    return MmtTlvDebugLogState().filePath;
}

inline void MmtTlvLogFileLine(const WCHAR* text)
{
    std::wstring path = MmtTlvDebugLogPath();
    if (path.empty())
        return;

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
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
    if (MmtTlvIsVerboseLogEnabled())
        OutputDebugStringW(buf);
    MmtTlvLogFileLine(buf);
}

inline void MmtTlvLogDebug(const WCHAR* format, ...)
{
    if (!MmtTlvIsVerboseLogEnabled()) {
        UNREFERENCED_PARAMETER(format);
        return;
    }

    WCHAR buf[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), format, args);
    va_end(args);
    OutputDebugStringW(buf);
    MmtTlvLogFileLine(buf);
}
