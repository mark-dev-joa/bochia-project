#pragma once
// The native app's scene index and its review-then-group-split operations, as labeler_native.py defines them.
// The GUI uses the same Project and atomic annotation store as the browser. No action writes the recordings.
#include "labeler_store.hpp"

namespace boccia::labeler {
inline constexpr int nativeApiVersion = 5;   // 5: built into the app. 4: colour fill. 3: grow-masks. 2: stats, thumbnails, exports.
Json sceneIndex(const Catalog& catalog);     // every source frame once; unmatched frames stay visible as sparse scenes
Json targets(const Project& project);        // the scene members the selection keeps, the review and export targets
// The data version: the labels, the review targets, and how the recordings are assigned and grouped.
std::string fingerprint(const Project& project, const Json* state = nullptr, const Json* groups = nullptr);
bool splitValid(const Project& project);
Json recordingStats(const Project& project);
cv::Mat thumbnail(Project& project, const std::string& recording, const std::string& camera, int index, int width = 128);
Json exportHistory(const Project& project);  // completed export folders, newest first
int colorable(const Project& project);       // colour-camera balls with a mask and no colour yet
Json growable(const Project& project);       // {"frames", "balls"} a mask-growing pass would touch
Json colorFillStatus(const Project& project);
Json startColorFill(Project& project);       // fills colours in the background; one revision at the end
int circleable(const Project& project);      // masked balls whose box the Canny circle has not measured yet
Json circleRefineStatus(const Project& project);
Json startCircleRefine(Project& project);    // measures every such ball's circle in the background
Json nativeCatalog(const Project& project);
std::pair<Json, Json> groupPlan(const Project& project, const Json& groups);   // splits per recording, target counts
Json nativeAction(Project& project, const std::string& action, const Json& payload);
class BallAssist;
// The proposal tools of one frame: segment, circles, propagate and polygon. Nothing here writes labels.
Json assistance(const Project& project, BallAssist& assist, const std::string& method, const Json& payload);
}
