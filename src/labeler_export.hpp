#pragma once
// Dataset export of the reviewed, selected frames: COCO with masks, COCO with detection boxes, YOLO, crops and a
// manifest, in a fresh folder every time, as ball_labeler.export_dataset writes it.
#include "labeler_store.hpp"

namespace boccia::labeler {
Json exportDataset(const Catalog& catalog, const Json& snapshot, const std::filesystem::path& exportRoot, bool crops = true,
                   const std::function<void(int)>& progress = {});
Json startExport(Project& project, bool crops);   // the background job; returns the job state
}
