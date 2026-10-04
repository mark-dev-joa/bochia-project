#include "native_labeler_client.hpp"
#include <windows.h>
#include <winhttp.h>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <stdexcept>

namespace boccia::labeler {
namespace {
std::wstring wide(const std::string& text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring value(count, 0); MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), value.data(), count); return value;
}
struct Handle { HINTERNET value{}; explicit Handle(HINTERNET v): value(v) { if (!value) throw std::runtime_error("로컬 서버 연결을 시작하지 못했습니다."); } ~Handle(){ if(value) WinHttpCloseHandle(value); } };
}
std::vector<unsigned char> Client::request(const std::string& method, const std::string& path, const std::string& body) const {
    Handle session(WinHttpOpen(L"Boccia Native Labeler/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session.value, 2000, 2000, 15000, 90000);
    Handle connection(WinHttpConnect(session.value, L"127.0.0.1", INTERNET_PORT(port_), 0));
    auto route = wide(path), verb = wide(method);
    Handle query(WinHttpOpenRequest(connection.value, verb.c_str(), route.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    const wchar_t* headers = L"Content-Type: application/json\r\n";
    if (!WinHttpSendRequest(query.value, headers, DWORD(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0)
        || !WinHttpReceiveResponse(query.value, nullptr)) throw std::runtime_error("로컬 서버에 연결하지 못했습니다. 연결 상태와 서버 로그를 확인하세요.");
    DWORD status{}, length = sizeof(status); WinHttpQueryHeaders(query.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX);
    std::vector<unsigned char> result;
    for (;;) {
        DWORD available{}; if (!WinHttpQueryDataAvailable(query.value, &available)) throw std::runtime_error("서버 응답을 읽지 못했습니다.");
        if (!available) break;
        if (result.size() + available > 40 * 1024 * 1024) throw std::runtime_error("서버 응답이 너무 큽니다.");
        const auto offset = result.size(); result.resize(offset + available); DWORD read{};
        if (!WinHttpReadData(query.value, result.data() + offset, available, &read)) throw std::runtime_error("서버 응답이 중단됐습니다.");
        result.resize(offset + read);
    }
    if (status < 200 || status >= 300) {
        auto value = Json::parse(result.begin(), result.end(), nullptr, false);
        const std::string message = value.is_object() ? value.value("error", "서버 오류") : "서버를 업데이트한 뒤 다시 연결하세요. HTTP " + std::to_string(status);
        if (status == 409) throw Conflict(message);
        throw std::runtime_error(message);
    }
    return result;
}
Json Client::get(const std::string& path) const { auto data = request("GET", path, ""); return Json::parse(data.begin(), data.end()); }
Json Client::post(const std::string& path, const Json& payload) const { auto data = request("POST", path, payload.dump()); return Json::parse(data.begin(), data.end()); }
std::string Client::frameQuery(const Json& member, const std::string& recording) {
    return "?recording=" + recording + "&camera=" + member.at("camera").get<std::string>() + "&index=" + std::to_string(member.at("index").get<int>());
}
cv::Mat Client::image(const Json& member, const std::string& recording) const {
    auto bytes = request("GET", "/api/image" + frameQuery(member, recording), "");
    auto image = cv::imdecode(bytes, cv::IMREAD_COLOR); if (image.empty()) throw std::runtime_error("이미지를 복원하지 못했습니다."); return image;
}
cv::Mat Client::thumbnail(const std::string& key) const {
    const auto first = key.find('/'), second = key.find('/', first + 1);
    if (first == std::string::npos || second == std::string::npos) throw std::runtime_error("프레임 키가 올바르지 않습니다.");
    auto bytes = request("GET", "/api/native/thumb?recording=" + key.substr(0, first) + "&camera=" + key.substr(first + 1, second - first - 1) + "&index=" + key.substr(second + 1), "");
    return cv::imdecode(bytes, cv::IMREAD_COLOR);
}
std::filesystem::path Client::engineLog(const std::filesystem::path& project) { auto log = project; log.replace_extension(); log += L"-engine.log"; return log; }
std::string Client::engineError(const std::filesystem::path& project) {
    std::ifstream stream(engineLog(project), std::ios::binary);
    std::string line, last;
    while (std::getline(stream, line)) { while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back(); if (!line.empty()) last = line; }
    return last;
}
void* Client::startServer(int port, const std::filesystem::path& source, const std::filesystem::path& project, const std::filesystem::path& exports) {
    auto quote = [](const std::filesystem::path& value) {
        auto text=value.native();if(text.find(L'"')!=std::wstring::npos)throw std::runtime_error("경로에 따옴표를 사용할 수 없습니다.");
        // Windows argv parsing doubles backslashes immediately before a quote.
        auto last=text.find_last_not_of(L'\\');auto trailing=last==std::wstring::npos?text.size():text.size()-last-1;
        return L"\""+text+std::wstring(trailing,L'\\')+L"\"";
    };
    const auto workspace = fromUtf8(BOCCIA_LABELER_ROOT);
    std::wstring command = quote(fromUtf8(BOCCIA_LABELER_PYTHON)) + L" -u " + quote(workspace / "ball_labeler.py")
        + L" --no-browser --port " + std::to_wstring(port) + L" --source " + quote(source) + L" --project " + quote(project) + L" --export-root " + quote(exports);
    // The engine has no window, so what it prints goes to a log beside the project; the app shows its last line when the engine stops.
    std::error_code ignored; std::filesystem::create_directories(project.parent_path(), ignored);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    const HANDLE log = CreateFileW(engineLog(project).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    // Only these two handles are passed on. Inheriting everything would also hand the engine this app's own output pipes,
    // and a script waiting on the app would then wait for as long as the engine lives.
    HANDLE passed[] = {input, log};
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> buffer(size);
    const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    const bool listed = log != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    const bool redirected = listed && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, passed, sizeof(passed), nullptr, nullptr);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (redirected) { startup.lpAttributeList = attributes; startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput = input; startup.StartupInfo.hStdOutput = log; startup.StartupInfo.hStdError = log; }
    else startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");   // the child inherits it, so the log can be read back as UTF-8
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, redirected, CREATE_NO_WINDOW | (redirected ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, workspace.c_str(), &startup.StartupInfo, &process);
    if (listed) DeleteProcThreadAttributeList(attributes);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!started) throw std::runtime_error("로컬 분할·저장 엔진을 실행하지 못했습니다.");
    CloseHandle(process.hThread);
    return process.hProcess;
}
}
