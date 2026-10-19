#include "labeler_settings.hpp"
#include "labeler_util.hpp"
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fstream>

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
    atomicJson(settingsFile(), stored, true);
}
}
