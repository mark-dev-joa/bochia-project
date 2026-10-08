#pragma once
// Labels of a confirmed representative frame carried to its repeated frames (the ones the repeated-scene filter left
// out of the review), so they can join the training data without a person looking at each (2026-10-08 user request).
// A repeated frame keeps its own draft ball where the confirmed one has a match, measured again on its own image, takes
// the confirmed ball's flags, gains the balls a person added and loses the ones a person removed. It becomes
// "propagated": exported like a confirmed frame, never a review target, and dropped back to a draft the moment a
// person edits it.
#include "labeler_store.hpp"

namespace boccia::labeler {
Json propagationStatus(const Project& project);
Json propagationCounts(const Project& project);   // {"representatives", "duplicates", "pending", "done", "carry"}
// force: also the frames already up to date with their representative. carry: also the review targets that follow a
// confirmed frame within 1.5 s and hold fewer balls than it get its labels as drafts (still to be confirmed by a person).
Json startPropagation(Project& project, bool force, bool carry = true);
}
