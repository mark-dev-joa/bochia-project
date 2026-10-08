#include "labeler_util.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <format>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace boccia::labeler {
namespace {
void require(NTSTATUS status, const char* what) { if (status < 0) throw std::runtime_error(std::string("Windows cryptography failed: ") + what); }
}
Sha256::Sha256() {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "open SHA-256");
    algorithm_ = algorithm;
    require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0), "create hash");
    hash_ = hash;
}
Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    if (algorithm_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
}
void Sha256::update(std::string_view bytes) {
    if (bytes.empty()) return;
    require(BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0), "hash data");
}
std::string Sha256::hex() {
    unsigned char digest[32];
    require(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest, sizeof digest, 0), "finish hash");
    static const char* digits = "0123456789abcdef";
    std::string text; text.reserve(64);
    for (unsigned char byte : digest) { text.push_back(digits[byte >> 4]); text.push_back(digits[byte & 15]); }
    return text;
}
std::string sha256Hex(std::string_view bytes) { Sha256 hash; hash.update(bytes); return hash.hex(); }
std::string randomHex(int bytes) {
    std::vector<unsigned char> buffer(size_t(std::max(1, bytes)));
    require(BCryptGenRandom(nullptr, buffer.data(), static_cast<ULONG>(buffer.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "random bytes");
    static const char* digits = "0123456789abcdef";
    std::string text; text.reserve(buffer.size() * 2);
    for (unsigned char byte : buffer) { text.push_back(digits[byte >> 4]); text.push_back(digits[byte & 15]); }
    return text;
}
std::string utcIso() {
    const auto now = std::chrono::floor<std::chrono::microseconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%S}+00:00", now);
}
std::string localStamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    tm local{}; localtime_s(&local, &now);
    char text[32]{}; strftime(text, sizeof text, "%Y%m%d-%H%M%S", &local);
    return text;
}
void atomicJson(const std::filesystem::path& path, const Json& value, bool indent) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path; temporary += "." + randomHex(16) + ".tmp";
    struct Cleanup { const std::filesystem::path& file; ~Cleanup() { std::error_code ignored; std::filesystem::remove(file, ignored); } } cleanup{temporary};
    const std::string text = value.dump(indent ? 2 : -1) + "\n";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("임시 파일을 만들지 못했습니다: " + utf8(temporary));
    bool ok = true;
    for (size_t offset = 0; ok && offset < text.size();) {
        DWORD written = 0;
        ok = WriteFile(file, text.data() + offset, static_cast<DWORD>(std::min<size_t>(text.size() - offset, 1u << 30)), &written, nullptr) && written > 0;
        offset += written;
    }
    ok = ok && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) throw std::runtime_error("파일을 쓰지 못했습니다: " + utf8(path));
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("파일을 바꾸지 못했습니다 (" + std::to_string(GetLastError()) + "): " + utf8(path));
}
bool localPortListening(int port, int timeoutMs) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    bool listening = false;
    const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle != INVALID_SOCKET) {
        u_long nonBlocking = 1; ioctlsocket(handle, FIONBIO, &nonBlocking);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(u_short(port)); inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0) listening = true;
        else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable; FD_ZERO(&writable); FD_SET(handle, &writable);
            timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            if (select(0, nullptr, &writable, nullptr, &timeout) == 1) { int error = 0; int size = sizeof error; getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size); listening = error == 0; }
        }
        closesocket(handle);
    }
    WSACleanup();
    return listening;
}
bool pathLess(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto pieces = [](const std::filesystem::path& path) {
        std::vector<std::wstring> result;
        for (const auto& piece : path) {
            std::wstring text = piece.wstring();
            if (text == L"\\" || text == L"/") continue;   // the root separator is not a piece in str(path).split(sep)
            for (auto& character : text) character = static_cast<wchar_t>(std::towlower(character));
            result.push_back(std::move(text));
        }
        return result;
    };
    return pieces(a) < pieces(b);
}
}
