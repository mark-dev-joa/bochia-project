#include "native_labeler_view.hpp"
#include <algorithm>

namespace boccia::labeler {
std::array<Span, 3> tileSpans(int focus, float width, float gap, float narrow) {
    std::array<Span, 3> spans;
    if (focus < 0 || focus > 2) {
        const float each = (width - 2 * gap) / 3;
        for (int i = 0; i < 3; ++i) spans[i] = {i * (each + gap), each};
        return spans;
    }
    narrow = std::min(narrow, (width - 2 * gap) / 4);
    const float wide = width - 2 * gap - 2 * narrow;
    float x = 0;
    for (int i = 0; i < 3; ++i) { spans[i] = {x, i == focus ? wide : narrow}; x += spans[i].width + gap; }
    return spans;
}
View fitView(int imageWidth, int imageHeight, float viewWidth, float viewHeight) {
    const float scale = std::min(viewWidth / imageWidth, viewHeight / imageHeight);
    return {scale, (viewWidth - imageWidth * scale) / 2, (viewHeight - imageHeight * scale) / 2};
}
View ballView(const cv::Rect2d& box, int imageWidth, int imageHeight, float viewWidth, float viewHeight) {
    const float fit = fitView(imageWidth, imageHeight, viewWidth, viewHeight).scale;
    const float scale = std::clamp(float(viewHeight * .34 / std::max(box.width, box.height)), fit, 14.f);
    return {scale, float(viewWidth / 2 - (box.x + box.width / 2) * scale), float(viewHeight / 2 - (box.y + box.height / 2) * scale)};
}
cv::Rect2d cropRegion(const cv::Rect2d& box, int imageWidth, int imageHeight, double factor) {
    const double side = std::min({std::max(box.width, box.height) * factor, double(imageWidth), double(imageHeight)});
    const double x = std::clamp(box.x + box.width / 2 - side / 2, 0., imageWidth - side);
    const double y = std::clamp(box.y + box.height / 2 - side / 2, 0., imageHeight - side);
    return {x, y, side, side};
}
}
