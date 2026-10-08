// Checks of the C++ labeler engine, one group per module. Each group mirrors the Python test of the same
// name, so a behaviour the two engines must share has the same check on both sides.
#include "labeler_util.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace boccia::labeler;
namespace fs = std::filesystem;
namespace {
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
struct TempDir {
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / ("boccia-engine-" + randomHex(6))) { fs::create_directories(path); }
    ~TempDir() { std::error_code ignored; fs::remove_all(path, ignored); }
};

void utilTests() {
    check(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc wrong");
    Sha256 pieces; pieces.update("ab"); pieces.update("c");
    check(pieces.hex() == sha256Hex("abc"), "incremental sha256 differs from one-shot");
    check(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of empty input wrong");
    const auto name = randomHex(16);
    check(name.size() == 32 && name != randomHex(16) && name.find_first_not_of("0123456789abcdef") == std::string::npos, "random hex name malformed");
    const auto stamp = utcIso();
    check(stamp.size() == 32 && stamp[10] == 'T' && stamp.substr(26) == "+00:00" && stamp[19] == '.', "utc timestamp format wrong: " + stamp);
    check(localStamp().size() == 15 && localStamp()[8] == '-', "local stamp format wrong");
    TempDir temp;
    const auto file = temp.path / "nested" / "value.json";
    atomicJson(file, Json{{"b", 1}, {"a", Json::array({1, 2})}}, true);
    auto contents = [&] { std::ifstream in(file, std::ios::binary); return std::string((std::istreambuf_iterator<char>(in)), {}); };   // closed again before the next replace
    std::string text = contents();
    check(text == "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": 1\n}\n", "indented json file wrong: " + text);
    atomicJson(file, Json{{"b", 1}}, false);
    text = contents();
    check(text == "{\"b\":1}\n", "compact json file wrong: " + text);
    int entries = 0; for (const auto& entry : fs::directory_iterator(file.parent_path())) { (void)entry; ++entries; }
    check(entries == 1, "temporary file left beside the json");
    check(pathLess("a/b", "a-b/c") && !pathLess("a-b/c", "a/b"), "path order must compare pieces, not characters");
    check(pathLess("D:/x/Capture-2/session.json", "d:/x/capture-10/session.json") == (std::wstring(L"capture-2") < std::wstring(L"capture-10")), "path order must ignore case");
    check(!pathLess("a/b", "a/b"), "equal paths are not less");
    std::cout << "PASS: sha256, random names, timestamps, atomic json and python path order\n";
}
}

int main(int argc, char** argv) try {
    (void)argc; (void)argv;
    utilTests();
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << "\n"; return 1; }
