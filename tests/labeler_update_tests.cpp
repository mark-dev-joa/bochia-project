// The launcher's update engine (design/hub/PLAN-LAUNCHER.md Tasks 3-5): sync, backup and revert on a scratch folder.
#include "labeler_update.hpp"
#include "hub_process.hpp"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <memory>
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

// 2026-10-21 end-to-end run: two licenses of the real release hold the same bytes; both shared one part file, and the
// second fetch removed the first piece, so the install failed at the swap.
void same_content_twice_installs() {
    Scratch scratch("twice");
    const auto root = scratch.path / "root";
    write(root / "BocciaLauncher.exe", "launcher");
    auto release = first();
    release.files["labeler/licenses/x.txt"] = "same license";
    release.files["labeler/licenses/y/x.txt"] = "same license";
    const auto result = sync(root, release.manifest("v1"), release.fetch(), {});
    check(result.outcome == Outcome::Updated, "the same content at two paths must install: " + result.error);
    check(read(root / "labeler" / "licenses" / "x.txt") == "same license" && read(root / "labeler" / "licenses" / "y" / "x.txt") == "same license",
          "both paths must hold it");
    check(result.downloaded == std::string("exe v1a v1modelcudasame license").size(), "the same content comes once");
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

// ---- over a real hub (Task 4): python hub/hub.py with a release registered by `boccia-hub release`

std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
int run(std::wstring command) {
    STARTUPINFOW startup{}; startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return -1;
    WaitForSingleObject(process.hProcess, 300000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

// A hub storage with one worker's invite code, its releases registered from trees beside it.
struct HubSetup {
    const Scratch& scratch;
    fs::path storage;
    std::string token = "launcher-test-token";
    std::unique_ptr<HubProcess> hub;
    explicit HubSetup(const Scratch& owner) : scratch(owner), storage(owner.path / "hub") {
        write(storage / "tokens.json", "{\"" + sha256Hex(token) + "\": \"점검\"}");
    }
    void start() { hub = std::make_unique<HubProcess>(storage); }
    void publish(const Release& release, const std::string& version) {
        const auto tree = scratch.path / ("tree-" + version);
        for (const auto& [path, bytes] : release.files) write(path == "BocciaLauncher.exe" ? tree / "labeler" / "BocciaLauncher.exe" : below(tree, path), bytes);
        const std::wstring command = L"python \"" + fs::path(BOCCIA_TEST_ROOT).wstring() + L"\\hub\\cli.py\" release labeler \"" + tree.wstring() +
                                     L"\" --storage \"" + storage.wstring() + L"\" --version " + wide(version);
        check(run(command) == 0, "boccia-hub release failed for " + version);
    }
    HubAddress address() const { return {hub->address(), token}; }
};

Manifest listed(const HubSetup& setup) {
    Manifest manifest;
    check(fetchManifest(setup.address(), "labeler", manifest) == Answer::Ok, "the hub must list a release");
    return manifest;
}

void installs_from_the_hub() {
    Scratch scratch("hub");
    HubSetup setup(scratch);
    setup.publish(first(), "v1");
    setup.start();
    const auto root = scratch.path / "root";
    write(root / "BocciaLauncher.exe", "launcher");
    const auto manifest = listed(setup);
    check(manifest.version == "v1" && manifest.files.size() == 5, "v1's list from the hub");
    const auto result = sync(root, manifest, hubFetch(setup.address()), {});
    check(result.outcome == Outcome::Updated, "install from the hub: " + result.error);
    for (const auto& [path, bytes] : first().files) check(read(below(root, path)) == bytes, "wrong bytes from the hub in " + path);
    check(installedVersion(root) == "v1", "v1 from the hub");
}

void wrong_token_is_unauthorized() {
    Scratch scratch("token");
    HubSetup setup(scratch);
    setup.publish(first(), "v1");
    setup.start();
    Manifest manifest;
    check(fetchManifest({setup.hub->address(), "nope"}, "labeler", manifest) == Answer::Unauthorized, "a wrong invite code must be Unauthorized");
}

void no_release_is_no_release() {
    Scratch scratch("none");
    HubSetup setup(scratch);
    setup.start();
    Manifest manifest;
    check(fetchManifest(setup.address(), "labeler", manifest) == Answer::NoRelease, "a hub without a release must say NoRelease");
}

void hub_off_is_unreachable() {
    Scratch scratch("off");
    HubSetup setup(scratch);
    setup.start();
    const auto address = setup.address();
    setup.hub->stop();
    const auto started = std::chrono::steady_clock::now();
    Manifest manifest;
    check(fetchManifest(address, "labeler", manifest) == Answer::Unreachable, "a stopped hub must be Unreachable");
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(6), "Unreachable must come within 6 s");
}

void interrupted_download_changes_nothing() {
    Scratch scratch("cut");
    HubSetup setup(scratch);
    setup.publish(first(), "v1");
    setup.start();
    const auto root = scratch.path / "root";
    write(root / "BocciaLauncher.exe", "launcher");
    check(sync(root, listed(setup), hubFetch(setup.address()), {}).outcome == Outcome::Updated, "v1 from the hub");
    auto bigger = first();
    std::string big(64u << 20, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 7 + (i >> 13));
    bigger.files["cuda/big.dll"] = big;
    setup.publish(bigger, "v2");
    bool stopped = false;
    const auto result = sync(root, listed(setup), hubFetch(setup.address()), [&](std::uint64_t done, std::uint64_t) {
        if (!stopped && done >= (8u << 20)) { setup.hub->stop(); stopped = true; }   // the hub goes away mid-file
    });
    check(stopped && result.outcome == Outcome::Failed, "a download cut off must fail");
    check(!fs::exists(root / "cuda" / "big.dll") && installedVersion(root) == "v1" && !partsLeft(root), "a cut download must change nothing");
    setup.start();
    const auto again = sync(root, listed(setup), hubFetch(setup.address()), {});
    check(again.outcome == Outcome::Updated && fileSha256(root / "cuda" / "big.dll") == sha256Hex(big), "the next start takes it whole: " + again.error);
}

void korean_root_installs() {
    Scratch scratch("korean");
    HubSetup setup(scratch);
    setup.publish(first(), "v1");
    setup.start();
    const auto root = scratch.path / fs::path(u8"보치아 라벨러");
    write(root / "BocciaLauncher.exe", "launcher");
    const auto result = sync(root, listed(setup), hubFetch(setup.address()), {});
    check(result.outcome == Outcome::Updated && read(root / "labeler" / "a.dll") == "a v1", "a Korean folder name must install: " + result.error);
}

// ---- the launcher itself (Task 5): the built BocciaLauncher.exe run with --no-window in a scratch folder; its settings come
// from a scratch LOCALAPPDATA and its shortcut goes to a scratch folder, so nothing of this PC's own is touched

// The release the launcher takes: the built launcher at the top, fake_labeler.exe as the labeler.
Release launcherRelease(const std::string& exit) {
    return {{{"BocciaLauncher.exe", read(BOCCIA_LAUNCHER_EXE)}, {"labeler/boccia_labeler.exe", read(BOCCIA_FAKE_LABELER_EXE)},
             {"labeler/fake-exit.txt", exit}, {"labeler/a.dll", "a v1"}, {"models/boccia/m.onnx", "model"}, {"cuda/c.dll", "cuda"}}};
}

std::wstring environment(const wchar_t* name) {
    std::wstring value(32767, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size())));
    return value;
}

