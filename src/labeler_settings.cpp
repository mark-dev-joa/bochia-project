#include "labeler_settings.hpp"
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fstream>
#include <stdexcept>

namespace boccia::labeler {
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
// The file as it is, or an empty object when there is none or it cannot be read.
Json readSettings() {
    try {
        std::ifstream input(settingsFile(), std::ios::binary);
        if (input) { Json stored = Json::parse(input); if (stored.is_object()) return stored; }
    } catch (const std::exception&) {}
    return Json::object();
}
// A temporary file beside it, then a rename: a reader sees the old file or the new one. Written here rather than with
// labeler_util's atomicJson so the launcher, built without the engine, saves the same file (design/hub/LAUNCHER.md).
void writeAtomically(const fs::path& target, const std::string& text) {
    auto temporary = target;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << text;
        if (!output) throw std::runtime_error("could not save the labeler settings");
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("could not save the labeler settings");
    }
}
}

fs::path settingsFile() {
    wchar_t location[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", location, 32768);
    return fs::path(location) / "Boccia/labeler-settings.json";
}

LabelerSettings loadLabelerSettings() {
    LabelerSettings settings;
    const Json stored = readSettings();
    settings.gpu = stored.value("device", "gpu") != "cpu";
    const Json hub = stored.value("hub", Json::object());
    if (hub.is_object()) {
        if (const auto address = hub.value("address", ""); !address.empty()) settings.hubAddress = address;
        settings.worker = hub.value("worker", "");
        settings.token = hub.value("token", "");
    }
    return settings;
}

void saveLabelerSettings(const LabelerSettings& settings) {
    Json stored = readSettings();
    stored["device"] = settings.gpu ? "gpu" : "cpu";
    stored["hub"] = Json{{"address", settings.hubAddress}, {"worker", settings.worker}, {"token", settings.token}};
    fs::create_directories(settingsFile().parent_path());
    writeAtomically(settingsFile(), stored.dump(2) + "\n");
}
}
