#include "labeler_update.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace boccia::update {
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {
constexpr size_t CHUNK = size_t{1} << 20;

class Sha256 {
public:
    Sha256() {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !BCRYPT_SUCCESS(BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0)))
            throw std::runtime_error("SHA-256 is not available");
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(const void* data, size_t size) {
        auto bytes = static_cast<const unsigned char*>(data);
        while (size) {
            const auto piece = static_cast<ULONG>(std::min<size_t>(size, CHUNK));
            if (!BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(bytes), piece, 0))) throw std::runtime_error("SHA-256 failed");
            bytes += piece;
            size -= piece;
        }
    }
    std::string hex() {
        std::array<unsigned char, 32> digest{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0))) throw std::runtime_error("SHA-256 failed");
        static constexpr char digits[] = "0123456789abcdef";
        std::string text;
        for (const unsigned char byte : digest) { text += digits[byte >> 4]; text += digits[byte & 15]; }
        return text;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

fs::path fromUtf8(const std::string& text) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())); }
std::string toUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
fs::path statePath(const fs::path& root) { return root / "update" / "state.json"; }
fs::path backupFolder(const fs::path& root) { return root / "update" / "backup"; }
fs::path asideFolder(const fs::path& root) { return root / "update" / "reverted"; }   // what a revert moved away; a running launcher can be moved, not deleted
std::int64_t writeTime(const fs::path& file) { return fs::last_write_time(file).time_since_epoch().count(); }

Json readState(const fs::path& root) {
    std::ifstream input(statePath(root), std::ios::binary);
    if (!input) return Json::object();
    try {
        Json state = Json::parse(input);
        if (state.is_object()) return state;
    } catch (const std::exception&) {}
    return Json::object();
}
void writeState(const fs::path& root, const Json& state) {   // a temporary file, then a rename: never half a state
    const auto target = statePath(root);
    auto temporary = target;
    temporary += ".tmp";
    fs::create_directories(target.parent_path());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << state.dump(1);
        if (!output) throw std::runtime_error("cannot write the install state");
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("cannot write the install state");
}
void move(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("cannot move " + toUtf8(from) + " (error " + std::to_string(GetLastError()) + ")");
}

// One sync or revert of a folder at a time, across processes (a second launcher started by a double click).
class Lock {
public:
    explicit Lock(const fs::path& root) {
        std::error_code error;
        fs::path full = fs::weakly_canonical(fs::absolute(root, error), error);
        if (error) full = root;
        std::wstring text = full.wstring();
        for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
        const auto digest = sha256Hex(toUtf8(fs::path(text)));
        const std::wstring name = L"Local\\BocciaLauncher-" + std::wstring(digest.begin(), digest.begin() + 16);
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_) {
            const DWORD wait = WaitForSingleObject(handle_, 0);
            held_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }
    ~Lock() {
        if (held_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    bool held() const { return held_; }
private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
};

bool safePath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;
    for (size_t start = 0;;) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
}
bool hexHash(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool contains(const Json& list, const std::string& value) {
    if (list.is_array()) for (const auto& item : list) if (item.is_string() && item.get<std::string>() == value) return true;
    return false;
}
Json entryOf(const fs::path& file, const std::string& sha256) {
    return {{"size", fs::file_size(file)}, {"time", writeTime(file)}, {"sha256", sha256}};
}
}

std::string sha256Hex(std::string_view bytes) {
    Sha256 hash;
    hash.add(bytes.data(), bytes.size());
    return hash.hex();
}

std::string fileSha256(const fs::path& file) {
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot read " + toUtf8(file));
    const std::unique_ptr<void, decltype(&CloseHandle)> closer(handle, &CloseHandle);
    Sha256 hash;
    std::vector<char> buffer(CHUNK);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) throw std::runtime_error("cannot read " + toUtf8(file));
        if (!read) break;
        hash.add(buffer.data(), read);
    }
    return hash.hex();
}