// Runs a program with LOCALAPPDATA pointing at `appdata`; its exit code, -1 when it did not start or did not end in time.
int runWith(const fs::path& appdata, std::wstring command, const fs::path& folder) {
    const std::wstring saved = environment(L"LOCALAPPDATA");
    SetEnvironmentVariableW(L"LOCALAPPDATA", appdata.c_str());
    STARTUPINFOW startup{}; startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &process) != 0;
    SetEnvironmentVariableW(L"LOCALAPPDATA", saved.c_str());
    if (!started) return -1;
    const bool ended = WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0;
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    if (!ended) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return ended ? static_cast<int>(code) : -1;
}

struct LauncherRun {
    fs::path root, appdata, desk;
    LauncherRun(const Scratch& scratch, const std::string& folder)
        : root(scratch.path / folder), appdata(scratch.path / "appdata"), desk(scratch.path / "desk") {
        fs::create_directories(root);
        fs::create_directories(desk);
        fs::copy_file(BOCCIA_LAUNCHER_EXE, root / "BocciaLauncher.exe", fs::copy_options::overwrite_existing);
    }
    void settings(const std::string& address, const std::string& token) const {
        write(appdata / "Boccia" / "labeler-settings.json", "{\"hub\": {\"address\": \"" + address + "\", \"worker\": \"점검\", \"token\": \"" + token + "\"}}");
    }
    int run(const std::wstring& extra = L"") const {
        return runWith(appdata, L"\"" + (root / "BocciaLauncher.exe").wstring() + L"\" --no-window --shortcut-folder \"" + desk.wstring() + L"\" " + extra, root);
    }
    std::string log() const {
        SYSTEMTIME now;
        GetLocalTime(&now);
        char name[40];
        std::snprintf(name, sizeof name, "labeler-%04d-%02d-%02d.log", now.wYear, now.wMonth, now.wDay);
        return read(root / "labeler" / "logs" / name);
    }
    int ran() const {   // how many times the fake labeler started
        const auto text = read(root / "labeler" / "ran.txt");
        return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    }
};

