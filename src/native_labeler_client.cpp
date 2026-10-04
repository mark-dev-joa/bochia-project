#include "native_labeler_client.hpp"
#include <windows.h>
#include <winhttp.h>
#include <opencv2/imgcodecs.hpp>
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
        throw std::runtime_error(value.is_object() ? value.value("error", "서버 오류") : "서버를 업데이트한 뒤 다시 연결하세요. HTTP " + std::to_string(status));
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
void Client::startServer(int port, const std::filesystem::path& source, const std::filesystem::path& project, const std::filesystem::path& exports) {
    auto quote = [](const std::filesystem::path& value) {
        auto text=value.native();if(text.find(L'"')!=std::wstring::npos)throw std::runtime_error("경로에 따옴표를 사용할 수 없습니다.");
        // Windows argv parsing doubles backslashes immediately before a quote.
        auto last=text.find_last_not_of(L'\\');auto trailing=last==std::wstring::npos?text.size():text.size()-last-1;
        return L"\""+text+std::wstring(trailing,L'\\')+L"\"";
    };
    const auto workspace = fromUtf8(BOCCIA_LABELER_ROOT);
    std::wstring command = quote(fromUtf8(BOCCIA_LABELER_PYTHON)) + L" -u " + quote(workspace / "ball_labeler.py")
        + L" --no-browser --port " + std::to_wstring(port) + L" --source " + quote(source) + L" --project " + quote(project) + L" --export-root " + quote(exports);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, workspace.c_str(), &startup, &process)) throw std::runtime_error("로컬 분할·저장 엔진을 실행하지 못했습니다.");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
}