Manifest parseManifest(const Json& json) {
    if (!json.is_object()) throw std::runtime_error("the release list is not an object");
    Manifest manifest{json.value("app", ""), json.value("id", ""), json.value("version", ""), {}};
    if (manifest.app.empty() || manifest.id.empty() || manifest.version.empty()) throw std::runtime_error("the release list lacks its app, id or version");
    const Json files = json.value("files", Json::array());
    if (!files.is_array() || files.empty()) throw std::runtime_error("the release list has no files");
    for (const auto& item : files) {
        ReleaseFile file{item.value("path", ""), item.value("size", std::uint64_t{0}), item.value("sha256", "")};
        if (!safePath(file.path)) throw std::runtime_error("an unsafe path in the release list: " + file.path);
        if (!hexHash(file.sha256)) throw std::runtime_error("a malformed hash in the release list: " + file.path);
        manifest.files.push_back(std::move(file));
    }
    return manifest;
}

bool installable(const fs::path& root, const fs::path& launcher) {
    std::error_code error;
    if (fs::is_regular_file(statePath(root), error)) return true;
    if (!fs::is_directory(root, error)) return false;
    for (const auto& entry : fs::directory_iterator(root, error)) {
        const auto name = entry.path().filename().wstring();
        if (_wcsicmp(name.c_str(), launcher.filename().c_str()) == 0 || _wcsicmp(name.c_str(), L"update") == 0) continue;
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        return false;
    }
    return !error;
}

SyncResult sync(const fs::path& root, const Manifest& manifest, const Fetch& fetch, const Progress& progress) {
    SyncResult result;
    const Lock lock(root);
    if (!lock.held()) { result.outcome = Outcome::Busy; return result; }
    std::vector<fs::path> parts;   // fetched pieces not yet in place, removed on any failure
    try {
        Json state = readState(root);
        if (contains(state.value("bad", Json::array()), manifest.id)) { result.outcome = Outcome::BadVersion; return result; }
        result.firstInstall = state.value("id", "").empty();
        std::error_code ignored;
        fs::remove_all(asideFolder(root), ignored);
        if (!fs::exists(statePath(root)))   // before anything is fetched: a cut first install goes on next time
            writeState(root, Json{{"version", ""}, {"id", ""}, {"files", Json::object()}, {"bad", Json::array()}, {"check", false}});
        Json known = state.value("files", Json::object());
        if (!known.is_object()) known = Json::object();
        Json files = Json::object();
        struct Item { const ReleaseFile* file; fs::path target, part; };
        std::vector<Item> wanted;
        std::set<std::string> listed;
        std::uint64_t total = 0;
        for (const auto& file : manifest.files) {
            listed.insert(file.path);
            const auto target = root / fromUtf8(file.path);
            if (fs::is_regular_file(target)) {
                const auto size = fs::file_size(target);
                const auto time = writeTime(target);
                const Json entry = known.value(file.path, Json());
                std::string have;
                if (entry.is_object() && entry.value("size", std::uint64_t{0}) == size && entry.value("time", std::int64_t{0}) == time) have = entry.value("sha256", "");
                if (have.empty()) have = fileSha256(target);
                if (have == file.sha256) { files[file.path] = {{"size", size}, {"time", time}, {"sha256", have}}; continue; }
            }
            wanted.push_back({&file, target, root / "update" / (file.sha256 + ".part")});
            total += file.size;
        }
        std::vector<std::string> removed;
        for (auto it = known.begin(); it != known.end(); ++it)
            if (!listed.count(it.key()) && fs::exists(root / fromUtf8(it.key()))) removed.push_back(it.key());
        if (wanted.empty() && removed.empty()) {
            if (state.value("id", "") != manifest.id || known != files) {
                state["version"] = manifest.version;
                state["id"] = manifest.id;
                state["files"] = files;
                writeState(root, state);
            }
            result.outcome = Outcome::UpToDate;
            return result;
        }
        // Everything first: nothing in place changes until every differing file is here and right.
        std::uint64_t done = 0;
        for (const auto& item : wanted) {
            fs::remove(item.part, ignored);   // a piece left by a cut; a folder in its place stays, and the write fails
            parts.push_back(item.part);
            fetch(*item.file, item.part, [&](std::uint64_t bytes) {
                done += bytes;
                result.downloaded += bytes;
                if (progress) progress(done, total);
            });
            if (!fs::is_regular_file(item.part) || fileSha256(item.part) != item.file->sha256) throw std::runtime_error("wrong content for " + item.file->path);
        }
        // The swap: each old file to update\backup, the new one in; a failure puts back everything moved, last first.
        const auto backup = backupFolder(root);
        fs::remove_all(backup, ignored);
        std::vector<std::pair<fs::path, fs::path>> moved;
        Json before = Json::object(), replaced = Json::array(), added = Json::array(), gone = Json::array();
        try {
            for (const auto& item : wanted) {
                if (fs::exists(item.target)) {
                    const auto saved = backup / fromUtf8(item.file->path);
                    move(item.target, saved);
                    moved.emplace_back(item.target, saved);
                    replaced.push_back(item.file->path);
                    if (known.contains(item.file->path)) before[item.file->path] = known[item.file->path];
                } else {
                    added.push_back(item.file->path);
                }
                move(item.part, item.target);
                moved.emplace_back(item.part, item.target);
                files[item.file->path] = entryOf(item.target, item.file->sha256);
            }
            for (const auto& path : removed) {
                const auto target = root / fromUtf8(path), saved = backup / fromUtf8(path);
                move(target, saved);
                moved.emplace_back(target, saved);
                gone.push_back(path);
                before[path] = known[path];
            }
        } catch (...) {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it) MoveFileExW(it->second.c_str(), it->first.c_str(), MOVEFILE_REPLACE_EXISTING);
            throw;
        }
        parts.clear();
        const std::string fromId = state.value("id", "");
        state["last"] = {{"from_version", state.value("version", "")}, {"from_id", fromId}, {"to_version", manifest.version}, {"to_id", manifest.id},
                         {"replaced", replaced}, {"added", added}, {"removed", gone}, {"before", before}};
        state["check"] = !fromId.empty();
        state["version"] = manifest.version;
        state["id"] = manifest.id;
        state["files"] = files;
        if (!state.contains("bad")) state["bad"] = Json::array();
        writeState(root, state);
        result.outcome = Outcome::Updated;
        return result;
    } catch (const std::exception& error) {
        std::error_code ignored;
        for (const auto& part : parts) fs::remove(part, ignored);
        result.outcome = Outcome::Failed;
        result.error = error.what();
        return result;
    }
}

