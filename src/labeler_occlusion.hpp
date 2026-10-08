#pragma once
// Balls partly hidden by a hand, a person or the frame edge, marked by themselves (2026-10-08 user request: marking
// each by hand is too much). The evidence is what the tool already measures: the share of the rim the Canny edge
// supports, the mask's circularity and the box's aspect. A rim hidden by another ball on the floor is not occlusion in
// this sense (the ball's full circle is still certain), so a missing arc that faces a neighbouring ball is exempt.
#include "labeler_store.hpp"
#include <filesystem>

namespace boccia::labeler {
struct OcclusionOptions {
    double coverage = .72;        // rim sectors with edge support, below which the ball counts as hidden
    double circularity = .8;      // mask area over the disc of its box, below which it counts as hidden
    double aspect = .85;          // short side over long side of the box, below which it counts as hidden
    bool includeReviewed = false; // also a person's confirmed images, but only ever turning the flag on there
    bool dryRun = false, countOnly = false;
    int limitFrames = 0, sampleLimit = 8;
};
OcclusionOptions occlusionOptions(const Json& payload);
Json occlusionStatus(const Project& project);
Json startOcclusionMark(Project& project, const OcclusionOptions& options);
Json stopOcclusionMark(Project& project);
}
