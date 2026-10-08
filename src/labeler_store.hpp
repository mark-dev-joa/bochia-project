#pragma once
// The label project file and its representative-frame sidecar, kept exactly as ball_labeler.Project and
// labeler_selection.SelectionFile keep them, so the web labeler and the native app open the same files.
#include "labeler_catalog.hpp"
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace boccia::labeler {
// Another writer moved the project on: the request assumed an older revision (the engine's old HTTP 409).
struct Conflict : std::runtime_error { using std::runtime_error::runtime_error; };
inline constexpr const char* splitNames[] = {"train", "val", "test"};
std::filesystem::path selectionPath(const std::filesystem::path& project);   // <stem>-selection.json beside the project
class SelectionFile {
public:
    SelectionFile(const Catalog& catalog, std::filesystem::path path);
    const std::filesystem::path path;
    Json read();                        // null when there is no sidecar; throws when the sidecar is invalid
    Json view(const Json& annotations); // {"enabled": false, "frames": {}} or the decisions with human work preserved
private:
    const Catalog& catalog_;
    std::optional<int64_t> stamp_;
    Json value_;
};
class Project {
public:
    Project(const Catalog& catalog, const std::filesystem::path& project, const std::filesystem::path& exportRoot);
    const Catalog& catalog;
    const std::filesystem::path path, exportRoot;
    mutable std::recursive_mutex lock;
    Json state;
    Json job = {{"status", "idle"}};    // the export job
    mutable SelectionFile selection;    // its sidecar cache refreshes on read
    Json summary() const;
    int commitAutomatic(const std::vector<std::pair<std::string, Json>>& entries);   // background drafts, own revision
    Json mutate(int expectedRevision, const std::function<void(Json&)>& callback);  // writes, then returns summary()
    Json frame(const std::string& recording, const std::string& camera, int index) const;   // with transient contours
    Json saveFrame(const Json& payload);
    Json saveSplits(const Json& payload);
    void write(const Json& replacement) const;
};
std::vector<std::pair<std::string, Json>> selectedReviewed(const Json& snapshot);
}
