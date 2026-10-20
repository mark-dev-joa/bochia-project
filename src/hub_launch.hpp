#pragma once
// What Boccia Hub.exe does before its window (design/hub/PLAN-2.md Task 5): find Python and hub.py, check the port,
// start the hub when it is off (detached, so closing the window leaves it running) and keep the login shortcut.
#include <filesystem>
#include <string>

namespace boccia::hubapp {
namespace fs = std::filesystem;
fs::path pythonOnPath(const std::wstring& path);   // pythonw.exe in a PATH string, skipping "\WindowsApps\" (the Store alias); empty when none
fs::path findPython(const fs::path& programFolder);   // <program>\python\pythonw.exe, then PEP 514 (HKCU, HKLM; 3.10 and newer, newest first), then %PATH%
fs::path hubScript(const fs::path& programFolder, const fs::path& repository);   // <program>\hub\hub.py, else <repository>\hub\hub.py; empty when neither
enum class Ping { Hub, Other, None };
Ping ping(int port);   // GET http://127.0.0.1:<port>/ping within 2 s: Hub for {"hub":"boccia"}, Other for any other answer, None for no answer
// "" when a hub answers on the port (started here when needed, waiting up to 10 s); otherwise the words to show:
// "Python 3.10 이상 필요", "<port> 포트를 다른 프로그램이 씀" or "허브를 켜지 못함".
std::string ensureHub(const fs::path& python, const fs::path& script, int port, const fs::path& storage, bool* started = nullptr);
fs::path startupFolder();   // BOCCIA_STARTUP_DIR when set (tests), else the user's Startup folder
bool autostartOn();         // <startup>\Boccia Hub.lnk exists
void setAutostart(bool on, const fs::path& exe);   // the shortcut runs exe --background (the hub only, no window)
}
