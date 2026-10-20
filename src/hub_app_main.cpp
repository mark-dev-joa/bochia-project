// Boccia Hub.exe (design/hub/PLAN-2.md Task 5): starts the hub (python hub/hub.py) when it is off and shows its screen
// (hub/ui) in a window of its own (WebView2). Closing the window leaves the hub running for the labelers.
//   Boccia Hub.exe                     the window
//   Boccia Hub.exe --background        the hub only (the login shortcut)
//   Boccia Hub.exe --check             a hidden check: hub, WebView2, the screen's title; never shows a window
//   --port <n> (8790)  --storage <dir> (D:\Boccia Hub)
#include "hub_launch.hpp"
#include "hub_client.hpp"
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
namespace fs = std::filesystem;
using namespace boccia::hubapp;

namespace {
constexpr wchar_t WINDOW_CLASS[] = L"BocciaHubWindow";
constexpr UINT_PTR CHECK_TIMER = 1;

struct App {
    HWND window = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    fs::path exe;
    int port = 8790;
    bool check = false, startedHub = false;
    int result = 0;
    UINT dpi = 96;
} app;

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size);
    return result;
}
// A hidden check prints into the pipe it was given, else onto the console it was started from.
void say(const std::string& text) {
    if (!GetStdHandle(STD_OUTPUT_HANDLE) && AttachConsole(ATTACH_PARENT_PROCESS)) { FILE* out{}; freopen_s(&out, "CONOUT$", "w", stdout); }
    std::cout << text << "\n" << std::flush;
}
fs::path settingsFolder() { wchar_t folder[32768]{}; GetEnvironmentVariableW(L"LOCALAPPDATA", folder, 32768); return fs::path(folder) / "Boccia"; }
Json readSettings() {
    try { std::ifstream input(settingsFolder() / "hub-settings.json", std::ios::binary); if (input) { Json stored = Json::parse(input); if (stored.is_object()) return stored; } }
    catch (const std::exception&) {}
    return Json::object();
}
void writeSettings(const Json& settings) {
    std::error_code error;
    fs::create_directories(settingsFolder(), error);
    std::ofstream(settingsFolder() / "hub-settings.json", std::ios::binary) << settings.dump(1);
}
void tellAutostart() { if (app.view) app.view->PostWebMessageAsJson(wide(Json{{"type", "autostart"}, {"on", autostartOn()}}.dump()).c_str()); }

void finishCheck(int result, const std::string& text) {
    if (!app.check || !app.window) return;
    app.result = result;
    say(text);
    if (app.startedHub) {   // a hub this check started goes away with it
        boccia::hub::Session session;
        session.request("127.0.0.1:" + std::to_string(app.port), "POST", "/api/shutdown", "{}", {"Content-Type: application/json"});
    }
    DestroyWindow(app.window);
}
// No WebView2 here: the default browser shows the same screen.
void fallback(const std::wstring& url) {
    if (app.check) { finishCheck(1, "FAIL: WebView2 is not available"); return; }
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    DestroyWindow(app.window);
}
void onMessage(const std::wstring& json) {
    Json message;
    try { message = Json::parse(narrow(json)); } catch (const std::exception&) { return; }
    if (!message.is_object()) return;
    const std::string type = message.value("type", "");
    if (type == "autostart?") tellAutostart();
    else if (type == "autostart" && !app.check) {
        const bool on = message.value("on", false);
        setAutostart(on, app.exe);
        Json settings = readSettings();
        settings["autostart"] = on;
        writeSettings(settings);
        tellAutostart();
    }
    else if (type == "close") DestroyWindow(app.window);
}

