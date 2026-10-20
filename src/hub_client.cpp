#include "hub_client.hpp"
#include <windows.h>
#include <winhttp.h>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <utility>

namespace boccia::hub {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
constexpr auto KNOCK_EVERY = 10s;          // while the hub is off
constexpr size_t BACKFILL_LINES = 2000, BATCH = 100, QUEUE_LIMIT = 5000;
constexpr int POLL_SECONDS = 25, POLL_TIMEOUT_MS = 30000, UPLOAD_TIMEOUT_MS = 60000;
constexpr int BYE_TIMEOUT_MS = 500;      // the bye of a closing app: a hub that went away must not hold up the exit

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size);
    return result;
}
std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::string describe(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                   GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string result = text ? narrow(text) : std::string();
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '.')) result.pop_back();
    return std::to_string(code) + (result.empty() ? "" : " " + result);
}
// "host:port" → host and port (8790 when the port is missing).
std::pair<std::wstring, INTERNET_PORT> split(const std::string& address) {
    const auto colon = address.rfind(':');
    int port = 8790;
    if (colon != std::string::npos) { try { port = std::stoi(address.substr(colon + 1)); } catch (const std::exception&) {} }
    return {wide(colon == std::string::npos ? address : address.substr(0, colon)), static_cast<INTERNET_PORT>(port)};
}
// The time and text of a log line "YYYY-MM-DD HH:MM:SS.mmm  text"; the time is empty when the line has no stamp.
std::pair<std::string, std::string> splitLine(const std::string& line) {
    const bool stamped = line.size() >= 25 && line[4] == '-' && line[7] == '-' && line[10] == ' ' && line[13] == ':' && line[16] == ':'
        && line[19] == '.' && line[23] == ' ' && line[24] == ' ';
    if (stamped) return {line.substr(0, 23), line.substr(25)};
    return {"", line};
}
// The last `count` lines of a file, read from its end; a line cut by the read is left out.
std::vector<std::string> tailLines(const fs::path& path, size_t count) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    auto remaining = std::streamoff(file.tellg());
    std::string text;
    size_t newlines = 0;
    while (remaining > 0 && newlines <= count) {
        const auto block = std::min<std::streamoff>(remaining, 64 * 1024);
        remaining -= block;
        std::string chunk(size_t(block), '\0');
        file.seekg(remaining);
        file.read(chunk.data(), block);
        newlines += size_t(std::count(chunk.begin(), chunk.end(), '\n'));
        text.insert(0, chunk);
    }
    if (!text.empty() && text.back() != '\n') {   // a line still being written waits for the next read
        const auto cut = text.rfind('\n');
        text = cut == std::string::npos ? std::string() : text.substr(0, cut + 1);
    }
    std::vector<std::string> lines;
    for (size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        start = end + 1;
    }
    if (lines.size() > count) lines.erase(lines.begin(), lines.begin() + std::ptrdiff_t(lines.size() - count));
    return lines;
}
// A name the hub accepts ([A-Za-z0-9._-]); anything else becomes _.
std::string safeName(const std::string& name) {
    std::string result;
    for (const unsigned char c : name) result += (std::isalnum(c) || c == '.' || c == '_' || c == '-') ? char(c) : '_';
    if (result.find_first_not_of('.') == std::string::npos) result = "file";
    return result;
}
std::string newSession() {
    std::random_device random;
    char text[33];
    std::snprintf(text, sizeof text, "%08x%08x%08x%08x", random(), random(), random(), random());
    return text;
}
std::vector<std::string> authHeaders(const Settings& settings, bool json = true) {
    return {"Authorization: Bearer " + settings.token, "X-Boccia-Pc: " + percentEncode(settings.pc),
            json ? "Content-Type: application/json; charset=utf-8" : "Content-Type: application/octet-stream"};
}
// JSON for the wire: a byte that is not UTF-8 (a CP949 path inside an error message) becomes U+FFFD, not an exception.
std::string wire(const Json& value) { return value.dump(-1, ' ', false, Json::error_handler_t::replace); }
}

std::string percentEncode(const std::string& utf8) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : utf8) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') result += char(c);
        else { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
    }
    return result;
}

std::string nowStamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const int milliseconds = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &seconds);
    char text[32], fraction[8];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &local);
    std::snprintf(fraction, sizeof fraction, ".%03d", milliseconds);
    return std::string(text) + fraction;
}

