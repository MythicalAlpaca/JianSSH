#include "jianssh/app/private_key_file.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace jianssh::app {
namespace {
constexpr std::size_t max_size = 1024 * 1024;
// A fixed allocation avoids un-wiped copies on buffer growth. Volatile stores
// prevent the compiler from optimizing the wipe away, including on early exit.
struct SecretBuffer {
    std::array<char, max_size + 1> bytes{};
    ~SecretBuffer() {
        volatile char* p = bytes.data();
        for (std::size_t i = 0; i < bytes.size(); ++i) p[i] = 0;
    }
};
#ifdef _WIN32
struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#else
struct Handle {
    int value;
    ~Handle() { if (value >= 0) close(value); }
};
#endif
bool base64_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
}
// Accept standard PEM legacy encryption headers, then base64, with matching
// delimiters and no material outside the envelope. No decoded secret copies.
bool plausible(std::string_view data) {
    constexpr std::string_view labels[] = {"OPENSSH PRIVATE KEY", "RSA PRIVATE KEY",
        "DSA PRIVATE KEY", "EC PRIVATE KEY", "PRIVATE KEY", "ENCRYPTED PRIVATE KEY"};
    auto next_line = [&data]() {
        auto n = data.find('\n');
        auto line = data.substr(0, n);
        data = n == std::string_view::npos ? std::string_view{} : data.substr(n + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return line;
    };
    auto first = next_line();
    std::string_view label;
    for (auto candidate : labels) {
        if (first.size() == candidate.size() + 16 && first.substr(0, 11) == "-----BEGIN " &&
            first.substr(11, candidate.size()) == candidate && first.substr(11 + candidate.size()) == "-----") {
            label = candidate; break;
        }
    }
    if (label.empty()) return false;
    bool legacy = false, headers_done = false, saw_data = false, padding = false, ended = false;
    std::size_t count = 0, pads = 0;
    while (!data.empty()) {
        auto line = next_line();
        if (line.size() == label.size() + 14 && line.substr(0, 9) == "-----END " &&
            line.substr(9, label.size()) == label && line.substr(9 + label.size()) == "-----") {
            ended = true; break;
        }
        if (!saw_data && !headers_done && line.substr(0, 11) == "Proc-Type: ") { legacy = true; continue; }
        if (!saw_data && !headers_done && legacy && line.substr(0, 10) == "DEK-Info: ") continue;
        if (!saw_data && legacy && line.empty()) { headers_done = true; continue; }
        if (line.empty()) return false;
        saw_data = true;
        for (char c : line) {
            if (c == '=') { padding = true; if (++pads > 2) return false; }
            else if (!base64_char(c) || padding) return false;
            ++count;
        }
    }
    if (!ended || count < 32 || count % 4 != 0) return false;
    for (char c : data) if (c != '\r' && c != '\n' && c != ' ' && c != '\t') return false;
    return true;
}
} // namespace

std::string validate_private_key_file(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos) return "私钥文件路径无效";
#ifdef _WIN32
    if (path.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return "私钥文件路径无效";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
    if (!n) return "私钥文件路径不是有效的 UTF-8";
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), wide.data(), n)) return "私钥文件路径无效";
    Handle file{CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return "无法打开或读取私钥文件";
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file.value) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return "私钥路径不是普通文件";
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
    std::string name(path);
    Handle file{open(name.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
    if (file.value < 0) return "无法打开或读取私钥文件";
    struct stat info{};
    if (fstat(file.value, &info) != 0 || !S_ISREG(info.st_mode)) return "私钥路径不是普通文件";
    const std::uint64_t size = info.st_size < 0 ? 0 : static_cast<std::uint64_t>(info.st_size);
#endif
    if (size > max_size) return "私钥文件过大（最多 1 MiB）";
    // Heap allocation keeps the large buffer off worker stacks.
    auto buffer = std::make_unique<SecretBuffer>();
    std::size_t used = 0;
    while (used < buffer->bytes.size()) {
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(file.value, buffer->bytes.data() + used, static_cast<DWORD>(buffer->bytes.size() - used), &got, nullptr)) return "读取私钥文件失败";
        if (!got) break;
        used += got;
#else
        auto got = read(file.value, buffer->bytes.data() + used, buffer->bytes.size() - used);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return "读取私钥文件失败";
        if (!got) break;
        used += static_cast<std::size_t>(got);
#endif
    }
    if (used > max_size) return "私钥文件过大（最多 1 MiB）";
    if (!used) return "私钥文件为空";
    if (!plausible(std::string_view(buffer->bytes.data(), used))) return "私钥文件格式无效或不受支持";
    return {};
}
} // namespace jianssh::app
