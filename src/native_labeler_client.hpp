#pragma once
#include "native_labeler_model.hpp"
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace boccia::labeler {
// HTTP 409: the engine holds a newer state than the request assumed.
struct Conflict : std::runtime_error { using std::runtime_error::runtime_error; };
class Client {
public:
    explicit Client(int port = 8767): port_(port) {}
    Json get(const std::string& path) const;
    Json post(const std::string& path, const Json& payload) const;
    cv::Mat image(const Json& member, const std::string& recording) const;
    cv::Mat thumbnail(const std::string& key) const;   // key = "recording/camera/index"
    static std::string frameQuery(const Json& member, const std::string& recording);
    // Returns the process handle (a Windows HANDLE); the caller closes it. The engine writes to engineLog(project).
    static void* startServer(int port, const std::filesystem::path& source, const std::filesystem::path& project,
                             const std::filesystem::path& exports);
    static std::filesystem::path engineLog(const std::filesystem::path& project);
    static std::string engineError(const std::filesystem::path& project);   // last line of that log, empty if none
private:
    std::vector<unsigned char> request(const std::string& method, const std::string& path, const std::string& body) const;
    int port_;
};
}
