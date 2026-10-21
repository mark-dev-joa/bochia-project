#pragma once
// A hub of its own for the tests: python hub/hub.py serve on 127.0.0.1, its port read from the first output line, killed
// by stop() (design/hub/PLAN-1.md Task 4, PLAN-LAUNCHER.md Task 4). Win32 only, so the static-CRT launcher tests use it too.
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

// python --version succeeds: python is installed (not the Store stub).
inline bool pythonAvailable() {
    STARTUPINFOW startup{}; startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    std::wstring command = L"python --version";
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    WaitForSingleObject(process.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return code == 0;
}

struct HubProcess {
    PROCESS_INFORMATION process{};
    HANDLE output = nullptr;
    std::thread drain;
    std::mutex lock;
    std::string printed;   // under lock: what the hub wrote, for a failure message
    int port = 0;
    bool running = false;
    explicit HubProcess(const std::filesystem::path& storage, int wantedPort = 0) {
        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        HANDLE input = nullptr;
        if (!CreatePipe(&output, &input, &inherit, 0)) return;
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup; startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = input; startup.hStdError = input;
        std::wstring command = L"python \"" + std::filesystem::path(BOCCIA_TEST_ROOT).wstring() + L"\\hub\\hub.py\" serve --port " + std::to_wstring(wantedPort)
            + L" --storage \"" + storage.wstring() + L"\" --bind 127.0.0.1 --print-port";
        running = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0;
        CloseHandle(input);
        if (!running) return;
        std::promise<std::string> first;
        auto firstLine = first.get_future();
        drain = std::thread([this, first = std::move(first)]() mutable {   // reads everything the hub prints so it never blocks on a full pipe
            std::string line; bool reported = false; char byte; DWORD read = 0;
            while (ReadFile(output, &byte, 1, &read, nullptr) && read) {
                { std::lock_guard guard(lock); if (printed.size() < 4000) printed += byte; }
                if (reported) continue;
                if (byte == '\n') { first.set_value(line); reported = true; } else line += byte;
            }
            if (!reported) first.set_value("");
        });
        if (firstLine.wait_for(std::chrono::seconds(15)) == std::future_status::ready) {
            const std::string line = firstLine.get();
            if (line.rfind("port ", 0) == 0) port = std::stoi(line.substr(5));
        }
        if (!port) {   // the destructor does not run for a throwing constructor: end the process and the reader first
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            stop();
            if (drain.joinable()) drain.join();
            std::lock_guard guard(lock);
            throw std::runtime_error("hub did not start:\n" + printed);
        }
    }
    ~HubProcess() { stop(); if (drain.joinable()) drain.join(); if (output) CloseHandle(output); }
    HubProcess(const HubProcess&) = delete;
    HubProcess& operator=(const HubProcess&) = delete;
    void stop() {
        if (!running) return;
        TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); running = false;
    }
    std::string address() const { return "127.0.0.1:" + std::to_string(port); }
};
