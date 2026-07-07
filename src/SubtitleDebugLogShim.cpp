#include "../../dantto4k/src/config.h"

#include <fstream>
#include <mutex>
#include <string>

namespace {

std::mutex g_subtitleDebugLogMutex;

}

void subtitleDebugLog(const std::string& line) {
    if (config.subtitleDebugLogPath.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_subtitleDebugLogMutex);
    std::ofstream stream(config.subtitleDebugLogPath, std::ios::app | std::ios::binary);
    if (stream) {
        stream << line << "\n";
    }
}