bool pendingCheck(const fs::path& root) {
    const Json state = readState(root);
    const Json last = state.value("last", Json::object());
    return state.value("check", false) && last.is_object() && !last.value("from_id", "").empty();
}

void confirmStart(const fs::path& root) {
    const Lock lock(root);
    if (!lock.held()) return;
    Json state = readState(root);
    if (!state.value("check", false)) return;
    state["check"] = false;
    try { writeState(root, state); } catch (const std::exception&) {}
}

std::string revertLastUpdate(const fs::path& root) {
    const Lock lock(root);
    if (!lock.held()) return "";
    try {
        Json state = readState(root);
        const Json last = state.value("last", Json());
        if (!last.is_object() || last.value("from_id", "").empty()) return "";
        const auto backup = backupFolder(root), aside = asideFolder(root);
        const Json before = last.value("before", Json::object());
        Json files = state.value("files", Json::object());
        for (const auto& item : last.value("added", Json::array())) {
            const std::string path = item.get<std::string>();
            const auto target = root / fromUtf8(path);
            if (fs::exists(target)) move(target, aside / fromUtf8(path));
            files.erase(path);
        }
        for (const char* list : {"replaced", "removed"}) {
            for (const auto& item : last.value(list, Json::array())) {
                const std::string path = item.get<std::string>();
                const auto target = root / fromUtf8(path), saved = backup / fromUtf8(path);
                if (!fs::exists(saved)) continue;
                if (fs::exists(target)) move(target, aside / fromUtf8(path));
                move(saved, target);
                if (before.contains(path)) files[path] = before[path]; else files.erase(path);
            }
        }
        std::error_code ignored;
        fs::remove_all(aside, ignored);   // a running launcher stays until the next sync clears it
        Json bad = state.value("bad", Json::array());
        bad.push_back(last.value("to_id", ""));
        state["bad"] = bad;
        state["files"] = files;
        state["version"] = last.value("from_version", "");
        state["id"] = last.value("from_id", "");
        state["check"] = false;
        state.erase("last");
        writeState(root, state);
        return last.value("to_version", "");
    } catch (const std::exception&) {
        return "";
    }
}

std::string installedVersion(const fs::path& root) { return readState(root).value("version", ""); }
}
