// The launcher's update engine (design/hub/PLAN-LAUNCHER.md Tasks 3-5): sync, backup and revert on a scratch folder.
#include "labeler_update.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace boccia::update;

namespace {
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}
fs::path below(const fs::path& root, const std::string& path) {
    return root / fs::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

struct Scratch {
    fs::path path;
    explicit Scratch(const std::string& name) : path(fs::temp_directory_path() / ("boccia-update-" + name + "-" + std::to_string(GetCurrentProcessId()))) {
        std::error_code ignored;
        fs::remove_all(path, ignored);
        fs::create_directories(path);
    }
    ~Scratch() { std::error_code ignored; fs::remove_all(path, ignored); }
};

// A release as manifest path -> bytes; its Fetch writes the bytes it was built with.
struct Release {
    std::map<std::string, std::string> files;
    Manifest manifest(const std::string& version) const {
        Manifest made{"labeler", version, version, {}};
        for (const auto& [path, bytes] : files) made.files.push_back({path, bytes.size(), sha256Hex(bytes)});
        return made;
    }
    Fetch fetch() const {
        return [files = files](const ReleaseFile& file, const fs::path& part, const std::function<void(std::uint64_t)>& received) {
            const std::string& bytes = files.at(file.path);
            std::ofstream output(part, std::ios::binary);
            if (!output) throw std::runtime_error("cannot write " + part.string());
            output << bytes;
            received(bytes.size());
        };
    }
};

Release first() {
    return {{{"BocciaLauncher.exe", "launcher"}, {"labeler/boccia_labeler.exe", "exe v1"}, {"labeler/a.dll", "a v1"},
             {"models/boccia/m.onnx", "model"}, {"cuda/c.dll", "cuda"}}};
}
fs::path installed(const Scratch& scratch) {   // a root with the launcher and v1 in place
    const auto root = scratch.path / "root";
    write(root / "BocciaLauncher.exe", "launcher");
    const auto release = first();
    check(sync(root, release.manifest("v1"), release.fetch(), {}).outcome == Outcome::Updated, "v1 did not install");
    return root;
}
bool partsLeft(const fs::path& root) {
    for (const auto& entry : fs::directory_iterator(root / "update")) if (entry.path().extension() == ".part") return true;
    return false;
}

void first_install_places_every_file() {
    Scratch scratch("first");
    const auto root = scratch.path / "root";
    write(root / "BocciaLauncher.exe", "launcher");
    const auto release = first();
    check(installable(root, root / "BocciaLauncher.exe"), "a folder with the launcher alone must be installable");
    std::uint64_t reported = 0;
    const auto result = sync(root, release.manifest("v1"), release.fetch(), [&](std::uint64_t done, std::uint64_t) { reported = done; });
    check(result.outcome == Outcome::Updated && result.firstInstall, "a first install must report Updated and firstInstall");
    for (const auto& [path, bytes] : release.files) check(read(below(root, path)) == bytes, "wrong bytes in " + path);
    const std::uint64_t expected = std::string("exe v1a v1modelcuda").size();   // the launcher was already there
    check(result.downloaded == expected && reported == expected, "downloaded " + std::to_string(result.downloaded));
    check(installedVersion(root) == "v1" && !pendingCheck(root), "v1 installed, nothing to go back to");
}

void second_sync_downloads_nothing() {
    Scratch scratch("again");
    const auto root = installed(scratch);
    const auto time = fs::last_write_time(root / "labeler" / "a.dll");
    const auto release = first();
    const auto result = sync(root, release.manifest("v1"), release.fetch(), {});
    check(result.outcome == Outcome::UpToDate && result.downloaded == 0, "a second sync must take nothing");
    check(fs::last_write_time(root / "labeler" / "a.dll") == time, "an unchanged file was written again");
}

void only_the_changed_file_comes() {
    Scratch scratch("changed");
    const auto root = installed(scratch);
    const auto exeTime = fs::last_write_time(root / "labeler" / "boccia_labeler.exe");
    auto release = first();
    release.files["labeler/a.dll"] = "a v2 longer";
    const auto result = sync(root, release.manifest("v2"), release.fetch(), {});
    check(result.outcome == Outcome::Updated && result.downloaded == std::string("a v2 longer").size(), "only a.dll must come");
    check(read(root / "labeler" / "a.dll") == "a v2 longer" && fs::last_write_time(root / "labeler" / "boccia_labeler.exe") == exeTime, "a.dll new, the rest as it was");
    check(installedVersion(root) == "v2" && pendingCheck(root), "v2 installed, its first start to be watched");
}

void dropped_file_goes_to_backup() {
    Scratch scratch("dropped");
    const auto root = installed(scratch);
    auto release = first();
    release.files.erase("models/boccia/m.onnx");
    check(sync(root, release.manifest("v2"), release.fetch(), {}).outcome == Outcome::Updated, "v2 without the model");
    check(!fs::exists(root / "models" / "boccia" / "m.onnx") && read(root / "update" / "backup" / "models" / "boccia" / "m.onnx") == "model",
          "a file the new list lacks goes to the backup");
}

void bad_hash_changes_nothing() {
    Scratch scratch("badhash");
    const auto root = installed(scratch);
    auto release = first();
    release.files["labeler/a.dll"] = "a v2";
    const auto manifest = release.manifest("v2");
    auto lying = release;
    lying.files["labeler/a.dll"] = "garbage";
    const auto result = sync(root, manifest, lying.fetch(), {});
    check(result.outcome == Outcome::Failed, "a wrong hash must fail");
    check(read(root / "labeler" / "a.dll") == "a v1" && installedVersion(root) == "v1" && !partsLeft(root), "nothing may change on a wrong hash");
}

void locked_target_rolls_back() {
    Scratch scratch("locked");
    const auto root = installed(scratch);
    auto release = first();
    release.files["labeler/a.dll"] = "a v2";
    release.files["labeler/boccia_labeler.exe"] = "exe v2";   // after a.dll in path order, and held open below
    const HANDLE held = CreateFileW((root / "labeler" / "boccia_labeler.exe").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(held != INVALID_HANDLE_VALUE, "could not hold the labeler open");
    const auto result = sync(root, release.manifest("v2"), release.fetch(), {});
    CloseHandle(held);
    check(result.outcome == Outcome::Failed, "a target that cannot move must fail");
    check(read(root / "labeler" / "a.dll") == "a v1" && read(root / "labeler" / "boccia_labeler.exe") == "exe v1" && installedVersion(root) == "v1",
          "the files moved before the failure must be back");
}

void data_and_logs_stay() {
    Scratch scratch("data");
    const auto root = installed(scratch);
    write(root / "labeler" / "data" / "x", "labels");
    write(root / "labeler" / "logs" / "y", "log");
    auto release = first();
    release.files["labeler/a.dll"] = "a v2";
    check(sync(root, release.manifest("v2"), release.fetch(), {}).outcome == Outcome::Updated, "v2");
    release.files.erase("cuda/c.dll");
    check(sync(root, release.manifest("v3"), release.fetch(), {}).outcome == Outcome::Updated, "v3");
    check(read(root / "labeler" / "data" / "x") == "labels" && read(root / "labeler" / "logs" / "y") == "log", "the worker's data and logs must stay");
    check(!fs::exists(root / "update" / "backup" / "labeler" / "data") && !fs::exists(root / "update" / "backup" / "labeler" / "logs"), "nor go to the backup");
}

void not_empty_folder_is_not_installable() {
    Scratch scratch("empty");
    const auto full = scratch.path / "full", quiet = scratch.path / "quiet";
    write(full / "BocciaLauncher.exe", "launcher");
    write(full / "note.txt", "mine");
    check(!installable(full, full / "BocciaLauncher.exe"), "a folder with other files must not be installed into");
    write(quiet / "BocciaLauncher.exe", "launcher");
    write(quiet / "desktop.ini", "[.ShellClassInfo]");
    SetFileAttributesW((quiet / "desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    check(installable(quiet, quiet / "BocciaLauncher.exe"), "hidden system files do not count");
    const auto root = installed(scratch);
    check(installable(root, root / "BocciaLauncher.exe"), "a launcher install stays installable");
}

void second_sync_on_the_same_root_is_busy() {
    Scratch scratch("busy");
    const auto root = installed(scratch);
    auto release = first();
    release.files["labeler/a.dll"] = "a v2";
    const auto other = release.manifest("v3");
    Outcome inner = Outcome::Updated;
    const auto plain = release.fetch();
    const Fetch nested = [&](const ReleaseFile& file, const fs::path& part, const std::function<void(std::uint64_t)>& received) {
        std::thread second([&] { inner = sync(root, other, plain, {}).outcome; });   // another thread: the mutex is not re-entered
        second.join();
        plain(file, part, received);
    };
    check(sync(root, release.manifest("v2"), nested, {}).outcome == Outcome::Updated, "the first sync must go on");
    check(inner == Outcome::Busy, "a second sync of the same folder must be Busy");
    check(installedVersion(root) == "v2", "the second sync must not have changed anything");
}

void unwritable_part_changes_nothing() {
    Scratch scratch("unwritable");
    const auto root = installed(scratch);
    auto release = first();
    release.files["labeler/a.dll"] = "a v2";
    write(root / "update" / (sha256Hex("a v2") + ".part") / "keep", "a folder where the part should go");
    const auto result = sync(root, release.manifest("v2"), release.fetch(), {});
    check(result.outcome == Outcome::Failed && read(root / "labeler" / "a.dll") == "a v1" && installedVersion(root) == "v1", "nothing may change when a part cannot be written");
}

void revert_restores_the_files_before() {
    Scratch scratch("revert");
    const auto root = installed(scratch);
    auto release = first();
    release.files["labeler/a.dll"] = "a v2";
    release.files["labeler/new.dll"] = "new";
    release.files.erase("cuda/c.dll");
    check(sync(root, release.manifest("v2"), release.fetch(), {}).outcome == Outcome::Updated && pendingCheck(root), "v2");
    check(revertLastUpdate(root) == "v2", "the revert must name the version it took back");
    check(read(root / "labeler" / "a.dll") == "a v1" && !fs::exists(root / "labeler" / "new.dll") && read(root / "cuda" / "c.dll") == "cuda", "v1's files must be back");
    check(installedVersion(root) == "v1" && !pendingCheck(root), "v1 again, nothing to watch");
    check(sync(root, release.manifest("v2"), release.fetch(), {}).outcome == Outcome::BadVersion && read(root / "labeler" / "a.dll") == "a v1", "a reverted version is not taken again");
    check(revertLastUpdate(root).empty(), "nothing more to revert");
}
}

int main() {
    try {
        first_install_places_every_file();
        second_sync_downloads_nothing();
        only_the_changed_file_comes();
        dropped_file_goes_to_backup();
        bad_hash_changes_nothing();
        locked_target_rolls_back();
        data_and_logs_stay();
        not_empty_folder_is_not_installable();
        second_sync_on_the_same_root_is_busy();
        unwritable_part_changes_nothing();
        revert_restores_the_files_before();
        std::cout << "PASS: sync, backup and revert: first install, nothing twice, the changed file only, dropped files, wrong hash, locked target, "
                     "data and logs, empty folder rule, busy, unwritable part, revert\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