void startView(const fs::path& userData) {
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(app.port) + L"/";
    const HRESULT created = CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([url](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) { fallback(url); return S_OK; }
            environment->CreateCoreWebView2Controller(app.window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [url](HRESULT made, ICoreWebView2Controller* controller) -> HRESULT {
                    if (FAILED(made) || !controller) { fallback(url); return S_OK; }
                    app.controller = controller;
                    app.controller->get_CoreWebView2(&app.view);
                    RECT bounds{};
                    GetClientRect(app.window, &bounds);
                    app.controller->put_Bounds(bounds);
                    ComPtr<ICoreWebView2Settings> settings;
                    if (SUCCEEDED(app.view->get_Settings(&settings))) { settings->put_IsStatusBarEnabled(FALSE); settings->put_AreDevToolsEnabled(FALSE); }
                    EventRegistrationToken token{};
                    app.view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR json = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) { onMessage(json); CoTaskMemFree(json); }
                            return S_OK;
                        }).Get(), &token);
                    if (app.check) app.view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            BOOL ok = FALSE;
                            args->get_IsSuccess(&ok);
                            if (!ok) { finishCheck(1, "FAIL: the screen did not load"); return S_OK; }
                            sender->ExecuteScript(L"document.title", Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                                [](HRESULT done, LPCWSTR json) -> HRESULT {
                                    const std::string title = json ? narrow(json) : std::string();
                                    if (SUCCEEDED(done) && title == "\"Boccia Hub\"") finishCheck(0, "PASS: hub ready, WebView2 loaded the screen");
                                    else finishCheck(1, "FAIL: document.title " + title);
                                    return S_OK;
                                }).Get());
                            return S_OK;
                        }).Get(), &token);
                    app.view->Navigate(url.c_str());
                    return S_OK;
                }).Get());
            return S_OK;
        }).Get());
    if (FAILED(created)) fallback(url);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (app.controller) { RECT bounds{}; GetClientRect(window, &bounds); app.controller->put_Bounds(bounds); }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize = {MulDiv(1100, int(app.dpi), 96), MulDiv(700, int(app.dpi), 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        app.dpi = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TIMER:
        if (wParam == CHECK_TIMER) finishCheck(1, "FAIL: the screen did not load in 30 s");
        return 0;
    case WM_DESTROY:
        if (app.controller) app.controller->Close();
        app.window = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool background = false;
    fs::path storage = L"D:\\Boccia Hub";
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--background") background = true;
        else if (argument == L"--check") app.check = true;
        else if (argument == L"--port" && i + 1 < argc) app.port = _wtoi(argv[++i]);
        else if (argument == L"--storage" && i + 1 < argc) storage = argv[++i];
    }
    LocalFree(argv);
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    app.exe = module;
    const fs::path program = app.exe.parent_path();
    const fs::path python = findPython(program), script = hubScript(program, fs::path(BOCCIA_HUB_ROOT));
    if (background) return ensureHub(python, script, app.port, storage).empty() ? 0 : 1;

    HANDLE single = nullptr;
    if (!app.check) {   // one window: a second start brings the first one forward
        single = CreateMutexW(nullptr, TRUE, L"Local\\BocciaHubWindow");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND other = FindWindowW(WINDOW_CLASS, nullptr)) { ShowWindow(other, IsIconic(other) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(other); }
            if (single) CloseHandle(single);
            return 0;
        }
    }
    const std::string problem = ensureHub(python, script, app.port, storage, &app.startedHub);
    if (!problem.empty()) {
        if (app.check) say("FAIL: " + problem);
        else MessageBoxW(nullptr, wide(problem).c_str(), L"Boccia Hub", MB_OK | MB_ICONWARNING);
        if (single) CloseHandle(single);
        return 1;
    }
    if (!app.check) {   // design decision 3: the hub starts at login; the first run sets it up, 설정 turns it off
        Json settings = readSettings();
        if (!settings.contains("autostart")) { setAutostart(true, app.exe); settings["autostart"] = true; writeSettings(settings); }
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof windowClass;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = WINDOW_CLASS;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW (the ANSI macro without UNICODE)
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&windowClass);
    app.dpi = GetDpiForSystem();
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(MulDiv(1440, int(app.dpi), 96), int(work.right - work.left)), height = std::min(MulDiv(900, int(app.dpi), 96), int(work.bottom - work.top));
    app.window = CreateWindowExW(0, WINDOW_CLASS, L"Boccia Hub", WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - width) / 2,
                                 work.top + (work.bottom - work.top - height) / 2, width, height, nullptr, nullptr, instance, nullptr);
    if (!app.window) { if (app.check) say("FAIL: no window"); return 1; }
    if (app.check) SetTimer(app.window, CHECK_TIMER, 30000, nullptr);
    else { ShowWindow(app.window, show); UpdateWindow(app.window); }
    const fs::path userData = app.check ? fs::temp_directory_path() / ("boccia-hub-check-" + std::to_string(GetCurrentProcessId())) : settingsFolder() / "hub-webview";
    startView(userData);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    app.view.Reset();
    app.controller.Reset();
    CoUninitialize();
    if (single) CloseHandle(single);
    return app.result;
}
