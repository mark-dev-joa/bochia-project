#pragma once
// Automatic draft annotations with immutable human edits and resumable runs, as labeler_auto.py makes them.
// The existing native-pixel circle cache is authenticated against serial, id and time of each raw frame; stereo
// associations select likely balls; SAM segments them. Similar neighbouring crops may reuse a transformed mask
// until the next anchor. These are pseudo labels, not ground truth or a measurement of detector accuracy.
#include "labeler_assist.hpp"
#include "labeler_store.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace boccia::labeler {
struct Seed { double x = 0, y = 0, radius = 0; };
Json seedBbox(const Seed& seed, int width, int height);
cv::Mat appearance(const cv::Mat& image, const Seed& seed);   // 40x40x3 float around the seed
bool similar(const cv::Mat& a, const cv::Mat& b);
// The mask of a previous frame moved and scaled from its seed to the new one, as an auto_tracking box.
Json transferMask(const Json& previousBox, const Seed& previousSeed, const Seed& seed, cv::Size shape);
class SeedSource {
public:
    virtual ~SeedSource() = default;
    virtual std::string method() const = 0;
    virtual std::pair<std::vector<Seed>, std::string> get(const std::string& recording, const std::string& camera, int index, const cv::Mat& image) = 0;
};
class SeedCache : public SeedSource {
public:
    SeedCache(const Catalog& catalog, const std::filesystem::path& root);
    std::string method() const override { return method_; }
    std::pair<std::vector<Seed>, std::string> get(const std::string& recording, const std::string& camera, int index, const cv::Mat& image) override;
    size_t covered() const { return covered_.size(); }
    size_t seeded() const { return seeds_.size(); }
    size_t seedCount() const;
private:
    std::map<std::tuple<std::string, std::string, int>, std::vector<Seed>> seeds_;
    std::set<std::tuple<std::string, std::string, int>> covered_;
    std::string method_ = "circle_candidates";
};
class Segmenter {   // what the job needs from EfficientSAM, so a test can stand in for it
public:
    virtual ~Segmenter() = default;
    virtual Json segment(const cv::Mat& image, const std::string& key, const Json& points, const Json& radius, const Json& seedBox) = 0;
};
class AutoLabelJob {
public:
    AutoLabelJob(Project& project, std::filesystem::path root, std::filesystem::path modelDir);
    ~AutoLabelJob();
    Json status() const;
    // Starts the run; the hooks replace the seed cache and the model (tests). Refuses without the model files.
    Json start(std::function<std::unique_ptr<SeedSource>()> seeds = {}, std::function<std::unique_ptr<Segmenter>()> segmenters = {});
    Json stop();
    void persist();
    bool ready() const;
    const std::filesystem::path statusPath;
private:
    void run(std::function<std::unique_ptr<SeedSource>()> seeds, std::function<std::unique_ptr<Segmenter>()> segmenters);
    Project& project_;
    const std::filesystem::path root_, modelDir_;
    mutable std::recursive_mutex lock_;
    Json state_ = {{"status", "idle"}};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
}
