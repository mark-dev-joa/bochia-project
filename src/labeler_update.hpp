#pragma once
// The launcher's update engine (design/hub/LAUNCHER.md): what the hub lists against what this install holds, the files
// that differ fetched and checked by SHA-256, swapped in with the old ones kept in update\backup, and the update taken
// back when the new labeler dies at once. Static CRT and system DLLs only: the launcher runs before any of it is here.
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace boccia::update {
struct ReleaseFile { std::string path; std::uint64_t size = 0; std::string sha256; };   // path: relative, '/'-separated, UTF-8
struct Manifest { std::string app, id, version; std::vector<ReleaseFile> files; };
// std::runtime_error on an unsafe or worker-folder path (labeler/data, labeler/logs, update), a malformed hash, two paths
// equal but for case, or a list without BocciaLauncher.exe and labeler/boccia_labeler.exe.
Manifest parseManifest(const nlohmann::json& json);
std::string fileSha256(const std::filesystem::path& file);      // lower-case hex
std::string sha256Hex(std::string_view bytes);

// Writes the whole file to `part` or throws, telling `received` the size of each piece written.
using Fetch = std::function<void(const ReleaseFile& file, const std::filesystem::path& part,
                                 const std::function<void(std::uint64_t bytes)>& received)>;
using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
enum class Outcome { UpToDate, Updated, Busy, BadVersion, Failed };
struct SyncResult { Outcome outcome = Outcome::Failed; std::string error; std::uint64_t downloaded = 0; bool firstInstall = false; };

// A launcher install (update\state.json), or a folder that holds nothing but the launcher and hidden or system files.
bool installable(const std::filesystem::path& root, const std::filesystem::path& launcher);
// Under %TEMP% (a launcher run from inside a zip is extracted alone there) or on a removable drive: no place for an install.
bool temporaryOrRemovable(const std::filesystem::path& root);
// Makes the install under `root` the manifest's: nothing in place changes until every differing file is fetched and right.
SyncResult sync(const std::filesystem::path& root, const Manifest& manifest, const Fetch& fetch, const Progress& progress);
bool pendingCheck(const std::filesystem::path& root);        // the last sync replaced a version whose first start is not yet seen
void confirmStart(const std::filesystem::path& root);
std::string revertLastUpdate(const std::filesystem::path& root);   // the version taken back, "" when there is nothing to take back
std::string installedVersion(const std::filesystem::path& root);

// The hub (design/hub/LAUNCHER.md): /releases/<app> lists the current version, /releases/files/<sha256> holds each file;
// both want the worker's invite code and nothing else.
struct HubAddress { std::string address, token; };   // "host:port", the invite code
enum class Answer { Ok, Unreachable, Unauthorized, NoRelease };
Answer fetchManifest(const HubAddress& hub, const std::string& app, Manifest& out);   // a list it cannot read counts as NoRelease
// GET /me: the name the hub knows for the invite code, which names the worker (2026-10-21). A hub without /me: NoRelease.
Answer fetchWorker(const HubAddress& hub, std::string& worker);
Fetch hubFetch(const HubAddress& hub);   // streams the file to the part; throws on another answer than 200 or a cut
}
