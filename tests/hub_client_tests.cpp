// The shared hub client against a real hub (python hub/hub.py on 127.0.0.1): knock, register, status, events with the
// log backfill, commands and the outbox (design/hub/PLAN-1.md Task 4, final review fixes). Prints SKIP when python is
// not installed.
#include "hub_client.hpp"
#include "hub_process.hpp"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
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
void writeFile(const fs::path& path, const std::string& text) { fs::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary) << text; }
std::string stamp(int i, const char* day = "2030-01-01") { char text[40]; std::snprintf(text, sizeof text, "%s 00:00:%02d.%03d", day, i / 1000, i % 1000); return text; }

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
size_t countWith(const Json& events, const std::string& piece) {
    size_t count = 0;
    for (const auto& event : events) if (event["text"].get<std::string>().find(piece) != std::string::npos) ++count;
    return count;
}
const std::string REPLACEMENT = "\xEF\xBF\xBD";   // U+FFFD, what an invalid byte becomes

int main() { try {
    std::cout.setf(std::ios::unitbuf);   // every line reaches the file even when the process dies
    std::cout << "GPU " << gpuName() << "\n";   // the registration's device name (may be empty on a PC without a GPU)
    if (!pythonAvailable()) { std::cout << "SKIP: python\n"; return 0; }
    const fs::path temp = fs::temp_directory_path() / ("hub-client-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(temp); fs::create_directories(temp);
    const fs::path outbox = temp / "outbox", logPath = temp / "labeler-2026-10-19.log", logPath2 = temp / "labeler-shared.log";
    const std::string pc = hostName(), id = "테스트@" + pc, idPath = "/api/workers/" + percentEncode(id);
    check(!pc.empty() && !accountName().empty(), "host and account names must be known");
    const auto settings = [&](const std::string& address, const std::string& token) {
        Settings made; made.address = address; made.token = token; made.worker = "테스트"; made.pc = pc; made.account = accountName(); made.app = "test"; made.version = "0"; return made; };
    const auto logFile = [&] { return logPath; };
    const auto registration = [] { return Registration{Json{{"gpu", "none"}}, Json{{"frames", 0}}}; };
    writeFile(logPath, "");

    {   // wrong_token_stops_retrying: even once the code becomes valid behind its back, only new settings get the client in
        HubProcess hub(temp / "hub1");
        Client client(settings(hub.address(), "wrong-token"), outbox, logFile, registration);
        check(waitUntil([&] { return client.state() == Client::State::Unauthorized; }, 3), "a wrong token must end Unauthorized");
        const int port = hub.port;
        hub.stop();
        writeFile(temp / "hub1/tokens.json", R"({"5645a758e6a8f12b6a2715cc22565a9f68d1ed73d98d33a1c5adf99277cd0b73": "테스트"})");   // sha256("wrong-token")
        HubProcess again(temp / "hub1", port);
        std::this_thread::sleep_for(12s);   // past the next knock slot: a client that tried again would be registered by now
        check(api(again.address(), "GET", "/api/workers").empty(), "an unauthorized client must not knock again");
        client.setSettings(settings(again.address(), "wrong-token"));
        check(waitUntil([&] { return client.state() == Client::State::Connected; }, 3), "did not connect after new settings");
        client.setStatus(Json{{"page", "review"}});
        client.event("확인 · 촬영 01 · 프레임 0003");
        check(waitUntil([&] { const auto found = worker(again.address(), id); return !found.is_null() && found["status"].value("page", "") == "review"; }, 3), "status did not arrive");
        check(waitUntil([&] { return countWith(api(again.address(), "GET", idPath + "/events"), "확인 · 촬영 01 · 프레임 0003") == 1; }, 3), "event did not arrive");
        const auto found = worker(again.address(), id);
        check(found["connected"] == true && found["app"] == "test" && found["device"]["gpu"] == "none" && found["account"] == accountName(), "registration fields wrong");
        std::cout << "wrong token stays out until new settings; status and event delivered\n";
    }
    {   // backfills_log_lines_since_last_event_without_duplicates, with a partial overlap
        std::ofstream log(logPath, std::ios::binary);
        for (int i = 1; i <= 2500; ++i) log << stamp(i) << "  줄 " << i << "\n";
        log.close();
        HubProcess hub(temp / "hub2");
        const std::string token = api(hub.address(), "POST", "/api/invites", Json{{"worker", "테스트"}})["token"];
        {   // lines 1..1000 reached the hub earlier (a labeler that was connected then): the backfill adds 1001..2500, once each
            const std::vector<std::string> headers{"Authorization: Bearer " + token, "Content-Type: application/json; charset=utf-8"};
            check(control.request(hub.address(), "POST", "/register", Json{{"app", "test"}, {"worker", "테스트"}, {"pc", pc}}.dump(), headers).status == 200, "pre-registration failed");
            Json early = Json::array();
            for (int i = 1; i <= 1000; ++i) early.push_back(Json{{"time", stamp(i)}, {"text", "줄 " + std::to_string(i)}});
            check(control.request(hub.address(), "POST", "/events", Json{{"events", early}}.dump(), headers).status == 200, "pre-sent events failed");
        }
        {
            Client client(settings(hub.address(), token), outbox, logFile, registration);
            check(waitUntil([&] { return client.state() == Client::State::Connected; }, 3), "did not connect to the second hub");
            check(waitUntil([&] { return api(hub.address(), "GET", idPath + "/events?limit=5000").size() == 2500; }, 15), "lines 1001..2500 must follow the 1,000 the hub had");
        }
        const auto events = api(hub.address(), "GET", idPath + "/events?limit=5000");
        std::set<std::string> texts;
        for (const auto& event : events) texts.insert(event["text"].get<std::string>());
        check(texts.size() == 2500 && events.front()["text"] == "줄 1" && events.back()["text"] == "줄 2500" && events[1000]["time"] == stamp(1001), "every line once, in order, the backfill after the hub's last");
        std::optional<Client> client;
        client.emplace(settings(hub.address(), token), outbox, logFile, registration);
        check(waitUntil([&] { return client->state() == Client::State::Connected; }, 3), "did not reconnect");
        std::this_thread::sleep_for(2s);
        check(api(hub.address(), "GET", idPath + "/events?limit=5000").size() == 2500, "a reconnect must not send the lines again");
        std::cout << "backfill sent 1,500 lines once\n";

        // A line that is not valid UTF-8 (a CP949 path in an error message) must not take the client down: live or backfilled
        client->event("오류 · \xBA\xB8 잘못된 바이트");
        std::this_thread::sleep_for(2s);
        check(client->state() == Client::State::Connected, "an event with invalid UTF-8 must leave the client connected");
        check(waitUntil([&] { return countWith(api(hub.address(), "GET", idPath + "/events?limit=5000"), REPLACEMENT) == 1; }, 3), "the invalid bytes must arrive as U+FFFD");
        { std::ofstream append(logPath, std::ios::binary | std::ios::app); append << stamp(3000) << "  오류 · \xBA\xB8 로그 줄\n"; }
        client.emplace(settings(hub.address(), token), outbox, logFile, registration);   // the old client is destroyed first
        check(waitUntil([&] { return client->state() == Client::State::Connected; }, 3), "did not reconnect after the bad log line");
        std::this_thread::sleep_for(2s);
        check(client->state() == Client::State::Connected, "a backfill with invalid UTF-8 must leave the client connected");
        check(waitUntil([&] { return countWith(api(hub.address(), "GET", idPath + "/events?limit=5000"), REPLACEMENT) == 2; }, 3), "the bad log line must arrive as U+FFFD");
        std::cout << "invalid UTF-8 replaced, client alive\n";

        // receives_a_command_and_reports_the_result
        const std::string commandId = api(hub.address(), "POST", idPath + "/commands", Json{{"name", "notify"}, {"value", "쉬세요"}})["id"];
        std::vector<Command> commands;
        check(waitUntil([&] { auto taken = client->takeCommands(); commands.insert(commands.end(), taken.begin(), taken.end()); return !commands.empty(); }, 3), "command did not arrive");
        check(commands.size() == 1 && commands[0].id == commandId && commands[0].name == "notify" && commands[0].value == "쉬세요", "command fields wrong");
        client->finish(commands[0], true, "띄움");
        check(waitUntil([&] { for (const auto& entry : api(hub.address(), "GET", idPath + "/commands")) if (entry["id"] == commandId && entry["state"] == "done" && entry["result"] == "띄움") return true; return false; }, 3), "result did not arrive");
        std::cout << "command received and finished\n";

        // offline_calls_return_at_once_and_outbox_uploads_later
        const int port = hub.port;
        hub.stop();
        check(waitUntil([&] { return client->state() == Client::State::Offline; }, 5), "a dead hub must show Offline");
        auto started = Clock::now();
        client->setStatus(Json{{"page", "tools"}}); client->event("오프라인 줄"); client->upload("log", logPath);
        check(secondsSince(started) < .05, "offline calls must return at once");
        HubProcess again(temp / "hub2", port);   // the same port and storage: the client finds it by itself
        check(waitUntil([&] { return client->state() == Client::State::Connected; }, 15), "did not reconnect to the restarted hub");
        check(waitUntil([&] { for (const auto& file : api(again.address(), "GET", idPath + "/files/log")) if (file["name"] == "labeler-2026-10-19.log") return true; return false; }, 5), "outbox file did not upload");
        check(waitUntil([&] { const auto found = worker(again.address(), id); return !found.is_null() && found["status"].value("page", "") == "tools"; }, 5), "status set while offline must arrive after reconnect");
        client->event("종료");   // what the labeler writes as it closes: the sender may be inside that call below
        started = Clock::now();
        client.reset();   // a connected client, inside its 25 s command wait
        check(secondsSince(started) < .5, "destroying a connected client must take under half a second");
        // 2026-10-20 user report: a closed labeler stayed connected in the hub for its command wait and 15 s more
        check(!worker(again.address(), id).value("connected", true), "a closed client must show as gone in the hub at once");
        std::cout << "offline calls returned at once, outbox uploaded after the hub came back, shutdown prompt\n";
    }
    {   // lines of another worker in the same log stay theirs; a re-registration sends the status again by itself
        writeFile(logPath2, stamp(1, "2030-01-02") + "  시작 · 점검\n" + stamp(2, "2030-01-02") + "  작업자 · 다른사람@" + pc + " · 버전 0\n" + stamp(3, "2030-01-02") + "  남의 줄\n"
                          + stamp(4, "2030-01-02") + "  작업자 · 테스트@" + pc + " · 버전 0\n" + stamp(5, "2030-01-02") + "  내 줄\n");
        HubProcess hub(temp / "hub3");
        const std::string token = api(hub.address(), "POST", "/api/invites", Json{{"worker", "테스트"}})["token"];
        Client client(settings(hub.address(), token), outbox, [&] { return logPath2; }, registration);
        check(waitUntil([&] { return client.state() == Client::State::Connected; }, 3), "did not connect to the third hub");
        std::this_thread::sleep_for(1s);
        const auto events = api(hub.address(), "GET", idPath + "/events");
        std::vector<std::string> texts;
        for (const auto& event : events) texts.push_back(event["text"]);
        check(texts == std::vector<std::string>{"시작 · 점검", "작업자 · 테스트@" + pc + " · 버전 0", "내 줄"}, "the backfill must skip the lines after another worker's mark");
        client.setStatus(Json{{"page", "tools"}});
        check(waitUntil([&] { const auto found = worker(hub.address(), id); return !found.is_null() && found["status"].value("page", "") == "tools"; }, 3), "status did not arrive at the third hub");
        HubProcess fresh(temp / "hub4");
        const std::string token4 = api(fresh.address(), "POST", "/api/invites", Json{{"worker", "테스트"}})["token"];
        client.setSettings(settings(fresh.address(), token4));
        check(waitUntil([&] { return client.state() == Client::State::Connected; }, 3), "did not connect to the fourth hub");
        check(waitUntil([&] { const auto found = worker(fresh.address(), id); return !found.is_null() && found["status"].value("page", "") == "tools"; }, 3), "a new registration must send the status again without a change");
        std::cout << "other workers' lines skipped, status resent on re-registration\n";
    }
    fs::remove_all(temp);
    std::cout << "PASS: knock, register, status, events with log backfill, commands, outbox, invalid UTF-8 and shutdown\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; } }
