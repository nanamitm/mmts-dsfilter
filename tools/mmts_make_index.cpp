// mmts_make_index.cpp
// Creates a lightweight sidecar file consumed by mmts-dsfilter.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>

static void printUsage()
{
    std::printf("Usage: mmts_make_index <input.mmts> [--start-sec N | --start-ms N] [--output file.mmtsidx]\n");
}

static bool parseInt64(const wchar_t* text, int64_t& value)
{
    if (!text || !*text)
        return false;

    errno = 0;
    wchar_t* end = nullptr;
    const long long parsed = std::wcstoll(text, &end, 10);
    if (errno != 0 || !end || *end != L'\0')
        return false;

    value = static_cast<int64_t>(parsed);
    return true;
}

static std::filesystem::path defaultIndexPath(const std::filesystem::path& input)
{
    return std::filesystem::path(input.wstring() + L"idx");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        printUsage();
        return 1;
    }

    const std::filesystem::path input(argv[1]);
    std::filesystem::path output = defaultIndexPath(input);
    int64_t startMs = 20000;

    for (int i = 2; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--start-sec") == 0) {
            int64_t sec = 0;
            if (++i >= argc || !parseInt64(argv[i], sec) || sec < 0) {
                printUsage();
                return 1;
            }
            startMs = sec * 1000;
        } else if (std::wcsncmp(argv[i], L"--start-sec=", 12) == 0) {
            int64_t sec = 0;
            if (!parseInt64(argv[i] + 12, sec) || sec < 0) {
                printUsage();
                return 1;
            }
            startMs = sec * 1000;
        } else if (std::wcscmp(argv[i], L"--start-ms") == 0) {
            if (++i >= argc || !parseInt64(argv[i], startMs) || startMs < 0) {
                printUsage();
                return 1;
            }
        } else if (std::wcsncmp(argv[i], L"--start-ms=", 11) == 0) {
            if (!parseInt64(argv[i] + 11, startMs) || startMs < 0) {
                printUsage();
                return 1;
            }
        } else if (std::wcscmp(argv[i], L"--output") == 0) {
            if (++i >= argc || !*argv[i]) {
                printUsage();
                return 1;
            }
            output = argv[i];
        } else if (std::wcsncmp(argv[i], L"--output=", 9) == 0) {
            output = argv[i] + 9;
            if (output.empty()) {
                printUsage();
                return 1;
            }
        } else {
            printUsage();
            return 1;
        }
    }

    std::error_code ec;
    const uint64_t sourceSize = std::filesystem::file_size(input, ec);
    if (ec) {
        std::wprintf(L"error: cannot stat %ls\n", input.c_str());
        return 1;
    }

    std::ofstream ofs(output, std::ios::binary);
    if (!ofs) {
        std::wprintf(L"error: cannot write %ls\n", output.c_str());
        return 1;
    }

    ofs << "MMTSIDX 1\n";
    ofs << "source_size=" << sourceSize << "\n";
    ofs << "start_ms=" << startMs << "\n";
    ofs << "mode=virtual-start\n";
    ofs << "note=Initial lightweight sidecar; future versions may add seek/RAP maps.\n";
    ofs.close();
    if (!ofs) {
        std::wprintf(L"error: failed to finish writing %ls\n", output.c_str());
        return 1;
    }

    std::wprintf(L"wrote %ls start=%lld ms source_size=%llu\n",
                 output.c_str(),
                 static_cast<long long>(startMs),
                 static_cast<unsigned long long>(sourceSize));
    return 0;
}
