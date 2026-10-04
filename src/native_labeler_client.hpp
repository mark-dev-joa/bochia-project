#pragma once
#include "native_labeler_model.hpp"
#include <filesystem>
#include <vector>

namespace boccia::labeler {
class Client {
public:
    explicit Client(int port = 8767): port_(port) {}
    Json get(const std::string& path) const;
    Json post(const std::string& path, const Json& payload) const;
    cv::Mat image(const Json& member, const std::string& recording) const;
    static std::string frameQuery(const Json& member, const std::string& recording);
    static void startServer(int port, const std::filesystem::path& source, const std::filesystem::path& project,
                            const std::filesystem::path& exports);
private:
    std::vector<unsigned char> request(const std::string& method, const std::string& path, const std::string& body) const;
    int port_;
};
}
