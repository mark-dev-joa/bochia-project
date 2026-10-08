#pragma once
// Balls the external detector found that the automatic draft has no box for: segmented, measured and added as drafts.
// The draft's seeds came from the stereo matching, which left the colour camera's slot empty for about a third of the
// balls (2026-10-08 investigation); this pass takes the detector's own balls instead, with options a person can tune,
// run as a trial on a few frames, inspect and then apply.
#include "labeler_store.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace boccia::labeler {
struct Detection { double x = 0, y = 0, radius = 0, coverage = 0; };
// The detector's balls per "recording/camera/index", for the frames the catalog can match by serial, time and id.
std::map<std::string, std::vector<Detection>> loadDetections(const Catalog& catalog, const std::filesystem::path& root);

struct MissingOptions {
    std::string cameras = "colour";        // "colour": the colour camera only; "all": every camera
    double coverage = .8;                  // the rim coverage the detector reported, at least
    double minRadius = 6, maxRadius = 45;  // pixels
    double margin = .5;                    // the rim margin of the circle fit (the project's by default)
    bool dryRun = false;                   // segment and measure, report, write nothing
    bool countOnly = false;                // only count the candidates
    int limitFrames = 0;                   // a trial on this many frames, spread over the recordings (0 = all)
    int sampleLimit = 8;                   // the balls reported for a look
    std::string name;                      // a trial's name, for assembling trials later
};
MissingOptions missingOptions(const Json& payload, const Project& project);
Json missingFillStatus(const Project& project);
Json startMissingFill(Project& project, const std::filesystem::path& root, const std::filesystem::path& modelDir, const MissingOptions& options);
Json stopMissingFill(Project& project);
}