std::string hostName() {
    wchar_t name[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &size) && size) return narrow(std::wstring(name, size));
    size = 256;
    if (GetComputerNameW(name, &size) && size) return narrow(std::wstring(name, size));
    return "pc";
}

std::string gpuName() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return {};
    std::string first, nvidia;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            const std::string name = narrow(description.Description);
            if (first.empty()) first = name;
            if (description.VendorId == 0x10DE && nvidia.empty()) nvidia = name;
        }
        adapter->Release();
    }
    factory->Release();
    return nvidia.empty() ? first : nvidia;
}

std::string accountName() {
    wchar_t name[256];
    DWORD size = 256;
    if (GetUserNameW(name, &size) && size > 1) return narrow(std::wstring(name, size - 1));
    return "";
}

// ---- Session

Session::Session(int timeoutMs) {
    handle_ = WinHttpOpen(L"BocciaHub/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (handle_ && timeoutMs > 0) WinHttpSetTimeouts(handle_, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    else if (handle_) WinHttpSetTimeouts(handle_, 3000, 3000, 5000, 5000);
}
Session::~Session() { if (handle_) WinHttpCloseHandle(handle_); }

Reply Session::request(const std::string& address, const char* method, const std::string& path, const std::string& body,
                       const std::vector<std::string>& headers, int timeoutMs) {
    Reply reply;
    if (!handle_) { reply.error = "WinHttpOpen failed"; return reply; }
    const auto [host, port] = split(address);
    HINTERNET connection = WinHttpConnect(handle_, host.c_str(), port, 0);
    if (!connection) { reply.error = describe(GetLastError()); return reply; }
    HINTERNET request = WinHttpOpenRequest(connection, wide(method).c_str(), wide(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) { reply.error = describe(GetLastError()); WinHttpCloseHandle(connection); return reply; }
    {
        std::lock_guard guard(lock_);
        if (cancelled_) { WinHttpCloseHandle(request); WinHttpCloseHandle(connection); reply.error = "cancelled"; return reply; }
        active_ = request;
    }
    DWORD timeout = DWORD(timeoutMs);
    WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);
    WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof timeout);
    std::wstring headerText;
    for (const auto& header : headers) headerText += wide(header) + L"\r\n";
    const wchar_t* extra = headerText.empty() ? nullptr : headerText.c_str();
    void* data = body.empty() ? nullptr : const_cast<char*>(body.data());
    const BOOL sent = WinHttpSendRequest(request, extra, extra ? DWORD(-1) : 0, data, DWORD(body.size()), DWORD(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) reply.error = describe(GetLastError());
    else {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        reply.status = int(status);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || !available) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            reply.body.append(chunk, 0, read);
        }
    }
    {
        std::lock_guard guard(lock_);
        const bool mine = active_ == request;   // interrupt() or cancel() may have closed it meanwhile
        active_ = nullptr;
        if (mine) WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connection);
    return reply;
}

void Session::interrupt() {
    std::lock_guard guard(lock_);
    if (active_) { WinHttpCloseHandle(active_); active_ = nullptr; }   // the blocked call on the other thread returns with an error
}

void Session::cancel() {
    std::lock_guard guard(lock_);
    cancelled_ = true;
    if (active_) { WinHttpCloseHandle(active_); active_ = nullptr; }
}

// ---- Client

