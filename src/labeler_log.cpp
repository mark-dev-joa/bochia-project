#include "labeler_log.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <vector>

namespace boccia::labeler {
namespace fs = std::filesystem;
namespace {
std::mutex lock;
HANDLE file = INVALID_HANDLE_VALUE;   // under lock; opened for FILE_APPEND_DATA, so each line lands whole at the end even
                                      // when two labelers on this PC write the same file (an ofstream seeks, then writes:
                                      // two processes overwrite each other's line)
fs::path path;                        // under lock
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
    if (file != INVALID_HANDLE_VALUE) return;
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
        const HANDLE handle = CreateFileW(target.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) { file = handle; path = target; return; }
    }
}

void logLine(const std::string& text) {
    std::function<void(const std::string&)> notify;
    std::string line;
    {
        std::lock_guard guard(lock);   // the stamp is taken under the lock, so the file keeps the lines in time order
        int milliseconds = 0;
        const std::tm local = localNow(&milliseconds);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
        char fraction[8];
        std::snprintf(fraction, sizeof fraction, ".%03d", milliseconds);
        line = std::string(stamp) + fraction + "  " + text;
        if (file != INVALID_HANDLE_VALUE) {
            const std::string bytes = line + "\n";
            DWORD written = 0;
            WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr);
        }
        notify = sink;
    }
    if (notify) notify(line);   // outside the lock: the sink takes locks of its own (the hub client)
}

void setLogSink(std::function<void(const std::string&)> next) { std::lock_guard guard(lock); sink = std::move(next); }

fs::path logFile() { std::lock_guard guard(lock); return path; }
}
