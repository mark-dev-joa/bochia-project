// BocciaLauncher.exe (design/hub/LAUNCHER.md). Beside boccia_labeler.exe, as in bin\release\labeler on the hub PC, it only
// starts the labeler. Anywhere else its folder is an install: the hub's current labeler, models and CUDA are taken into
// it, the labeler is started, and a new version that dies within 10 s is taken back.
//   --no-window                no window at all; the end is told by the exit code (tests)
//   --sync-only                take the release, do not start the labeler
//   --shortcut-folder <dir>    where the first install puts "Boccia Labeler.lnk" (the desktop by default)
//   --capture <png> --view settings|progress|unreachable    draws that window off screen into a PNG and ends
#include "labeler_settings.hpp"
#include "labeler_update.hpp"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

// Common Controls 6: buttons, fields and the bar drawn as Windows draws them now.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using namespace boccia::update;
using boccia::labeler::LabelerSettings;

namespace {
enum Exit : int { Done = 0, Broken = 2, NoHub = 3, WrongCode = 4, NotEmpty = 5, OtherLauncher = 6, NotInstalled = 7 };
const wchar_t* const TITLE = L"Boccia Labeler";
constexpr int ICON = 101;   // assets/icons/labeler_icon.rc
constexpr DWORD WATCH_MS = 10000;   // a new version that ends with an error within this is taken back

struct Options {
    bool noWindow = false, syncOnly = false;
    fs::path shortcutFolder, capture;
    std::wstring view;
};

std::string utf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
std::wstring trimmed(const std::wstring& text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    return first == std::wstring::npos ? L"" : text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}
fs::path modulePath() {
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return buffer;
}

// ---- windows: Win32 controls in the system's message font; the caller runs the message loop

HINSTANCE instance() { return GetModuleHandleW(nullptr); }
int scaled(int value) { static const UINT dpi = GetDpiForSystem(); return MulDiv(value, static_cast<int>(dpi), 96); }
HFONT uiFont() {
    static const HFONT font = [] {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof metrics;
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }();
    return font;
}

struct Window {
    HWND hwnd = nullptr;
    bool closable = true, done = false;
    int result = 0;   // IDOK: 시작 or 다시; IDCANCEL: closed
};

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_COMMAND:
        if (window && LOWORD(w) == IDOK) { window->result = IDOK; window->done = true; }
        return 0;
    case WM_CLOSE:
        if (window && window->closable) { window->result = IDCANCEL; window->done = true; }
        return 0;
    case WM_CTLCOLORSTATIC:   // labels on the window's own white
        SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    default:
        return DefWindowProcW(hwnd, message, w, l);
    }
}

