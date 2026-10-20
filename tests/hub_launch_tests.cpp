// What Boccia Hub.exe does before its window (design/hub/PLAN-2.md Task 5): find Python and hub.py, tell a hub from
// another program on the port, start one hub and only one, and the login shortcut. Uses scratch folders and free ports.
#include "hub_launch.hpp"
#include "hub_client.hpp"
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#pragma comment(lib, "ws2_32.lib")

using namespace boccia::hubapp;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool waitUntil(const std::function<bool()>& ready, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) { if (ready()) return true; std::this_thread::sleep_for(100ms); }
    return ready();
}
int freePort() {
    WSADATA data; WSAStartup(MAKEWORD(2, 2), &data);
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
    bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof address);
    int length = sizeof address; getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    closesocket(probe);
    return ntohs(address.sin_port);
}
void touch(const fs::path& file) { fs::create_directories(file.parent_path()); std::ofstream(file, std::ios::binary) << ""; }
// A process of our own, ended by the destructor (python -m http.server for "another program on the port").
struct Child {
    PROCESS_INFORMATION process{};
    explicit Child(std::wstring command) {
        STARTUPINFOW startup{}; startup.cb = sizeof startup;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) throw std::runtime_error("could not start a child");
    }
    ~Child() { TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hThread); CloseHandle(process.hProcess); }
};
std::wstring shortcutArguments(const fs::path& link) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* shell = nullptr; IPersistFile* file = nullptr; wchar_t arguments[1024]{};
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) {
        if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Load(link.c_str(), STGM_READ))) shell->GetArguments(arguments, 1024);
        if (file) file->Release();
        shell->Release();
    }
    return arguments;
}

int main() { try {
    const fs::path temp = fs::temp_directory_path() / ("hub-launch-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(temp); fs::create_directories(temp);

    // find_python_finds_this_pc
    const fs::path python = findPython(temp);
    check(!python.empty() && python.filename() == "pythonw.exe" && python.wstring().find(L"WindowsApps") == std::wstring::npos, "this PC's pythonw.exe must be found");
    // python_on_path_skips_store_alias
    touch(temp / "WindowsApps" / "pythonw.exe"); touch(temp / "bin" / "pythonw.exe");
    check(pythonOnPath((temp / "WindowsApps").wstring()).empty(), "the Microsoft Store alias is not Python");
    check(pythonOnPath((temp / "WindowsApps").wstring() + L";" + (temp / "bin").wstring()) == temp / "bin" / "pythonw.exe", "a real pythonw.exe on PATH must be found");
    // hub_script_found
    const fs::path repository = BOCCIA_TEST_ROOT;
    check(hubScript(temp, repository) == repository / "hub" / "hub.py", "a development build finds the repository's hub.py");
    touch(temp / "hub" / "hub.py");
    check(hubScript(temp, repository) == temp / "hub" / "hub.py", "a release folder's own hub.py comes first");
    fs::remove_all(temp / "hub");
    std::cout << "python " << python.string() << "\n";

    // ping_none_on_free_port, ping_tells_other_from_hub
    const int otherPort = freePort();
    check(ping(otherPort) == Ping::None, "nothing listens on a free port");
    {
        const fs::path console = python.parent_path() / "python.exe";
        Child other(L"\"" + console.wstring() + L"\" -m http.server " + std::to_wstring(otherPort) + L" --bind 127.0.0.1 --directory \"" + temp.wstring() + L"\"");
        check(waitUntil([&] { return ping(otherPort) != Ping::None; }, 10), "the other program did not start");
        check(ping(otherPort) == Ping::Other, "another program must not count as the hub");
        check(ensureHub(python, hubScript(temp, repository), otherPort, temp / "store-other") == std::to_string(otherPort) + " 포트를 다른 프로그램이 씀", "a taken port must be said");
    }
    check(ensureHub({}, hubScript(temp, repository), freePort(), temp / "store-none") == "Python 3.10 이상 필요", "no Python must be said");

    // ensure_hub_twice_starts_one
    const int port = freePort();
    bool started = false;
    check(ensureHub(python, hubScript(temp, repository), port, temp / "store", &started).empty() && started && ping(port) == Ping::Hub, "the hub did not start");
    started = true;
    check(ensureHub(python, hubScript(temp, repository), port, temp / "store", &started).empty() && !started, "a running hub must not be started twice");
    boccia::hub::Session session;
    check(session.request("127.0.0.1:" + std::to_string(port), "POST", "/api/shutdown", "{}", {"Content-Type: application/json"}).status == 200, "shutdown refused");
    check(waitUntil([&] { return ping(port) == Ping::None; }, 5), "the hub did not stop");
    check(fs::is_regular_file(temp / "store" / "hub.log"), "the hub must keep its log in its storage");

    // autostart_toggle
    SetEnvironmentVariableW(L"BOCCIA_STARTUP_DIR", (temp / "startup").c_str());
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    check(startupFolder() == temp / "startup" && !autostartOn(), "the test's startup folder starts empty");
    setAutostart(true, self);
    check(autostartOn() && fs::is_regular_file(temp / "startup" / "Boccia Hub.lnk") && shortcutArguments(temp / "startup" / "Boccia Hub.lnk") == L"--background",
          "the login shortcut starts the hub only");
    setAutostart(false, self);
    check(!autostartOn() && !fs::exists(temp / "startup" / "Boccia Hub.lnk"), "turning it off removes the shortcut");

    std::error_code ignored; fs::remove_all(temp, ignored);
    std::cout << "PASS: python found, store alias skipped, hub script found, ping, one hub for two starts, login shortcut\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; } }
