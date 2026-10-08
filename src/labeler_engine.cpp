#include "labeler_engine.hpp"
#include "labeler_util.hpp"
#include <stdexcept>

namespace boccia::labeler {
Engine::Engine(EnginePaths given)
    : paths(std::move(given)), catalog_(paths.source), project_(catalog_, paths.project, paths.exports), assist_(paths.models), auto_(project_, paths.root, paths.models) {}
Engine::~Engine() { auto_.stop(); }
Json Engine::catalog() const { return {{"recordings", catalog_.metadata()}, {"source", utf8(catalog_.source)}, {"project_path", utf8(project_.path)}, {"summary", project_.summary()}}; }
Json Engine::nativeCatalog() const { return boccia::labeler::nativeCatalog(project_); }
Json Engine::frame(const std::string& recording, const std::string& camera, int index) const { return project_.frame(recording, camera, index); }
cv::Mat Engine::image(const std::string& recording, const std::string& camera, int index) const { return catalog_.read(recording, camera, index); }
cv::Mat Engine::thumbnail(const std::string& key, int width) {
    const auto first = key.find('/'), second = key.find('/', first + 1);
    if (first == std::string::npos || second == std::string::npos) throw std::runtime_error("촬영 또는 카메라를 찾지 못했습니다.");
    return boccia::labeler::thumbnail(project_, key.substr(0, first), key.substr(first + 1, second - first - 1), std::stoi(key.substr(second + 1)), width);
}
Json Engine::saveAnnotation(const Json& payload) { return {{"summary", project_.saveFrame(payload)}}; }
Json Engine::saveSplits(const Json& payload) { return {{"summary", project_.saveSplits(payload)}}; }
Json Engine::nativeAction(const std::string& action, const Json& payload) { return boccia::labeler::nativeAction(project_, action, payload); }
Json Engine::assist(const std::string& method, const Json& payload) { return assistance(project_, assist_, method, payload); }
Json Engine::assistStatus() const { return assist_.status(); }
Json Engine::exportStatus() const { std::lock_guard guard(project_.lock); return project_.job; }
Json Engine::exports() const { return {{"exports", exportHistory(project_)}}; }
Json Engine::colorFillStatus() const { return boccia::labeler::colorFillStatus(project_); }
Json Engine::circleRefineStatus() const { return boccia::labeler::circleRefineStatus(project_); }
Json Engine::autoStatus() const { return {{"job", auto_.status()}, {"summary", project_.summary()}}; }
Json Engine::autoStart() { return auto_.start(); }
Json Engine::autoStop() { return auto_.stop(); }
}
