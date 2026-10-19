#pragma once
// The labeler's log (2026-10-15: an import that failed on another PC left nothing to read): one line per event with its
// time, in <program>/logs/labeler-<date>.log, or %LOCALAPPDATA%/Boccia/logs when that cannot be written. Lines are
// dropped until openLog() is called, so tests and hidden checks write none.
#include <filesystem>
#include <functional>
#include <string>

namespace boccia::labeler {
void openLog(const std::filesystem::path& folder);
void logLine(const std::string& text);   // UTF-8
std::filesystem::path logFile();         // the file in use, empty when none
void setLogSink(std::function<void(const std::string& line)> sink);   // gets every line logLine() makes, time included; nullptr stops it
}
