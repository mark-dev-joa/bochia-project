#pragma once
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <string>
#include <filesystem>
#include <ctime>

namespace boccia::labeler {
using Json = nlohmann::json;
inline std::filesystem::path fromUtf8(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(), value.end())); }
inline std::string utf8(const std::filesystem::path& value) { const auto text = value.u8string(); return {text.begin(), text.end()}; }
cv::Mat decodeMask(const Json& rle, int width, int height);
Json encodeMask(const cv::Mat& mask);
void setOccludedFlag(Json& box, bool value);   // marks the ball partly hidden; a circle merely inscribed in its box goes with it
Json maskAnnotation(const cv::Mat& mask, const Json& previous, const std::string& source);
cv::Rect2d detectionBox(const Json& box);
bool nudgeEdge(Json& box, int edge, bool expand, int width, int height);
Json annotationPayload(const Json& frame, int revision);
// The status the hub shows for this labeler (design/hub): the page, the scene, the running job, 이전 적용, the device and
// the error, with `text` saying the same in one line.
Json hubStatusJson(const std::string& page, const Json& scene, const Json& job, bool applyPrevious, bool gpu, const std::string& error,
                   const Json& progress = nullptr);   // progress (hubProgress) goes in when it is an object
// The hub's progress numbers for this labeler (design/hub/PLAN-2.md Task 3): the top bar's 검수 totals, per recording, holds,
// propagated frames, and this worker's confirmations over the last 7 local days, today and the last 10 minutes, read from
// the label file's reviewed_at/reviewed_by in catalog.summary.frames.
Json hubProgress(const Json& catalog, const std::string& worker, std::time_t now);
}
