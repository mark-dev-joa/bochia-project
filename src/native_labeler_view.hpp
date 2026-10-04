#pragma once
#include "native_labeler_model.hpp"
#include <array>
#include <map>
#include <vector>

namespace boccia::labeler {
struct Span { float x = 0, width = 0; };
struct View { float scale = 1, x = 0, y = 0; };
// focus < 0이면 같은 폭 세 칸. 아니면 초점 타일이 남는 폭을 모두 갖는다. C, A, B 순서는 유지한다.
std::array<Span, 3> tileSpans(int focus, float width, float gap = 10, float narrow = 184);
View fitView(int imageWidth, int imageHeight, float viewWidth, float viewHeight);
// 선택한 공이 보기 높이의 약 34%를 차지한다. 화면 맞춤보다 작아지지 않고 14배를 넘지 않는다.
View ballView(const cv::Rect2d& box, int imageWidth, int imageHeight, float viewWidth, float viewHeight);
// 공 확대 조각의 정사각형 영역. 이미지 가장자리에서는 줄이지 않고 안쪽으로 민다.
cv::Rect2d cropRegion(const cv::Rect2d& box, int imageWidth, int imageHeight, double factor = 2.1);
}
