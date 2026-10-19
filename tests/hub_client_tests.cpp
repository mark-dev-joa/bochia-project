// The shared hub client against a real hub (python hub/hub.py on 127.0.0.1): knock, register, status, events with the
// log backfill, commands and the outbox (design/hub/PLAN-1.md Task 4). Prints SKIP when python is not installed.
#include "hub_client.hpp"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace boccia::hub;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool waitUntil(const std::function<bool()>& ready, double seconds) {
    const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < deadline) { if (ready()) return true; std::this_thread::sleep_for(50ms); }
    return ready();
}
double secondsSince(Clock::time_point from) { return std::chrono::duration<double>(Clock::now() - from).count(); }

// A hub of its own: python hub/hub.py serve on 127.0.0.1, its port read from the first output line, killed by stop().
struct HubProcess {
    PROCESS_INFORMATION process{};
    HANDLE output = nullptr;
    std::thread drain;
    int port = 0;
    bool running = false;
    explicit HubProcess(const fs::path& storage, int wantedPort = 0) {
        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        HANDLE input = nullptr;
        if (!CreatePipe(&output, &input, &inherit, 0)) return;
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup; startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = input; startup.hStdError = input;
        std::wstring command = L"python \"" + fs::path(BOCCIA_TEST_ROOT).wstring() + L"\\hub\\hub.py\" serve --port " + std::to_wstring(wantedPort)
            + L" --storage \"" + storage.wstring() + L"\" --bind 127.0.0.1 --print-port";
        running = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0;
        CloseHandle(input);
        if (!running) return;
        std::promise<std::string> first;
        auto firstLine = first.get_future();
        drain = std::thread([this, first = std::move(first)]() mutable {   // reads everything the hub prints so it never blocks on a full pipe
            std::string line; bool reported = false; char byte; DWORD read = 0;
            while (ReadFile(output, &byte, 1, &read, nullptr) && read) {
                if (reported) continue;
                if (byte == '\n') { first.set_value(line); reported = true; } else line += byte;
            }
            if (!reported) first.set_value("");
        });
        if (firstLine.wait_for(15s) == std::future_status::ready) {
            const std::string line = firstLine.get();
            if (line.rfind("port ", 0) == 0) port = std::stoi(line.substr(5));
        }
    }
    ~HubProcess() { stop(); if (drain.joinable()) drain.join(); if (output) CloseHandle(output); }
    void stop() {
        if (!running) return;
        TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); running = false;
    }
    std::string address() const { return "127.0.0.1:" + std::to_string(port); }
};

Session control;   // the test's own control-API calls
Json api(const std::string& address, const char* method, const std::string& path, const Json& body = nullptr) {
    const auto reply = control.request(address, method, path, body.is_null() ? "" : body.dump(),
        body.is_null() ? std::vector<std::string>{} : std::vector<std::string>{"Content-Type: application/json; charset=utf-8"});
    if (reply.status == 0) throw std::runtime_error("hub unreachable: " + reply.error);
    if (reply.status / 100 != 2) throw std::runtime_error("HTTP " + std::to_string(reply.status) + " " + method + " " + path + ": " + reply.body);
    return reply.body.empty() ? Json() : Json::parse(reply.body);
}
Json worker(const std::string& address, const std::string& id) {
    for (const auto& entry : api(address, "GET", "/api/workers")) if (entry["id"] == id) return entry;
    return Json();
}