void open(Window& window, int width, int height, bool closable, bool offScreen) {
    static const bool registered = [] {
        WNDCLASSEXW type{};
        type.cbSize = sizeof type;
        type.lpfnWndProc = windowProc;
        type.hInstance = instance();
        type.hIcon = LoadIconW(instance(), MAKEINTRESOURCEW(ICON));
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW, which is narrow without UNICODE
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        type.lpszClassName = L"BocciaLauncher";
        return RegisterClassExW(&type) != 0;
    }();
    (void)registered;
    window.closable = closable;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, scaled(width), scaled(height)};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    const int x = offScreen ? -20000 : work.left + (work.right - work.left - w) / 2, y = offScreen ? -20000 : work.top + (work.bottom - work.top - h) / 2;
    window.hwnd = CreateWindowExW(0, L"BocciaLauncher", TITLE, style, x, y, w, h, nullptr, nullptr, instance(), nullptr);
    SetWindowLongPtrW(window.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    if (!closable) EnableMenuItem(GetSystemMenu(window.hwnd, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
}

HWND control(const Window& window, const wchar_t* type, const std::wstring& text, DWORD style, int x, int y, int width, int height, int id) {
    const DWORD extended = std::wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0;
    const HWND made = CreateWindowExW(extended, type, text.c_str(), WS_CHILD | WS_VISIBLE | style, scaled(x), scaled(y), scaled(width), scaled(height),
                                      window.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance(), nullptr);
    SendMessageW(made, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    return made;
}

bool loop(Window& window) {   // until 시작 · 다시 (true) or the window is closed (false)
    window.done = false;
    MSG message;
    while (!window.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window.hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return window.result == IDOK;
}

std::wstring text(HWND edit) {
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    value.resize(static_cast<size_t>(GetWindowTextW(edit, value.data(), static_cast<int>(value.size()))));
    return trimmed(value);
}

// 이름 · 초대 코드 · 허브 주소 · 시작. The three fields of the labeler's own worker dialog, saved to the same file.
struct SettingsWindow {
    Window window;
    HWND name = nullptr, code = nullptr, address = nullptr;
    SettingsWindow(const LabelerSettings& settings, bool offScreen) {
        open(window, 360, 246, true, offScreen);
        control(window, L"STATIC", L"이름", 0, 20, 16, 320, 18, 0);
        name = control(window, L"EDIT", wide(settings.worker), ES_AUTOHSCROLL | WS_TABSTOP, 20, 36, 320, 26, 10);
        control(window, L"STATIC", L"초대 코드", 0, 20, 76, 320, 18, 0);
        code = control(window, L"EDIT", wide(settings.token), ES_AUTOHSCROLL | WS_TABSTOP, 20, 96, 320, 26, 11);
        control(window, L"STATIC", L"허브 주소", 0, 20, 136, 320, 18, 0);
        address = control(window, L"EDIT", wide(settings.hubAddress), ES_AUTOHSCROLL | WS_TABSTOP, 20, 156, 320, 26, 12);
        control(window, L"BUTTON", L"시작", BS_DEFPUSHBUTTON | WS_TABSTOP, 240, 200, 100, 30, IDOK);
        ShowWindow(window.hwnd, offScreen ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (!offScreen) SetFocus(text(name).empty() ? name : code);   // off screen it must not take the keyboard from the worker's window
    }
    ~SettingsWindow() { DestroyWindow(window.hwnd); }
    bool ask(LabelerSettings& settings) {   // false: closed
        while (loop(window)) {
            HWND empty = nullptr;
            for (const HWND field : {name, code, address}) if (!empty && text(field).empty()) empty = field;
            if (empty) { MessageBeep(MB_ICONWARNING); SetFocus(empty); continue; }
            settings.worker = utf8(text(name));
            settings.token = utf8(text(code));
            settings.hubAddress = utf8(text(address));
            return true;
        }
        return false;
    }
};

// 허브에 연결할 수 없습니다 · 다시
struct UnreachableWindow {
    Window window;
    explicit UnreachableWindow(bool offScreen) {
        open(window, 320, 110, true, offScreen);
        control(window, L"STATIC", L"허브에 연결할 수 없습니다", 0, 20, 22, 280, 20, 0);
        control(window, L"BUTTON", L"다시", BS_DEFPUSHBUTTON | WS_TABSTOP, 200, 62, 100, 30, IDOK);
        ShowWindow(window.hwnd, offScreen ? SW_SHOWNOACTIVATE : SW_SHOW);
    }
    ~UnreachableWindow() { DestroyWindow(window.hwnd); }
};

std::wstring sizes(std::uint64_t done, std::uint64_t total) {   // 받는 중 · 1.2 / 2.6 GB, in MB below 1 GiB
    const bool gb = total >= (std::uint64_t{1} << 30);
    const double unit = gb ? double(std::uint64_t{1} << 30) : double(std::uint64_t{1} << 20);
    wchar_t line[96];
    std::swprintf(line, std::size(line), L"받는 중 · %.1f / %.1f %s", double(done) / unit, double(total) / unit, gb ? L"GB" : L"MB");
    return line;
}

// The bar and the line; not closable, as nothing would undo half a download but the next start.
struct ProgressWindow {
    Window window;
    HWND line = nullptr, bar = nullptr;
    explicit ProgressWindow(bool offScreen) {
        open(window, 360, 90, false, offScreen);
        line = control(window, L"STATIC", L"", 0, 20, 16, 320, 20, 0);
        bar = control(window, PROGRESS_CLASSW, L"", 0, 20, 46, 320, 18, 0);
        SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
        ShowWindow(window.hwnd, offScreen ? SW_SHOWNOACTIVATE : SW_SHOW);
    }
    ~ProgressWindow() { DestroyWindow(window.hwnd); }
    void show(std::uint64_t done, std::uint64_t total) {
        SetWindowTextW(line, sizes(done, total).c_str());
        SendMessageW(bar, PBM_SETPOS, total ? static_cast<WPARAM>(done * 1000 / total) : 0, 0);
    }
};

// ---- the install

// The labeler of this install, already running from these files: nothing of it may be swapped now.
bool running(const fs::path& program) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, program.filename().c_str()) != 0) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
            path.resize(size);
            std::error_code error;
            found = fs::equivalent(path, program, error);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

// A line in the labeler's log, as the labeler writes it, so the hub's activity shows it.
void logLine(const fs::path& root, const std::string& line) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    char day[16], stamp[40];
    std::snprintf(day, sizeof day, "%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);
    std::snprintf(stamp, sizeof stamp, "%s %02d:%02d:%02d.%03d", day, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    const auto folder = root / "labeler" / "logs";
    std::error_code error;
    fs::create_directories(folder, error);
    const HANDLE file = CreateFileW((folder / ("labeler-" + std::string(day) + ".log")).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::string bytes = std::string(stamp) + "  " + line + "\n";
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void makeShortcut(const fs::path& folder, const fs::path& launcher) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    link->SetPath(launcher.c_str());
    link->SetWorkingDirectory(launcher.parent_path().c_str());
    link->SetIconLocation(launcher.c_str(), 0);
    link->SetDescription(TITLE);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        file->Save((folder / L"Boccia Labeler.lnk").c_str(), TRUE);
        file->Release();
    }
    link->Release();
}
fs::path desktop() {
    PWSTR path = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

// Starts the program in its folder: the process, or nothing.
bool start(const fs::path& program, const std::wstring& arguments, PROCESS_INFORMATION& process) {
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    std::wstring command = L"\"" + program.wstring() + L"\"" + (arguments.empty() ? L"" : L" " + arguments);
    return CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, program.parent_path().c_str(), &startup, &process) != 0;
}
int waitFor(PROCESS_INFORMATION& process) {
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

// The labeler of the install. The first start of a new version is watched: an error end within 10 s takes it back.
int startLabeler(const fs::path& root, const Options& options) {
    const auto labeler = root / "labeler" / "boccia_labeler.exe";
    PROCESS_INFORMATION process{};
    const bool watch = pendingCheck(root);
    if (!start(labeler, L"", process)) {
        if (watch && !revertLastUpdate(root).empty() && start(labeler, L"", process)) return waitFor(process);
        if (!options.noWindow) MessageBoxW(nullptr, L"라벨러를 실행하지 못했습니다", TITLE, MB_OK | MB_ICONERROR);
        return Broken;
    }
    if (watch) {
        DWORD code = 0;
        if (WaitForSingleObject(process.hProcess, WATCH_MS) == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && code != 0) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            const std::string version = revertLastUpdate(root);
            if (!version.empty()) logLine(root, "업데이트 되돌림 · " + version);
            if (!start(labeler, L"", process)) return Broken;
        } else {
            confirmStart(root);   // it lived 10 s, or the worker closed it
        }
    }
    return waitFor(process);
}

// The sync, with the progress window once something is coming; the window's loop runs while the sync works.
SyncResult syncShowing(const fs::path& root, const Manifest& manifest, const HubAddress& hub, bool noWindow) {
    if (noWindow) return sync(root, manifest, hubFetch(hub), {});
    std::atomic<std::uint64_t> done{0}, total{0};
    std::atomic<bool> finished{false};
    SyncResult result;
    std::thread worker([&] {
        result = sync(root, manifest, hubFetch(hub), [&](std::uint64_t now, std::uint64_t all) { done = now; total = all; });
        finished = true;
    });
    std::unique_ptr<ProgressWindow> window;
    while (!finished) {
        if (!window && done > 0) window = std::make_unique<ProgressWindow>(false);
        if (window) window->show(done, total);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    worker.join();
    return result;
}

int install(const fs::path& root, const fs::path& launcher, const Options& options) {
    if (temporaryOrRemovable(root) || !installable(root, launcher)) {   // a zip's temp folder or a USB stick is no place for labels
        if (!options.noWindow) MessageBoxW(nullptr, L"빈 폴더에 넣어 주세요", TITLE, MB_OK | MB_ICONINFORMATION);
        return NotEmpty;
    }
    auto settings = boccia::labeler::loadLabelerSettings();
    const auto labeler = root / "labeler" / "boccia_labeler.exe";
    const bool installed = fs::is_regular_file(labeler) && !installedVersion(root).empty();
    const auto ask = [&]() {   // the settings window; false when it was closed or cannot be shown
        if (options.noWindow) return false;
        SettingsWindow window(settings, false);
        if (!window.ask(settings)) return false;
        boccia::labeler::saveLabelerSettings(settings);
        return true;
    };
    // Without the hub's answer an installed labeler still starts, off the hub (final review: a replaced invite code).
    const bool asked = !(settings.worker.empty() || settings.token.empty()) || ask();
    if (!asked && !installed) return options.noWindow ? WrongCode : Done;
    if (asked && !(installed && running(labeler))) {   // the labeler of this install is not open: its files may change
        for (;;) {
            Manifest manifest;
            const HubAddress hub{settings.hubAddress, settings.token};
            const auto answer = fetchManifest(hub, "labeler", manifest);
            if (answer == Answer::Unauthorized) {
                if (ask()) continue;
                if (installed) break;
                return options.noWindow ? WrongCode : Done;
            }
            if (answer == Answer::Ok) {
                const auto result = syncShowing(root, manifest, hub, options.noWindow);
                if (result.outcome == Outcome::Busy) return OtherLauncher;
                if ((result.outcome == Outcome::Updated || result.outcome == Outcome::UpToDate) && result.firstInstall)
                    makeShortcut(options.shortcutFolder.empty() ? desktop() : options.shortcutFolder, launcher);
                if (result.outcome != Outcome::Failed || installed) break;
            } else if (installed) {
                break;   // no hub now: the version already here
            }
            if (options.noWindow) return answer == Answer::Ok ? NotInstalled : NoHub;
            UnreachableWindow window(false);
            if (!loop(window.window)) return Done;
        }
    }
    if (options.syncOnly) return Done;
    if (!fs::is_regular_file(labeler)) return NotInstalled;
    return startLabeler(root, options);
}

// ---- --capture: one of the windows with sample values, drawn off screen and saved as a PNG

bool savePng(HBITMAP bitmap, const fs::path& file) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* image = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &image)) &&
              SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->WriteSource(image, nullptr)) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    for (IUnknown* object : std::initializer_list<IUnknown*>{frame, encoder, stream, image, factory}) if (object) object->Release();
    return ok;
}

int capture(const Options& options) {
    std::unique_ptr<SettingsWindow> settings;
    std::unique_ptr<ProgressWindow> progress;
    std::unique_ptr<UnreachableWindow> unreachable;
    HWND hwnd = nullptr;
    if (options.view == L"settings") {
        LabelerSettings sample;
        sample.worker = "홍길동";
        sample.token = "k7Qp2mX9vR4tL8wZ";
        settings = std::make_unique<SettingsWindow>(sample, true);
        hwnd = settings->window.hwnd;
    } else if (options.view == L"progress") {
        progress = std::make_unique<ProgressWindow>(true);
        progress->show(std::uint64_t{1288490188}, std::uint64_t{2791728742});   // 1.2 / 2.6 GB
        hwnd = progress->window.hwnd;
    } else if (options.view == L"unreachable") {
        unreachable = std::make_unique<UnreachableWindow>(true);
        hwnd = unreachable->window.hwnd;
    } else {
        return Broken;
    }
    for (int i = 0; i < 20; ++i) {   // let it paint
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(10);
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);   // what changed after it was shown
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right - rect.left, rect.bottom - rect.top);
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    PrintWindow(hwnd, memory, PW_RENDERFULLCONTENT);
    SelectObject(memory, previous);
    const bool saved = savePng(bitmap, options.capture);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return saved ? Done : Broken;
}