// Shared by the app and the two threads; a thread still inside a request when the Client dies keeps it alive.
struct Client::Core {
    mutable std::mutex lock;
    std::condition_variable wake;
    Settings settings;                      // under lock
    std::string sessionId = newSession();   // under lock: this instance's registration; the hub ends polls of older ones
    bool settingsChanged = false;           // under lock
    const fs::path outbox;
    const std::function<fs::path()> logFile;
    const std::function<Registration()> registration;
    std::atomic<State> state{State::Offline};
    std::atomic<Clock::time_point> seen{};
    std::string error;                      // under lock
    Json status;                            // under lock
    bool statusDirty = false, outboxDirty = false;   // under lock
    std::vector<Json> events, results;      // under lock: to send
    std::vector<Command> commands;          // under lock: for the app
    bool stop = false, senderDone = false, pollerDone = false;   // under lock
    Session senderSession, pollerSession;   // one per thread; the app's thread only interrupts or cancels them
    Core(Settings made, fs::path box, std::function<fs::path()> log, std::function<Registration()> extra)
        : settings(std::move(made)), outbox(std::move(box)), logFile(std::move(log)), registration(std::move(extra)) {}
    void fail(const std::string& what, const Reply& reply) {
        state = State::Offline;
        std::lock_guard guard(lock);
        error = what + ": " + (reply.status ? "HTTP " + std::to_string(reply.status) + (reply.body.empty() ? "" : " " + reply.body.substr(0, 200)) : reply.error);
    }
    bool postEvents(Session& session, const Settings& current, const Json& batch) {
        const auto reply = session.request(current.address, "POST", "/events", wire(Json{{"events", batch}}), authHeaders(current));
        if (reply.status != 200) { fail("events", reply); return false; }
        seen = Clock::now();
        return true;
    }
    // After a (re)connect: the log's last lines that the hub has not seen, oldest first, 100 at a time. Only this worker's
    // lines: the app writes "작업자 · <worker>@<pc> …" when it (re)starts the client, so a line after another worker's mark is
    // that worker's; lines before any mark count as ours.
    bool backfill(Session& session, const Settings& current, const std::string& since) {
        const fs::path file = logFile ? logFile() : fs::path();
        if (file.empty()) return true;
        static const std::string mark = "작업자 · ";
        std::string owner;
        Json batch = Json::array();
        for (const auto& line : tailLines(file, BACKFILL_LINES)) {
            const auto [time, text] = splitLine(line);
            if (time.empty()) continue;
            if (text.compare(0, mark.size(), mark) == 0) { const auto at = text.find('@', mark.size()); owner = text.substr(mark.size(), at == std::string::npos ? std::string::npos : at - mark.size()); }
            if (!owner.empty() && owner != current.worker) continue;
            if (time <= since) continue;
            batch.push_back(Json{{"time", time}, {"text", text}});
            if (batch.size() == BATCH) { if (!postEvents(session, current, batch)) return false; batch = Json::array(); }
        }
        return batch.empty() || postEvents(session, current, batch);
    }
    // Every file waiting in the outbox; a file the hub refuses for good is dropped, a hub that is gone keeps them.
    void sendOutbox(Session& session, const Settings& current) {
        for (const char* kind : {"log", "screen"}) {
            std::error_code ignored;
            for (const auto& entry : fs::directory_iterator(outbox / kind, ignored)) {
                if (!entry.is_regular_file(ignored)) continue;
                std::ifstream file(entry.path(), std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                file.close();
                const std::string name = narrow(entry.path().filename().wstring());
                const auto reply = session.request(current.address, "POST", std::string("/files/") + kind + "/" + name, bytes, authHeaders(current, false), UPLOAD_TIMEOUT_MS);
                if (reply.status == 0 || reply.status / 100 == 5) { fail("upload", reply); std::lock_guard guard(lock); outboxDirty = true; return; }
                if (reply.status != 200) { std::lock_guard guard(lock); error = "upload " + name + ": HTTP " + std::to_string(reply.status); }
                fs::remove(entry.path(), ignored);
                seen = Clock::now();
            }
        }
    }
    // While connected: the status when it changed, command results, queued events, then the outbox when something was added.
    void flush(Session& session, const Settings& current) {
        Json state_;
        bool dirty = false;
        { std::lock_guard guard(lock); dirty = statusDirty; state_ = status; statusDirty = false; }
        if (dirty) {
            const auto reply = session.request(current.address, "POST", "/status", wire(state_), authHeaders(current));
            if (reply.status != 204) { fail("status", reply); std::lock_guard guard(lock); statusDirty = true; return; }
            seen = Clock::now();
        }
        std::vector<Json> pending;
        { std::lock_guard guard(lock); pending.swap(results); }
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto reply = session.request(current.address, "POST", "/commands/" + pending[i].value("id", "") + "/result", wire(pending[i]), authHeaders(current));
            if (reply.status == 0) {   // no hub: the rest waits
                fail("result", reply);
                std::lock_guard guard(lock);
                results.insert(results.begin(), pending.begin() + std::ptrdiff_t(i), pending.end());
                return;
            }
            seen = Clock::now();       // a 404 means the hub forgot the command: nothing to keep
        }
        std::vector<Json> queued;
        { std::lock_guard guard(lock); queued.swap(events); }
        for (size_t at = 0; at < queued.size(); at += BATCH) {
            Json batch = Json::array();
            for (size_t i = at; i < queued.size() && i < at + BATCH; ++i) batch.push_back(queued[i]);
            if (!postEvents(session, current, batch)) return;   // the rest is dropped: the log file is the record
        }
        bool added = false;
        { std::lock_guard guard(lock); added = outboxDirty; outboxDirty = false; }
        if (added) sendOutbox(session, current);
    }
    // Offline: /ping, then /register; a refusal (401/403) ends in Unauthorized until the settings change.
    void knock(Session& session, const Settings& current, const std::string& instance) {
        auto reply = session.request(current.address, "GET", "/ping");
        if (reply.status != 200) { fail("ping", reply); return; }
        const Registration extra = registration ? registration() : Registration{};
        const Json body{{"app", current.app}, {"worker", current.worker}, {"pc", current.pc}, {"account", current.account},
                        {"version", current.version}, {"session", instance}, {"device", extra.device}, {"summary", extra.summary}};
        reply = session.request(current.address, "POST", "/register", wire(body), authHeaders(current));
        if (reply.status != 200) { fail("register", reply); if (reply.status == 401 || reply.status == 403) state = State::Unauthorized; return; }
        std::string since;
        try { since = Json::parse(reply.body).value("since", ""); } catch (const std::exception&) { fail("register", reply); return; }
        seen = Clock::now();
        { std::lock_guard guard(lock); error.clear(); statusDirty = !status.is_null(); }   // a new hub, or a restarted one, gets the status again
        state = State::Connected;
        wake.notify_all();   // the poller starts
        if (!backfill(session, current, since)) return;
        sendOutbox(session, current);
        if (state == State::Connected) flush(session, current);
    }
};