struct LauncherHub {   // a hub with v1 of the launcher release, and a launcher folder whose settings point at it
    Scratch scratch;
    HubSetup setup;
    LauncherRun launcher;
    LauncherHub(const std::string& name, const std::string& exit) : scratch(name), setup(scratch), launcher(scratch, "install") {
        setup.publish(launcherRelease(exit), "v1");
        setup.start();
        launcher.settings(setup.hub->address(), setup.token);
    }
};

fs::path shortcutTarget(const fs::path& link) {
    IShellLinkW* shell = nullptr;
    IPersistFile* file = nullptr;
    wchar_t target[MAX_PATH]{};
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) {
        if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Load(link.c_str(), STGM_READ)))
            shell->GetPath(target, MAX_PATH, nullptr, 0);
        if (file) file->Release();
        shell->Release();
    }
    return target;
}

void launcher_installs_and_starts() {
    LauncherHub hub("launch", "0 0");
    check(hub.launcher.run() == 0, "the launcher must install and start");
    check(read(hub.launcher.root / "labeler" / "a.dll") == "a v1" && read(hub.launcher.root / "cuda" / "c.dll") == "cuda", "the release must be in place");
    check(hub.launcher.ran() == 1, "the labeler must have started once");
}

void launcher_dev_mode_only_starts() {
    Scratch scratch("dev");
    const auto folder = scratch.path / "labeler";
    fs::create_directories(folder);
    fs::copy_file(BOCCIA_LAUNCHER_EXE, folder / "BocciaLauncher.exe");
    fs::copy_file(BOCCIA_FAKE_LABELER_EXE, folder / "boccia_labeler.exe");
    check(runWith(scratch.path / "appdata", L"\"" + (folder / "BocciaLauncher.exe").wstring() + L"\"", folder) == 0, "beside the labeler it only starts it");
    check(fs::exists(folder / "ran.txt") && !fs::exists(folder / "update"), "no settings, no hub, nothing installed");
}

// 2026-10-21: --capture beside the labeler started the labeler with the launcher's capture options (a labeler window
// opened); the launcher's own windows are drawn wherever it is.
void capture_beside_the_labeler_does_not_start_it() {
    Scratch scratch("capture");
    const auto folder = scratch.path / "labeler";
    fs::create_directories(folder);
    fs::copy_file(BOCCIA_LAUNCHER_EXE, folder / "BocciaLauncher.exe");
    fs::copy_file(BOCCIA_FAKE_LABELER_EXE, folder / "boccia_labeler.exe");
    const auto png = scratch.path / "unreachable.png";
    check(runWith(scratch.path / "appdata", L"\"" + (folder / "BocciaLauncher.exe").wstring() + L"\" --capture \"" + png.wstring() + L"\" --view unreachable", folder) == 0,
          "the capture must end with 0");
    check(fs::exists(png) && fs::file_size(png) > 0 && !fs::exists(folder / "ran.txt"), "the launcher's window drawn, the labeler not started");
}

void launcher_refuses_a_full_folder() {
    Scratch scratch("full");
    LauncherRun launcher(scratch, "full");
    write(launcher.root / "note.txt", "mine");
    check(launcher.run() == 5 && !fs::exists(launcher.root / "labeler"), "a folder with other files must be refused (5)");
}

void wrong_token_exits_4() {
    LauncherHub hub("code", "0 0");
    hub.launcher.settings(hub.setup.hub->address(), "nope");
    check(hub.launcher.run() == 4 && !fs::exists(hub.launcher.root / "labeler"), "a wrong invite code must end with 4");
}