int main() { try {
    const fs::path temp = fs::temp_directory_path() / ("hub-client-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(temp); fs::create_directories(temp);
    const fs::path outbox = temp / "outbox", logPath = temp / "labeler-2026-10-19.log";
    const std::string pc = hostName(), id = "테스트@" + pc, idPath = "/api/workers/" + percentEncode(id);
    check(!pc.empty() && !accountName().empty(), "host and account names must be known");
    const auto settings = [&](const std::string& address, const std::string& token) {
        Settings made; made.address = address; made.token = token; made.worker = "테스트"; made.pc = pc; made.account = accountName(); made.app = "test"; made.version = "0"; return made; };
    const auto logFile = [&] { return logPath; };
    const auto registration = [] { return Registration{Json{{"gpu", "none"}}, Json{{"frames", 0}}}; };
    { std::ofstream(logPath, std::ios::binary) << ""; }

    {   // wrong_token_stops_retrying, then setSettings connects; connects_registers_and_sends_status_and_events
        HubProcess hub(temp / "hub1");
        if (!hub.port) { std::cout << "SKIP: python\n"; return 0; }
        Client client(settings(hub.address(), "wrong-token"), outbox, logFile, registration);
        check(waitUntil([&] { return client.state() == Client::State::Unauthorized; }, 3), "a wrong token must end Unauthorized");
        const std::string token = api(hub.address(), "POST", "/api/invites", Json{{"worker", "테스트"}})["token"];
        std::this_thread::sleep_for(12s);   // past the next knock slot: a refused client must not try again by itself
        check(api(hub.address(), "GET", "/api/workers").empty(), "an unauthorized client must not knock again");
        client.setSettings(settings(hub.address(), token));
        check(waitUntil([&] { return client.state() == Client::State::Connected; }, 3), "did not connect after new settings");
        client.setStatus(Json{{"page", "review"}});
        client.event("확인 · 촬영 01 · 프레임 0003");
        check(waitUntil([&] { const auto found = worker(hub.address(), id); return !found.is_null() && found["status"].value("page", "") == "review"; }, 3), "status did not arrive");
        check(waitUntil([&] { for (const auto& event : api(hub.address(), "GET", idPath + "/events")) if (event["text"] == "확인 · 촬영 01 · 프레임 0003") return true; return false; }, 3), "event did not arrive");
        const auto found = worker(hub.address(), id);
        check(found["connected"] == true && found["app"] == "test" && found["device"]["gpu"] == "none" && found["account"] == accountName(), "registration fields wrong");
        std::cout << "connected after new settings, status and event delivered\n";
    }
    {   // backfills_log_lines_since_last_event_without_duplicates
        std::ofstream log(logPath, std::ios::binary);
        for (int i = 1; i <= 2500; ++i) { char stamp[32]; std::snprintf(stamp, sizeof stamp, "2030-01-01 00:00:%02d.%03d", i / 1000, i % 1000); log << stamp << "  줄 " << i << "\n"; }
        log.close();
        HubProcess hub(temp / "hub2");
        check(hub.port != 0, "second hub did not start");
        const std::string token = api(hub.address(), "POST", "/api/invites", Json{{"worker", "테스트"}})["token"];
        {
            Client client(settings(hub.address(), token), outbox, logFile, registration);
            check(waitUntil([&] { return client.state() == Client::State::Connected; }, 3), "did not connect to the second hub");
            check(waitUntil([&] { return api(hub.address(), "GET", idPath + "/events?limit=5000").size() == 2000; }, 15), "the last 2,000 log lines must arrive");
        }
        const auto events = api(hub.address(), "GET", idPath + "/events?limit=5000");
        check(events.front()["text"] == "줄 501" && events.back()["text"] == "줄 2500" && events.front()["time"] == "2030-01-01 00:00:00.501", "backfill must be the file's last 2,000 lines in order");
        Client client(settings(hub.address(), token), outbox, logFile, registration);
        check(waitUntil([&] { return client.state() == Client::State::Connected; }, 3), "did not reconnect");
        std::this_thread::sleep_for(2s);
        check(api(hub.address(), "GET", idPath + "/events?limit=5000").size() == 2000, "a reconnect must not send the lines again");
        std::cout << "backfill sent the last 2,000 lines once\n";

        // receives_a_command_and_reports_the_result
        const std::string commandId = api(hub.address(), "POST", idPath + "/commands", Json{{"name", "notify"}, {"value", "쉬세요"}})["id"];
        std::vector<Command> commands;
        check(waitUntil([&] { auto taken = client.takeCommands(); commands.insert(commands.end(), taken.begin(), taken.end()); return !commands.empty(); }, 3), "command did not arrive");
        check(commands.size() == 1 && commands[0].id == commandId && commands[0].name == "notify" && commands[0].value == "쉬세요", "command fields wrong");
        client.finish(commands[0], true, "띄움");
        check(waitUntil([&] { for (const auto& entry : api(hub.address(), "GET", idPath + "/commands")) if (entry["id"] == commandId && entry["state"] == "done" && entry["result"] == "띄움") return true; return false; }, 3), "result did not arrive");
        std::cout << "command received and finished\n";

        // offline_calls_return_at_once_and_outbox_uploads_later
        const int port = hub.port;
        hub.stop();
        check(waitUntil([&] { return client.state() == Client::State::Offline; }, 5), "a dead hub must show Offline");
        auto started = Clock::now();
        client.setStatus(Json{{"page", "tools"}}); client.event("오프라인 줄"); client.upload("log", logPath);
        check(secondsSince(started) < .05, "offline calls must return at once");
        HubProcess again(temp / "hub2", port);   // the same port and storage: the client finds it by itself
        std::optional<HubProcess> fresh;
        if (!again.port) { std::cout << "(port busy, reconnecting through new settings)\n"; fresh.emplace(temp / "hub2"); client.setSettings(settings(fresh->address(), token)); }
        const std::string address = again.port ? again.address() : fresh->address();
        check(waitUntil([&] { return client.state() == Client::State::Connected; }, 15), "did not reconnect to the restarted hub");
        check(waitUntil([&] { for (const auto& file : api(address, "GET", idPath + "/files/log")) if (file["name"] == "labeler-2026-10-19.log") return true; return false; }, 5), "outbox file did not upload");
        check(waitUntil([&] { const auto found = worker(address, id); return !found.is_null() && found["status"].value("page", "") == "tools"; }, 5), "status set while offline must arrive after reconnect");
        started = Clock::now();
        { Client gone(settings(address, token), outbox, logFile, registration); check(waitUntil([&] { return gone.state() == Client::State::Connected; }, 3), "did not connect"); started = Clock::now(); }
        check(secondsSince(started) < 2.5, "destroying a connected client must take under 2 s");
        std::cout << "offline calls returned at once, outbox uploaded after the hub came back\n";
    }
    fs::remove_all(temp);
    std::cout << "PASS: knock, register, status, events with log backfill, commands, outbox and shutdown\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; } }
