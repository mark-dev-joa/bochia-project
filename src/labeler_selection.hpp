#pragma once
// Conservative, within-camera representative-frame selection, as labeler_selection.py builds it: the review and
// training subset changes, never images or annotations. Every excluded frame points at a retained frame and
// records the measured changes.
#include "labeler_catalog.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace boccia::labeler {
Json defaultSelectionRules();   // RULES of labeler_selection.py, with the same value types
Json selectionRules(const Json& overrides);
struct BoxPair { int i = 0, j = 0; double distance = 0; };
// One-to-one correspondence of the boxes of two frames, or nothing when a ball moved, appeared or vanished.
std::optional<std::vector<BoxPair>> matchBoxes(const Json& a, const Json& b, const Json& rules = defaultSelectionRules());
struct FrameFeatures {
    cv::Mat scene, image;          // the scene scaled to rules["scene_width"], and the frame itself
    Json boxes;
    std::vector<cv::Mat> masks;    // per box; empty when the box has no mask
};
FrameFeatures frameFeatures(const cv::Mat& image, const Json& boxes, const Json& rules = defaultSelectionRules());
cv::Mat ballPatch(const cv::Mat& image, const Json& box);   // 48x48x3 float: the ball with context, blurred
struct FrameComparison { bool duplicate = false; std::string reason; Json metrics = Json::object(); };
FrameComparison compareFrames(const FrameFeatures& a, const FrameFeatures& b, const std::vector<BoxPair>& pairs, const Json& rules = defaultSelectionRules());
using SelectionProgress = std::function<void(int done, int total, const std::string& camera)>;
Json buildSelection(const Catalog& catalog, const Json& snapshot, const SelectionProgress& progress = {}, const Json& ruleOverrides = Json::object());
}