void crash_within_ten_seconds_reverts() {
    LauncherHub hub("crash", "0 0");
    check(hub.launcher.run() == 0, "v1");
    auto v2 = launcherRelease("3 500");
    v2.files["labeler/a.dll"] = "a v2";
    hub.setup.publish(v2, "v2");
    check(hub.launcher.run() == 0, "after the revert v1 starts again");
    check(hub.launcher.log().find("업데이트 되돌림 · v2") != std::string::npos, "the revert must be in the labeler's log");
    check(installedVersion(hub.launcher.root) == "v1" && read(hub.launcher.root / "labeler" / "a.dll") == "a v1", "v1's files must be back");
    check(hub.launcher.ran() == 3, "v1, v2 dying, v1 again");
    check(hub.launcher.run() == 0 && installedVersion(hub.launcher.root) == "v1", "the next start keeps v1");
}

void quick_close_is_kept() {
    LauncherHub hub("quick", "0 0");
    check(hub.launcher.run() == 0, "v1");
    auto v2 = launcherRelease("0 200");
    v2.files["labeler/a.dll"] = "a v2";
    hub.setup.publish(v2, "v2");
    check(hub.launcher.run() == 0 && installedVersion(hub.launcher.root) == "v2" && !pendingCheck(hub.launcher.root), "a quick close with 0 keeps v2");
}

void first_install_makes_the_shortcut() {
    LauncherHub hub("shortcut", "0 0");
    check(hub.launcher.run() == 0, "v1");
    std::error_code error;
    check(fs::equivalent(shortcutTarget(hub.launcher.desk / "Boccia Labeler.lnk"), hub.launcher.root / "BocciaLauncher.exe", error), "the shortcut must point at the launcher");
}

void launcher_updates_itself() {
    LauncherHub hub("itself", "0 0");
    check(hub.launcher.run() == 0, "v1");
    const auto before = read(BOCCIA_LAUNCHER_EXE);
    auto v2 = launcherRelease("0 0");
    v2.files["BocciaLauncher.exe"] = before + std::string(1, '\0');
    hub.setup.publish(v2, "v2");
    check(hub.launcher.run() == 0, "v2");
    check(read(hub.launcher.root / "BocciaLauncher.exe") == v2.files["BocciaLauncher.exe"], "the launcher must be v2's");
    check(read(hub.launcher.root / "update" / "backup" / "BocciaLauncher.exe") == before, "the launcher before goes to the backup");
}

void running_labeler_is_not_updated() {
    LauncherHub hub("running", "0 5000");
    check(hub.launcher.run(L"--sync-only") == 0, "v1, not started");
    STARTUPINFOW startup{}; startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + (hub.launcher.root / "labeler" / "boccia_labeler.exe").wstring() + L"\"";
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, (hub.launcher.root / "labeler").c_str(), &startup, &process) != 0,
          "could not start the labeler");
    auto v2 = launcherRelease("0 5000");
    v2.files["labeler/a.dll"] = "a v2";
    hub.setup.publish(v2, "v2");
    const int code = hub.launcher.run(L"--sync-only");
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    check(code == 0 && installedVersion(hub.launcher.root) == "v1" && read(hub.launcher.root / "labeler" / "a.dll") == "a v1", "a running labeler is not updated");
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
        same_content_twice_installs();
        not_empty_folder_is_not_installable();
        second_sync_on_the_same_root_is_busy();
        unwritable_part_changes_nothing();
        revert_restores_the_files_before();
        if (!pythonAvailable()) {
            std::cout << "SKIP: the tests over a hub need python\n";
        } else {
            installs_from_the_hub();
            wrong_token_is_unauthorized();
            no_release_is_no_release();
            hub_off_is_unreachable();
            interrupted_download_changes_nothing();
            korean_root_installs();
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            launcher_installs_and_starts();
            launcher_dev_mode_only_starts();
            capture_beside_the_labeler_does_not_start_it();
            launcher_refuses_a_full_folder();
            wrong_token_exits_4();
            crash_within_ten_seconds_reverts();
            quick_close_is_kept();
            first_install_makes_the_shortcut();
            launcher_updates_itself();
            running_labeler_is_not_updated();
        }
        std::cout << "PASS: sync, backup and revert: first install, nothing twice, the changed file only, dropped files, wrong hash, locked target, "
                     "data and logs, the same content twice, empty folder rule, busy, unwritable part, revert; over a hub: install, wrong code, no release, hub off, cut download, "
                     "Korean folder; the launcher: install, beside the labeler, capture beside it, full folder, wrong code, revert, quick close, shortcut, "
                     "itself, running labeler\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