Options parse(int count, wchar_t** arguments) {
    Options options;
    for (int i = 1; i < count; ++i) {
        const std::wstring argument = arguments[i];
        const auto next = [&]() { return i + 1 < count ? std::wstring(arguments[++i]) : std::wstring(); };
        if (argument == L"--no-window") options.noWindow = true;
        else if (argument == L"--sync-only") options.syncOnly = true;
        else if (argument == L"--shortcut-folder") options.shortcutFolder = next();
        else if (argument == L"--capture") options.capture = next();
        else if (argument == L"--view") options.view = next();
    }
    return options;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int show) {
    const fs::path launcher = modulePath();
    if (launcher.empty()) return Broken;
    const fs::path folder = launcher.parent_path();
    int count = 0;
    wchar_t** list = CommandLineToArgvW(GetCommandLineW(), &count);
    const Options options = parse(count, list);
    LocalFree(list);
    const bool capturing = !options.capture.empty() && (options.view == L"settings" || options.view == L"progress" || options.view == L"unreachable");
    // Beside the labeler (bin\release\labeler on the hub PC): start it with these arguments and wait, as always. The
    // launcher's own captures are drawn wherever it is (2026-10-21: beside the labeler they started it, window and all).
    const auto beside = folder / "boccia_labeler.exe";
    if (!capturing && fs::is_regular_file(beside)) {
        SHELLEXECUTEINFOW launch{};
        launch.cbSize = sizeof launch;
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        launch.lpVerb = L"open";
        launch.lpFile = beside.c_str();
        launch.lpParameters = arguments;
        launch.lpDirectory = folder.c_str();
        launch.nShow = show;
        if (!ShellExecuteExW(&launch)) {
            MessageBoxW(nullptr, L"라벨러를 실행하지 못했습니다", TITLE, MB_OK | MB_ICONERROR);
            return NoHub;
        }
        if (!launch.hProcess) return Done;
        WaitForSingleObject(launch.hProcess, INFINITE);
        DWORD result = 0;
        GetExitCodeProcess(launch.hProcess, &result);
        CloseHandle(launch.hProcess);
        return static_cast<int>(result);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int code = capturing ? capture(options) : options.capture.empty() ? install(folder, launcher, options) : Broken;
    CoUninitialize();
    return code;
}