void Client::sendLoop(std::shared_ptr<Core> core) {
    Session& session = core->senderSession;
    Clock::time_point nextKnock{};   // at once
    bool first = true;               // the first cycle does not wait: a short-lived app still gets to knock
    for (;;) {
        Settings current;
        std::string instance;
        {
            std::unique_lock guard(core->lock);
            core->wake.wait_for(guard, first ? 0s : 1s, [&] {
                return core->stop || core->settingsChanged
                    || (core->state == State::Connected && (core->statusDirty || core->outboxDirty || !core->events.empty() || !core->results.empty()));
            });
            if (core->stop) break;
            if (core->settingsChanged) { core->settingsChanged = false; core->state = State::Offline; nextKnock = {}; }
            current = core->settings;
            instance = core->sessionId;
        }
        first = false;
        try {
            if (core->state == State::Offline) {
                if (Clock::now() < nextKnock) continue;
                nextKnock = Clock::now() + KNOCK_EVERY;
                core->knock(session, current, instance);
            } else if (core->state == State::Connected) core->flush(session, current);
        } catch (const std::exception& error) { core->fail("client", Reply{0, "", error.what()}); }   // never out of the thread
    }
    { std::lock_guard guard(core->lock); core->senderDone = true; }
    core->wake.notify_all();
}

void Client::pollLoop(std::shared_ptr<Core> core) {
    Session& session = core->pollerSession;
    for (;;) {
        Settings current;
        std::string instance;
        {
            std::unique_lock guard(core->lock);
            core->wake.wait(guard, [&] { return core->stop || core->state == State::Connected; });
            if (core->stop) break;
            current = core->settings;
            instance = core->sessionId;
        }
        auto headers = authHeaders(current);
        headers.push_back("X-Boccia-Session: " + instance);
        const auto reply = session.request(current.address, "GET", "/commands?wait=" + std::to_string(POLL_SECONDS), "", headers, POLL_TIMEOUT_MS);
        {
            std::lock_guard guard(core->lock);
            if (core->stop) break;
            if (instance != core->sessionId) continue;   // the settings changed meanwhile: this answer belongs to the old registration
        }
        if (reply.status == 200) {
            try {
                std::vector<Command> received;
                for (const auto& item : Json::parse(reply.body).value("commands", Json::array())) {
                    Command command;
                    command.id = item.value("id", "");
                    command.name = item.value("name", "");
                    const Json value = item.value("value", Json());
                    command.value = value.is_string() ? value.get<std::string>() : value.is_null() ? "" : value.dump();
                    received.push_back(command);
                }
                core->seen = Clock::now();
                std::lock_guard guard(core->lock);
                core->commands.insert(core->commands.end(), received.begin(), received.end());
            } catch (const std::exception&) { core->fail("commands", reply); }
        } else {
            core->fail("commands", reply);   // 403 (register first), 409 (another instance registered) and no answer: knock again
            if (reply.status == 401) core->state = State::Unauthorized;
        }
    }
    { std::lock_guard guard(core->lock); core->pollerDone = true; }
    core->wake.notify_all();
}

