#include "labeler_log.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
std::mutex lock;
std::ofstream file;   // under lock
fs::path path;        // under lock
std::function<void(const std::string&)> sink;   // under lock
std::tm localNow(int* milliseconds = nullptr) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    if (milliseconds) *milliseconds = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &seconds);
    return local;
}
}

void openLog(const fs::path& folder) {
    std::lock_guard guard(lock);
    if (file.is_open()) return;
    const std::tm local = localNow();
    char day[16];
    std::strftime(day, sizeof day, "%Y-%m-%d", &local);
    std::vector<fs::path> folders{folder};
    wchar_t* data = nullptr; size_t size = 0;
    if (_wdupenv_s(&data, &size, L"LOCALAPPDATA") == 0 && data) { folders.push_back(fs::path(data) / "Boccia" / "logs"); std::free(data); }
    for (const auto& candidate : folders) {
        std::error_code error;
        fs::create_directories(candidate, error);
        const auto target = candidate / ("labeler-" + std::string(day) + ".log");
        file.open(target, std::ios::app | std::ios::binary);
        if (file.is_open()) { path = target; return; }
        file.clear();
    }
}

void logLine(const std::string& text) {
    int milliseconds = 0;
    const std::tm local = localNow(&milliseconds);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    char fraction[8];
    std::snprintf(fraction, sizeof fraction, ".%03d", milliseconds);
    const std::string line = std::string(stamp) + fraction + "  " + text;
    std::function<void(const std::string&)> notify;
    {
        std::lock_guard guard(lock);
        if (file.is_open()) { file << line << "\n"; file.flush(); }
        notify = sink;
    }
    if (notify) notify(line);   // outside the lock: the sink takes locks of its own (the hub client)
}

void setLogSink(std::function<void(const std::string&)> next) { std::lock_guard guard(lock); sink = std::move(next); }

fs::path logFile() { std::lock_guard guard(lock); return path; }
}
