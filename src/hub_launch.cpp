#include "hub_launch.hpp"
#include "hub_client.hpp"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <thread>
#include <tuple>
#include <vector>

namespace boccia::hubapp {
namespace {
std::wstring lower(std::wstring text) { std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); }); return text; }
std::wstring readString(HKEY key, const wchar_t* name) {
    wchar_t buffer[32768]{};
    DWORD size = sizeof buffer, type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return buffer;
}
// PEP 514: Software\Python\PythonCore\<3.x>\InstallPath of this user, then of the machine.
fs::path registeredPython() {
    std::vector<std::tuple<int, int, fs::path>> found;
    const std::pair<HKEY, REGSAM> roots[] = {{HKEY_CURRENT_USER, 0}, {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}};
    for (const auto& [root, view] : roots) {
        HKEY core = nullptr;
        if (RegOpenKeyExW(root, L"Software\\Python\\PythonCore", 0, KEY_READ | view, &core) != ERROR_SUCCESS) continue;
        wchar_t tag[256];
        for (DWORD index = 0;; ++index) {
            DWORD length = 256;
            if (RegEnumKeyExW(core, index, tag, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            int major = 0, minor = 0;
            if (swscanf_s(tag, L"%d.%d", &major, &minor) != 2 || major != 3 || minor < 10) continue;
            HKEY install = nullptr;
            if (RegOpenKeyExW(core, (std::wstring(tag) + L"\\InstallPath").c_str(), 0, KEY_READ | view, &install) != ERROR_SUCCESS) continue;
            fs::path windowed = readString(install, L"WindowedExecutablePath");
            if (windowed.empty()) { const std::wstring folder = readString(install, nullptr); if (!folder.empty()) windowed = fs::path(folder) / "pythonw.exe"; }
            RegCloseKey(install);
            std::error_code error;
            if (!windowed.empty() && fs::is_regular_file(windowed, error)) found.emplace_back(major, minor, windowed);
        }
        RegCloseKey(core);
    }
    if (found.empty()) return {};
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return std::tie(std::get<0>(a), std::get<1>(a)) > std::tie(std::get<0>(b), std::get<1>(b)); });
    return std::get<2>(found.front());
}
}

fs::path pythonOnPath(const std::wstring& path) {
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = std::min(path.find(L';', start), path.size());
        std::wstring folder = path.substr(start, end - start);
        start = end + 1;
        folder.erase(std::remove(folder.begin(), folder.end(), L'"'), folder.end());
        if (folder.empty() || lower(folder).find(L"\\windowsapps") != std::wstring::npos) continue;   // the Store's alias opens the Store
        std::error_code error;
        const fs::path candidate = fs::path(folder) / "pythonw.exe";
        if (fs::is_regular_file(candidate, error)) return candidate;
    }
    return {};
}

fs::path findPython(const fs::path& programFolder) {
    std::error_code error;
    if (const fs::path bundled = programFolder / "python" / "pythonw.exe"; fs::is_regular_file(bundled, error)) return bundled;
    if (const fs::path registered = registeredPython(); !registered.empty()) return registered;
    wchar_t path[32767]{};
    return GetEnvironmentVariableW(L"PATH", path, 32767) ? pythonOnPath(path) : fs::path();
}

fs::path hubScript(const fs::path& programFolder, const fs::path& repository) {
    std::error_code error;
    for (const fs::path& script : {programFolder / "hub" / "hub.py", repository / "hub" / "hub.py"})
        if (!script.parent_path().parent_path().empty() && fs::is_regular_file(script, error)) return script;
    return {};
}

Ping ping(int port) {
    hub::Session session;
    const auto reply = session.request("127.0.0.1:" + std::to_string(port), "GET", "/ping", "", {}, 2000);
    if (reply.status == 0) return Ping::None;
    try { if (reply.status == 200 && hub::Json::parse(reply.body).value("hub", "") == "boccia") return Ping::Hub; } catch (const std::exception&) {}
    return Ping::Other;
}

std::string ensureHub(const fs::path& python, const fs::path& script, int port, const fs::path& storage, bool* started) {
    if (started) *started = false;
    const Ping now = ping(port);
    if (now == Ping::Hub) return {};
    if (now == Ping::Other) return std::to_string(port) + " 포트를 다른 프로그램이 씀";
    if (python.empty()) return "Python 3.10 이상 필요";
    if (script.empty()) return "허브를 켜지 못함";
    std::error_code error;
    fs::create_directories(storage, error);
    std::wstring folder = storage.wstring();
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();   // "D:\x\" would escape the closing quote
    std::wstring command = L"\"" + python.wstring() + L"\" \"" + script.wstring() + L"\" serve --port " + std::to_wstring(port) + L" --storage \"" + folder + L"\" --bind auto";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    // Its own process group, outside this program's job: closing the window or ending this program leaves the hub running.
    const DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    const std::wstring directory = script.parent_path().wstring();
    BOOL launched = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, directory.c_str(), &startup, &process);
    if (!launched) launched = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr, directory.c_str(), &startup, &process);
    if (!launched) return "허브를 켜지 못함";
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ping(port) == Ping::Hub) { if (started) *started = true; return {}; }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return "허브를 켜지 못함";
}

fs::path startupFolder() {
    wchar_t chosen[32768]{};
    if (GetEnvironmentVariableW(L"BOCCIA_STARTUP_DIR", chosen, 32768)) return chosen;
    PWSTR folder = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &folder))) result = folder;
    CoTaskMemFree(folder);
    return result;
}

bool autostartOn() { std::error_code error; return fs::is_regular_file(startupFolder() / "Boccia Hub.lnk", error); }

void setAutostart(bool on, const fs::path& exe) {
    const fs::path link = startupFolder() / "Boccia Hub.lnk";
    std::error_code error;
    if (!on) { fs::remove(link, error); return; }
    fs::create_directories(link.parent_path(), error);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* shell = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) {
        shell->SetPath(exe.c_str());
        shell->SetArguments(L"--background");
        shell->SetWorkingDirectory(exe.parent_path().c_str());
        shell->SetIconLocation(exe.c_str(), 0);
        shell->SetDescription(L"Boccia Hub");
        IPersistFile* file = nullptr;
        if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)))) { file->Save(link.c_str(), TRUE); file->Release(); }
        shell->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
}
}
