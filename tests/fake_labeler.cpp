// Stands in for boccia_labeler.exe in the launcher tests (design/hub/PLAN-LAUNCHER.md Task 5). A window program, as the
// labeler is, so no console opens: it adds a line to ran.txt beside it and ends after "<exit code> <milliseconds>" from
// fake-exit.txt (0 at once without one).
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring module(32768, L'\0');
    module.resize(GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size())));
    const std::filesystem::path folder = std::filesystem::path(module).parent_path();
    std::ofstream(folder / "ran.txt", std::ios::app) << "ran\n";
    int code = 0, milliseconds = 0;
    std::ifstream(folder / "fake-exit.txt") >> code >> milliseconds;
    Sleep(static_cast<DWORD>(milliseconds));
    return code;
}
