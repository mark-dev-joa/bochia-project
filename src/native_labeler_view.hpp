#pragma once
#include "native_labeler_model.hpp"
#include <array>
#include <map>
#include <vector>

namespace boccia::labeler {
struct Span { float x = 0, width = 0; };
// 3936 -> "3,936"
inline std::string grouped(int value) { auto text = std::to_string(value); for (int at = int(text.size()) - 3; at > 0; at -= 3) text.insert(at, ","); return text; }
struct View { float scale = 1, x = 0, y = 0; };
// focus < 0이면 같은 폭 세 칸. 아니면 초점 타일이 남는 폭을 모두 갖는다. C, A, B 순서는 유지한다.
std::array<Span, 3> tileSpans(int focus, float width, float gap = 10, float narrow = 184);
View fitView(int imageWidth, int imageHeight, float viewWidth, float viewHeight);
// 선택한 공이 보기 높이의 약 34%를 차지한다. 화면 맞춤보다 작아지지 않고 14배를 넘지 않는다.
View ballView(const cv::Rect2d& box, int imageWidth, int imageHeight, float viewWidth, float viewHeight);
// 공 확대 조각의 정사각형 영역. 이미지 가장자리에서는 줄이지 않고 안쪽으로 민다.
cv::Rect2d cropRegion(const cv::Rect2d& box, int imageWidth, int imageHeight, double factor = 2.1);
enum class MemberState { Todo, Editing, Done, Hold, Duplicate };
struct BallRef { int camera = -1, index = -1; };
MemberState memberState(const Json& member);
// 검수 대상인 카메라끼리 공 개수가 다르면 true.
bool countsDiffer(const Json& scene);
// 카메라 순서, 그 안에서는 박스의 왼쪽 좌표 순서.
std::vector<BallRef> ballOrder(const std::vector<Json>& annotations);
BallRef nextBall(const std::vector<BallRef>& order, BallRef current, int step);
// 새 윤곽이 고칠 공. 선택한 공의 박스가 그 점을 포함하면 선택한 공, 아니면 그 점을 포함하는 마지막 박스, 없으면 -1(새 공).
int replaceTarget(const std::vector<cv::Rect2d>& boxes, cv::Point2d point, int selected);
// 점들을 둘러싼 사각형의 중심. 둘레를 찍은 폴리곤이 어느 공인지는 첫 점이 아니라 이 중심으로 정한다.
cv::Point2d boundsCenter(const std::vector<cv::Point2d>& points);
struct Circle { double x = 0, y = 0, r = 0; };
// 점들에 가장 잘 맞는 원(최소제곱). 점이 셋보다 적거나 한 줄에 있으면 반지름 0.
Circle fitCircle(const std::vector<cv::Point>& points);
struct SplitTotal { int recordings = 0, images = 0, balls = 0, empty = 0; };
struct SplitCheck { bool ok = false; std::string text, detail; };
std::map<std::string, SplitTotal> splitTotals(const Json& stats, const Json& splits);
// 순서: 묶음이 두 쪽에 걸치는지, 세 쪽에 이미지가 있는지, 공 없는 이미지, 촬영 1개뿐인 쪽, 같은 날.
std::vector<SplitCheck> splitChecks(const Json& stats, const Json& splits, const Json& groups);
// groups는 촬영 ID -> 묶음 ID. 묶음 ID는 항상 그 묶음에서 ID가 가장 앞선 촬영이다.
// 고른 촬영이 속한 묶음을 모두 하나로 합친다.
void mergeGroups(Json& groups, const std::vector<std::string>& recordings);
// 촬영 하나를 묶음에서 뺀다. 남은 촬영은 계속 한 묶음이다.
void leaveGroup(Json& groups, const std::string& recording);
}
