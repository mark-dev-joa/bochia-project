#pragma once
// The labeler's settings for this PC (%LOCALAPPDATA%\Boccia\labeler-settings.json): the 실행 장치 and the hub the worker
// joins (design/hub/DESIGN.md). Nothing here belongs to a project.
#include <filesystem>
#include <string>

namespace boccia::labeler {
struct LabelerSettings {
    bool gpu = true;                                                   // drafts and SAM on the GPU
    std::string hubAddress = "mark-hong-pc.tail68c059.ts.net:8790";   // host:port of the hub
    std::string worker, token;                                        // the worker's name and invite code; no name, no hub
};
std::filesystem::path settingsFile();
LabelerSettings loadLabelerSettings();                       // a missing file or key gives the default
void saveLabelerSettings(const LabelerSettings& settings);   // one atomic write; keys it does not know stay as they were
}
