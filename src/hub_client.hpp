#pragma once
// The hub client both apps share (design/hub/DESIGN.md): it knocks on the hub while the hub is off, registers when it
// answers, then sends status, events and files and polls for commands, on two threads of its own. The app only queues
// and takes; nothing here blocks the app's thread.
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace boccia::hub {
using Json = nlohmann::json;
struct Settings { std::string address, token, worker, pc, account, app, version; };   // address "host:port"
struct Command { std::string id, name, value; };
struct Registration { Json device, summary; };   // the app's own part of /register
struct Reply { int status = 0; std::string body, error; };   // status 0: no answer, error says why

// One WinHTTP session, used from one thread: GET/POST to the hub with a UTF-8 body and extra headers ("Name: value").
// Another thread may end the request in progress (interrupt) or end it and refuse the next ones (cancel).
class Session {
public:
    explicit Session(int timeoutMs = 0);   // > 0: every phase of a call (name, connection, send, answer) waits at most that long
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Reply request(const std::string& address, const char* method, const std::string& path, const std::string& body = "",
                  const std::vector<std::string>& headers = {}, int timeoutMs = 5000);
    void interrupt();   // the blocked call returns with an error; the next request runs as usual
    void cancel();      // interrupt, and every later request returns at once
private:
    void* handle_ = nullptr;
    std::mutex lock_;
    void* active_ = nullptr;    // under lock_: the request handle a call is blocked on
    bool cancelled_ = false;    // under lock_
};

class Client {
public:
    enum class State { Offline, Connected, Unauthorized };
    // outbox: files to upload when offline; logFile: the app's log, read for the backfill after a (re)connect;
    // registration: the app's device and summary for /register, called on the client's own thread.
    Client(Settings settings, std::filesystem::path outbox, std::function<std::filesystem::path()> logFile, std::function<Registration()> registration);
    ~Client();   // stops both threads, waiting up to 2 s; a thread still inside a request finishes on its own
    void setStatus(Json state);                   // replaces; sent on the next cycle when it changed
    void event(const std::string& text);          // "YYYY-MM-DD HH:MM:SS.mmm  text" or plain text (time = now); dropped while offline, the log file is the record
    void upload(const std::string& kind, const std::filesystem::path& file);   // log|screen; copied to the outbox, sent when connected
    std::vector<Command> takeCommands();          // for the app thread; empty when none
    void finish(const Command& command, bool ok, const std::string& text);
    State state() const;
    std::string lastError() const;
    std::chrono::steady_clock::time_point lastSeen() const;   // the last answer from the hub
    void setSettings(Settings settings);          // a new address/token/name: reconnects
private:
    struct Core;
    static void sendLoop(std::shared_ptr<Core> core);
    static void pollLoop(std::shared_ptr<Core> core);
    std::shared_ptr<Core> core_;
    std::thread sender_, poller_;
};
std::string hostName();      // this PC's name (DNS host name)
std::string gpuName();       // the first NVIDIA adapter's name, else the first hardware adapter's, else ""
std::string accountName();   // the Windows account
std::string percentEncode(const std::string& utf8);   // for ids in paths and the X-Boccia-Pc header
std::string nowStamp();      // "YYYY-MM-DD HH:MM:SS.mmm", local time, the log's stamp
}