Client::Client(Settings settings, fs::path outbox, std::function<fs::path()> logFile, std::function<Registration()> registration)
    : core_(std::make_shared<Core>(std::move(settings), std::move(outbox), std::move(logFile), std::move(registration))) {
    sender_ = std::thread(sendLoop, core_);
    poller_ = std::thread(pollLoop, core_);
}

Client::~Client() {
    const bool connected = core_->state == State::Connected;   // before the cancels below: a call they cut marks it Offline
    Settings current;
    std::string instance;
    { std::lock_guard guard(core_->lock); core_->stop = true; current = core_->settings; instance = core_->sessionId; }
    core_->wake.notify_all();
    core_->senderSession.cancel();   // a request in progress (the 25 s command wait above all) returns now
    core_->pollerSession.cancel();
    // POST /bye: the hub shows this app as gone at once, not after the command wait just cut and its 15 s after the last
    // call (2026-10-20 user report). A crash still shows that way.
    if (connected) {
        auto headers = authHeaders(current);
        headers.push_back("X-Boccia-Session: " + instance);
        Session(BYE_TIMEOUT_MS).request(current.address, "POST", "/bye", "{}", headers, BYE_TIMEOUT_MS);
    }
    bool senderDone = false, pollerDone = false;
    {
        std::unique_lock guard(core_->lock);
        core_->wake.wait_for(guard, 2s, [&] { return core_->senderDone && core_->pollerDone; });
        senderDone = core_->senderDone;
        pollerDone = core_->pollerDone;
    }
    if (senderDone) sender_.join(); else sender_.detach();
    if (pollerDone) poller_.join(); else poller_.detach();
}

void Client::setStatus(Json state) {
    { std::lock_guard guard(core_->lock); if (core_->status != state) { core_->status = std::move(state); core_->statusDirty = true; } }
    core_->wake.notify_all();
}

void Client::event(const std::string& text) {
    auto [time, body] = splitLine(text);
    if (time.empty()) time = nowStamp();
    if (core_->state != State::Connected) return;
    { std::lock_guard guard(core_->lock); if (core_->events.size() < QUEUE_LIMIT) core_->events.push_back(Json{{"time", time}, {"text", body}}); }
    core_->wake.notify_all();
}

void Client::upload(const std::string& kind, const fs::path& file) {
    std::error_code error;
    const fs::path folder = core_->outbox / kind;
    fs::create_directories(folder, error);
    fs::copy_file(file, folder / safeName(narrow(file.filename().wstring())), fs::copy_options::overwrite_existing, error);
    if (error) { std::lock_guard guard(core_->lock); core_->error = "outbox: error " + std::to_string(error.value()); return; }
    { std::lock_guard guard(core_->lock); core_->outboxDirty = true; }
    core_->wake.notify_all();
}

std::vector<Command> Client::takeCommands() { std::lock_guard guard(core_->lock); return std::exchange(core_->commands, {}); }

void Client::finish(const Command& command, bool ok, const std::string& text) {
    { std::lock_guard guard(core_->lock); core_->results.push_back(Json{{"id", command.id}, {"ok", ok}, {"text", text}}); }
    core_->wake.notify_all();
}

Client::State Client::state() const { return core_->state; }
std::string Client::lastError() const { std::lock_guard guard(core_->lock); return core_->error; }
Clock::time_point Client::lastSeen() const { return core_->seen; }

void Client::setSettings(Settings settings) {
    { std::lock_guard guard(core_->lock); core_->settings = std::move(settings); core_->sessionId = newSession(); core_->settingsChanged = true; core_->state = State::Offline; }
    core_->wake.notify_all();
    core_->pollerSession.interrupt();   // the command wait on the old hub ends now; the next one goes to the new settings
}
}
