#pragma once
// Small building blocks the C++ engine shares with nothing else: hashing, random names, timestamps,
// atomic JSON files and the path order Python's sorted(Path) uses.
#include "native_labeler_model.hpp"
#include <string>
#include <string_view>

namespace boccia::labeler {
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(std::string_view bytes);
    std::string hex();   // finishes the hash; the object must not be used afterwards
private:
    void* algorithm_ = nullptr;
    void* hash_ = nullptr;
};
std::string sha256Hex(std::string_view bytes);
std::string randomHex(int bytes = 16);       // uuid4().hex when bytes is 16
std::string utcIso();                        // datetime.now(timezone.utc).isoformat(): 2026-10-08T05:11:22.123456+00:00
std::string localStamp();                    // datetime.now().strftime("%Y%m%d-%H%M%S")
// Writes the value to a temporary file beside the target, flushes it to disk and replaces the target in one step.
void atomicJson(const std::filesystem::path& path, const Json& value, bool indent);
// The order sorted(pathlib.Path) gives on Windows: lower-cased path pieces compared one by one.
bool pathLess(const std::filesystem::path& a, const std::filesystem::path& b);
}
