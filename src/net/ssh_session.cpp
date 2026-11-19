#include <jianssh/net/ssh_session.hpp>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include "jianssh/app/file_paths.hpp"
#include <unordered_set>
#include <sys/stat.h>
#include <limits>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#define MSG_NOSIGNAL 0
#define SHUT_RDWR SD_BOTH
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace jianssh::net {
namespace {
using Clock = std::chrono::steady_clock;
using Socket = libssh2_socket_t;
#ifdef _WIN32
using SockLen = int;
constexpr Socket invalid_socket = LIBSSH2_INVALID_SOCKET;
inline bool socket_valid(Socket s) { return s != invalid_socket; }
inline void close_socket(Socket s) { if (s != invalid_socket) ::closesocket(s); }
inline int socket_error() { return ::WSAGetLastError(); }
inline bool socket_would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
inline bool socket_not_connected(int e) { return e == WSAENOTCONN; }
inline void set_nonblocking(Socket s) { u_long mode = 1; ::ioctlsocket(s, FIONBIO, &mode); }
inline std::string socket_error_text(int e) {
    switch (e) {
    case WSAECONNREFUSED: return "连接被拒绝";
    case WSAETIMEDOUT: return "连接超时";
    case WSAENETUNREACH: return "网络不可达";
    case WSAEHOSTUNREACH: return "主机不可达";
    case WSAEACCES: return "权限不足";
    case WSAEADDRINUSE: return "地址已被占用";
    case WSAENOTCONN: return "连接未建立";
    case WSAECONNRESET: return "连接被重置";
    default: return "网络错误（代码 " + std::to_string(e) + "）";
    }
}
inline void local_close(int fd) { if (fd >= 0) ::_close(fd); }
inline std::int64_t local_read(int fd, void* p, std::size_t n) { return ::_read(fd, p, static_cast<unsigned>(n)); }
inline std::int64_t local_write(int fd, const void* p, std::size_t n) { return ::_write(fd, p, static_cast<unsigned>(n)); }
inline int local_sync(int fd) { return ::_commit(fd); }
#else
using SockLen = socklen_t;
constexpr Socket invalid_socket = -1;
inline bool socket_valid(Socket s) { return s >= 0; }
inline void close_socket(Socket s) { if (s >= 0) ::close(s); }
inline int socket_error() { return errno; }
inline bool socket_would_block(int e) { return e == EINPROGRESS || e == EAGAIN || e == EWOULDBLOCK; }
inline bool socket_not_connected(int e) { return e == ENOTCONN; }
inline void set_nonblocking(Socket s) { const int flags = ::fcntl(s, F_GETFL, 0); if (flags >= 0) ::fcntl(s, F_SETFL, flags | O_NONBLOCK); }
inline std::string socket_error_text(int e) {
    switch (e) {
    case ECONNREFUSED: return "连接被拒绝";
    case ETIMEDOUT: return "连接超时";
    case ENETUNREACH: return "网络不可达";
    case EHOSTUNREACH: return "主机不可达";
    case EACCES: return "权限不足";
    case EADDRINUSE: return "地址已被占用";
    case ENOTCONN: return "连接未建立";
    case ECONNRESET: return "连接被重置";
    default: return "网络错误（代码 " + std::to_string(e) + "）";
    }
}
inline void local_close(int fd) { if (fd >= 0) ::close(fd); }
inline std::int64_t local_read(int fd, void* p, std::size_t n) { return ::read(fd, p, n); }
inline std::int64_t local_write(int fd, const void* p, std::size_t n) { return ::write(fd, p, n); }
inline int local_sync(int fd) { return ::fsync(fd); }
#endif
std::string dns_error_text(int e) {
#ifdef EAI_AGAIN
    if (e == EAI_AGAIN) return "域名解析服务暂时不可用";
#endif
#ifdef EAI_FAIL
    if (e == EAI_FAIL) return "域名解析失败";
#endif
#ifdef EAI_NONAME
    if (e == EAI_NONAME) return "找不到该主机";
#endif
#ifdef EAI_MEMORY
    if (e == EAI_MEMORY) return "域名解析内存不足";
#endif
#ifdef EAI_SYSTEM
    if (e == EAI_SYSTEM) return "系统域名解析错误";
#endif
    return "域名解析错误（代码 " + std::to_string(e) + "）";
}
std::string ssh_error_text(int rc) {
    switch (rc) {
    case LIBSSH2_ERROR_SOCKET_NONE: return "网络套接字错误";
    case LIBSSH2_ERROR_BANNER_RECV: return "无法读取服务器标识";
    case LIBSSH2_ERROR_BANNER_SEND: return "无法发送客户端标识";
    case LIBSSH2_ERROR_INVALID_MAC: return "消息校验失败";
    case LIBSSH2_ERROR_KEX_FAILURE: return "密钥交换失败";
    case LIBSSH2_ERROR_ALLOC: return "内存分配失败";
    case LIBSSH2_ERROR_SOCKET_SEND: return "网络发送失败";
    case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE: return "密钥交换失败";
    case LIBSSH2_ERROR_TIMEOUT: return "连接超时";
    case LIBSSH2_ERROR_HOSTKEY_INIT: return "主机密钥初始化失败";
    case LIBSSH2_ERROR_HOSTKEY_SIGN: return "主机密钥签名验证失败";
    case LIBSSH2_ERROR_DECRYPT: return "数据解密失败";
    case LIBSSH2_ERROR_SOCKET_DISCONNECT: return "网络连接已断开";
    case LIBSSH2_ERROR_PROTO: return "SSH 协议错误";
    case LIBSSH2_ERROR_PASSWORD_EXPIRED: return "密码已过期";
    case LIBSSH2_ERROR_FILE: return "文件操作失败";
    case LIBSSH2_ERROR_METHOD_NOT_SUPPORTED: return "服务器不支持该认证方式";
    case LIBSSH2_ERROR_AUTHENTICATION_FAILED: return "认证失败，请检查用户名和凭据";
    case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED: return "公钥未通过服务器验证";
    case LIBSSH2_ERROR_CHANNEL_OUTOFORDER: return "通道顺序错误";
    case LIBSSH2_ERROR_CHANNEL_FAILURE: return "通道操作失败";
    case LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED: return "通道请求被服务器拒绝";
    case LIBSSH2_ERROR_CHANNEL_UNKNOWN: return "服务器返回未知通道";
    case LIBSSH2_ERROR_CHANNEL_WINDOW_EXCEEDED: return "通道窗口超出限制";
    case LIBSSH2_ERROR_CHANNEL_PACKET_EXCEEDED: return "通道数据包超出限制";
    case LIBSSH2_ERROR_CHANNEL_CLOSED: return "通道已关闭";
    case LIBSSH2_ERROR_SFTP_PROTOCOL: return "文件传输协议错误";
    case LIBSSH2_ERROR_REQUEST_DENIED: return "请求被服务器拒绝";
    case LIBSSH2_ERROR_INVAL: return "SSH 参数无效";
    case LIBSSH2_ERROR_INVALID_POLL_TYPE: return "网络等待类型无效";
    case LIBSSH2_ERROR_PUBLICKEY_PROTOCOL: return "公钥协议错误";
    default: return "SSH 错误（代码 " + std::to_string(rc) + "）";
    }
}
void wipe(std::string& s) noexcept {
    volatile char* p = s.empty() ? nullptr : s.data();
    for (std::size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}
void wipe_auth(AuthConfig& a) noexcept { wipe(a.password); wipe(a.passphrase); }
struct CredentialsGuard { AuthConfig& auth; ~CredentialsGuard() { wipe_auth(auth); } };
struct StringGuard { std::string& text; ~StringGuard() { wipe(text); } };
struct Failure { SessionError error; };
[[noreturn]] void fail(ErrorCode code, std::string text, int native = 0) {
    throw Failure{{code, native, std::move(text)}};
}
bool fill_random(unsigned char* out, std::size_t n) {
#ifdef _WIN32
    return ::BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < n) {
        auto got = ::read(fd, out + done, n - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ::close(fd); return false; }
        done += static_cast<std::size_t>(got);
    }
    ::close(fd);
    return true;
#endif
}
std::string base64(const unsigned char* p, std::size_t n) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned bits = 0, count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        bits = (bits << 8) | p[i]; count += 8;
        while (count >= 6) { count -= 6; out += alphabet[(bits >> count) & 63]; }
    }
    if (count) out += alphabet[(bits << (6 - count)) & 63];
    return out;
}
std::string key_algorithm(const std::vector<std::uint8_t>& key) {
    if (key.size() < 4) return "未知";
    std::size_t n = (std::size_t(key[0]) << 24) | (std::size_t(key[1]) << 16) |
                    (std::size_t(key[2]) << 8) | key[3];
    if (n > key.size() - 4 || n > 128) return "未知";
    return std::string(reinterpret_cast<const char*>(key.data() + 4), n);
}
struct Address { sockaddr_storage addr{}; socklen_t length{}; int family{}; };
struct ResolveResult {
    std::mutex mutex;
    bool done{};
    int error{};
    std::vector<Address> addresses;
};
// libc DNS can block without a cancellable fd (including musl). Isolate it from
// the SSH owner, retaining no session pointers/fds. At most four process-wide
// helper calls may be outstanding; cancellation never waits for libc DNS.
std::atomic<unsigned> resolver_count{};
}

struct SshSession::Impl {
    mutable std::mutex mutex;
    std::mutex lifecycle;
    SessionSnapshot view;
    std::deque<SessionEvent> events;
    std::deque<std::string> output;
    std::size_t output_offset{};
    std::deque<std::string> input;
    std::size_t input_offset{};
    std::size_t input_capacity{}, output_capacity{}, event_capacity{64};
    std::optional<std::pair<int,int>> pending_size;
    std::optional<bool> trust_answer;
    std::uint64_t next_request{};
    std::deque<std::pair<std::uint64_t, TaskRequest>> jobs;
    std::unordered_set<std::uint64_t> task_cancels;
    std::size_t task_capacity{32};
    std::uint64_t next_task{};
    bool running{};
    std::atomic<bool> cancelled{};
    std::thread owner;
#ifndef _WIN32
    int wake_read{-1}, wake_write{-1};
#endif
    Socket socket{invalid_socket};
    LIBSSH2_SESSION* session{};
    LIBSSH2_CHANNEL* channel{};
    LIBSSH2_SFTP* sftp{};
    struct Job {
        std::uint64_t id{};
        TaskRequest request;
        TaskSnapshot result;
        Clock::time_point deadline{};
        int phase{}, fd{-1}, dirfd{-1};
        std::string local_name;
        LIBSSH2_SFTP_HANDLE* handle{};
        LIBSSH2_CHANNEL* exec{};
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
#ifdef _WIN32
        struct _stat64 source{};
#else
        struct stat source{};
#endif
#ifdef _WIN32
        std::filesystem::path local_directory;
#endif
        std::string temp;
        std::string buffer;
        std::size_t offset{};
        bool finishing{}, published{}, temp_created{}, pending_call{};
    };
    std::optional<Job> active;
#ifndef _WIN32
    int agent_socket{-1};
    Clock::time_point agent_deadline{};
    std::optional<SessionError> agent_error;
    std::vector<std::uint8_t> agent_key;
#endif

    Impl() {
#ifndef _WIN32
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
            throw std::runtime_error("SSH 唤醒管道创建失败");
        wake_read = fds[0]; wake_write = fds[1];
#else
        // Winsock is process-wide: initialize once and never tear it down while
        // another session may still own sockets.
        static std::once_flag wsa_once;
        std::call_once(wsa_once, [] {
            WSADATA data{};
            if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
                throw std::runtime_error("SSH 网络初始化失败");
        });
#endif
    }
    ~Impl() {
        cancelled = true; wake();
        if (owner.joinable()) owner.join();
#ifndef _WIN32
        ::close(wake_read); ::close(wake_write);
#endif
    }
    void wake() noexcept {
#ifndef _WIN32
        const char c = 1;
        // Full pipe is already a pending wakeup.
        ssize_t n;
        do { n = ::write(wake_write, &c, 1); } while (n < 0 && errno == EINTR);
#endif
    }
    void emit_locked(SessionEvent e) {
        if (events.size() >= event_capacity) { events.pop_front(); ++view.dropped_events; }
        events.push_back(std::move(e));
    }
    void state(SessionState s) {
        std::lock_guard lock(mutex);
        view.state = s;
        emit_locked({EventType::StateChanged, s, {}, {}});
    }
    void check_cancel() {
        if (cancelled.load()) fail(ErrorCode::Cancelled, "SSH 操作已取消");
    }
    void wait_socket(Clock::time_point deadline, short explicit_events = 0, bool watch_socket = true) {
        check_cancel();
        const auto now = Clock::now();
        if (now >= deadline) fail(ErrorCode::Timeout, "SSH 操作超时");
        short wanted = explicit_events;
        if (!wanted && session) {
            const int dirs = libssh2_session_block_directions(session);
            if (dirs & LIBSSH2_SESSION_BLOCK_INBOUND) wanted |= POLLIN;
            if (dirs & LIBSSH2_SESSION_BLOCK_OUTBOUND) wanted |= POLLOUT;
        }
        if (!wanted) wanted = POLLIN;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        int timeout = static_cast<int>(std::clamp<std::int64_t>(ms + 1, 1, INT_MAX));
#ifdef _WIN32
        // There is no POSIX self-pipe here; cap each wait so cancellation,
        // queued input and shutdown are observed promptly.
        timeout = std::min(timeout, 20);
        if (!watch_socket || !socket_valid(socket)) {
            ::Sleep(static_cast<DWORD>(timeout));
            check_cancel();
            if (Clock::now() >= deadline) fail(ErrorCode::Timeout, "SSH 操作超时");
            return;
        }
        WSAPOLLFD watch{};
        watch.fd = socket; watch.events = wanted;
        const int rc = ::WSAPoll(&watch, 1, timeout);
        if (rc == SOCKET_ERROR) {
            const int error = socket_error();
            fail(ErrorCode::Network, "网络等待失败：" + socket_error_text(error), error);
        }
        check_cancel();
        if (watch.revents & POLLNVAL) fail(ErrorCode::Network, "SSH 连接已失效", EBADF);
        // POLLHUP/POLLERR are passed back to libssh2/getsockopt for an exact error.
        if (!rc && Clock::now() >= deadline) fail(ErrorCode::Timeout, "SSH 操作超时");
#else
        pollfd fds[2]{{wake_read, POLLIN, 0}, {watch_socket ? socket : -1, wanted, 0}};
        int rc;
        do { rc = ::poll(fds, 2, timeout); } while (rc < 0 && errno == EINTR && !cancelled);
        if (rc < 0) fail(ErrorCode::Network, std::string("网络等待失败：") + socket_error_text(errno), errno);
        if (fds[0].revents) { char bytes[128]; while (::read(wake_read, bytes, sizeof(bytes)) > 0) {} }
        check_cancel();
        if (fds[1].revents & POLLNVAL) fail(ErrorCode::Network, "SSH 连接已失效", EBADF);
        // POLLHUP/POLLERR are passed back to libssh2/getsockopt for an exact error.
        if (!rc && Clock::now() >= deadline) fail(ErrorCode::Timeout, "SSH 操作超时");
#endif
    }
    template<class F> int retry(F&& fn, Clock::time_point deadline, ErrorCode code, const char* operation) {
        for (;;) {
            check_cancel();
            if (Clock::now() >= deadline) fail(ErrorCode::Timeout, std::string(operation) + "超时");
            const int rc = fn();
            if (rc == LIBSSH2_ERROR_EAGAIN) { wait_socket(deadline); continue; }
            if (rc < 0) ssh_error(code, operation, rc);
            return rc;
        }
    }
    [[noreturn]] void ssh_error(ErrorCode code, const char* operation, int rc) {
        // libssh2's own message is English; map protocol codes to Chinese so no
        // English diagnostic reaches the interface.
        if (rc == LIBSSH2_ERROR_SOCKET_RECV || rc == LIBSSH2_ERROR_SOCKET_SEND ||
            rc == LIBSSH2_ERROR_SOCKET_DISCONNECT) code = ErrorCode::Network;
        fail(code, std::string(operation) + "失败：" + ssh_error_text(rc), rc);
    }
    std::vector<Address> resolve(const ConnectOptions& options, Clock::time_point deadline) {
        // Numeric addresses do not need a resolver helper.
        auto result = std::make_shared<ResolveResult>();
        const std::string host = options.host;
        const std::string service = std::to_string(options.port);
        auto work = [result, host, service] {
            addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
            addrinfo* list = nullptr;
            int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list);
            std::vector<Address> addresses;
            if (!rc) {
                for (auto* p = list; p; p = p->ai_next) {
                    if (p->ai_addrlen > sizeof(sockaddr_storage)) continue;
                    Address a; a.family = p->ai_family; a.length = p->ai_addrlen;
                    std::memcpy(&a.addr, p->ai_addr, p->ai_addrlen); addresses.push_back(a);
                }
                ::freeaddrinfo(list);
            }
            { std::lock_guard lock(result->mutex); result->error = rc;
              result->addresses = std::move(addresses); result->done = true; }
        };
        in_addr v4{}; in6_addr v6{};
        if (::inet_pton(AF_INET, host.c_str(), &v4) == 1 || ::inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
            work();
        } else {
            unsigned previous = resolver_count.fetch_add(1);
            if (previous >= 4) { --resolver_count; fail(ErrorCode::Resolve, "域名解析并发已满，请稍后重试"); }
            try {
                std::thread([work = std::move(work), result] {
                    try { work(); } catch (...) {
                        std::lock_guard lock(result->mutex); result->error = EAI_MEMORY; result->done = true;
                    }
                    --resolver_count;
                }).detach();
            } catch (...) { --resolver_count; throw; }
        }
        for (;;) {
            check_cancel();
            if (Clock::now() >= deadline) fail(ErrorCode::Timeout, "域名解析超时");
            {
                std::lock_guard lock(result->mutex);
                if (result->done) {
                    if (result->error) fail(ErrorCode::Resolve, std::string("域名解析失败：") + dns_error_text(result->error), result->error);
                    if (result->addresses.empty()) fail(ErrorCode::Resolve, "域名解析未返回可用地址");
                    return std::move(result->addresses);
                }
            }
            const auto slice = std::min(deadline, Clock::now() + std::chrono::milliseconds(25));
            // A DNS polling slice is not the overall operation timeout.
            try { wait_socket(slice, 0, false); } catch (const Failure& e) {
                if (e.error.code != ErrorCode::Timeout) throw;
            }
        }
    }
    void tcp_connect(const ConnectOptions& options, Clock::time_point deadline) {
        const auto addresses = resolve(options, deadline);
        int last_error = ECONNREFUSED;
        for (const auto& a : addresses) {
            check_cancel();
            socket = ::socket(a.family, SOCK_STREAM, IPPROTO_TCP);
            if (!socket_valid(socket)) { last_error = socket_error(); continue; }
            set_nonblocking(socket);
            int rc = ::connect(socket, reinterpret_cast<const sockaddr*>(&a.addr), a.length);
            if (rc == 0) return;
            last_error = socket_error();
            if (socket_would_block(last_error)) {
                // Per-address slice allows a dead IPv6 address to fall back to IPv4.
                auto slice = std::min(deadline, Clock::now() + std::chrono::seconds(3));
                for (;;) {
                    try { wait_socket(slice, POLLOUT); }
                    catch (const Failure& e) {
                        if (e.error.code != ErrorCode::Timeout || Clock::now() >= deadline) throw;
                        last_error = ETIMEDOUT; break;
                    }
                    SockLen n = sizeof(last_error);
                    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&last_error), &n) < 0)
                        last_error = socket_error();
                    if (!last_error) {
                        sockaddr_storage peer{}; SockLen length = sizeof(peer);
                        if (::getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) == 0) return;
                        last_error = socket_error();
                        if (socket_not_connected(last_error)) continue;
                    }
                    break;
                }
            }
            close_socket(socket); socket = invalid_socket;
        }
        fail(last_error == ETIMEDOUT ? ErrorCode::Timeout : ErrorCode::Network,
             std::string("无法连接到服务器：") + socket_error_text(last_error), last_error);
    }
    void verify_host(const ConnectOptions& options) {
        std::size_t size = 0; int type = 0;
        const char* key = libssh2_session_hostkey(session, &size, &type);
        if (!key || !size) fail(ErrorCode::Protocol, "服务器未提供主机密钥");
        std::vector<std::uint8_t> raw(reinterpret_cast<const std::uint8_t*>(key), reinterpret_cast<const std::uint8_t*>(key) + size);
        if (!options.expected_host_key.empty()) {
            if (raw != options.expected_host_key) fail(ErrorCode::HostKeyMismatch, "服务器主机密钥与记录不一致，已拒绝认证");
            return;
        }
        const auto* hash = reinterpret_cast<const unsigned char*>(libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256));
        if (!hash) fail(ErrorCode::Protocol, "无法计算主机密钥指纹");
        HostKeyRequest request;
        request.host = options.host; request.port = options.port; request.raw_key = std::move(raw);
        request.algorithm = key_algorithm(request.raw_key);
        request.sha256_fingerprint = "SHA256:" + base64(hash, 32);
        {
            std::lock_guard lock(mutex);
            request.id = ++next_request; trust_answer.reset();
            view.state = SessionState::AwaitingHostKey; view.host_key_request = request;
            emit_locked({EventType::HostKeyConfirmation, view.state, {}, request});
        }
        const auto deadline = Clock::now() + options.trust_timeout;
        for (;;) {
            check_cancel();
            {
                std::lock_guard lock(mutex);
                if (trust_answer.has_value()) {
                    bool accepted = *trust_answer;
                    trust_answer.reset(); view.host_key_request.reset();
                    if (!accepted) fail(ErrorCode::HostKeyRejected, "已拒绝服务器主机密钥，认证中止");
                    return;
                }
            }
            // No protocol traffic/authentication while trust is pending.
            wait_socket(deadline, 0, false);
        }
    }
    static void put_u32(std::vector<std::uint8_t>& out, std::uint32_t n) {
        out.push_back(n >> 24); out.push_back(n >> 16); out.push_back(n >> 8); out.push_back(n);
    }
    static std::uint32_t get_u32(const std::vector<std::uint8_t>& in, std::size_t& at) {
        if (at > in.size() || in.size() - at < 4) fail(ErrorCode::Authentication, "SSH 代理 响应格式错误");
        std::uint32_t n = (std::uint32_t(in[at]) << 24) | (std::uint32_t(in[at+1]) << 16) |
                          (std::uint32_t(in[at+2]) << 8) | in[at+3]; at += 4; return n;
    }
    static std::vector<std::uint8_t> get_string(const std::vector<std::uint8_t>& in, std::size_t& at) {
        const auto n = get_u32(in, at);
        if (at > in.size() || n > in.size() - at) fail(ErrorCode::Authentication, "SSH 代理 字符串格式错误");
        std::vector<std::uint8_t> out(in.begin() + at, in.begin() + at + n); at += n; return out;
    }
#ifndef _WIN32
    void agent_wait(short events) {
        check_cancel();
        if (Clock::now() >= agent_deadline) fail(ErrorCode::Timeout, "SSH 代理 操作超时");
        pollfd fds[2]{{wake_read, POLLIN, 0}, {agent_socket, events, 0}};
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(agent_deadline - Clock::now()).count();
        int rc = ::poll(fds, 2, static_cast<int>(std::clamp<std::int64_t>(ms + 1, 1, INT_MAX)));
        if (rc < 0 && errno != EINTR) fail(ErrorCode::Authentication, "等待 SSH 代理 响应失败", errno);
        if (fds[0].revents) { char buffer[128]; while (::read(wake_read, buffer, sizeof(buffer)) > 0) {} }
        check_cancel();
    }
    void agent_transfer(std::uint8_t* data, std::size_t n, bool sending) {
        while (n) {
            check_cancel();
            if (Clock::now() >= agent_deadline) fail(ErrorCode::Timeout, "SSH 代理 操作超时");
            auto rc = sending ? ::send(agent_socket, data, n, MSG_NOSIGNAL) : ::recv(agent_socket, data, n, 0);
            if (rc > 0) { data += rc; n -= static_cast<std::size_t>(rc); continue; }
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { agent_wait(sending ? POLLOUT : POLLIN); continue; }
            fail(ErrorCode::Authentication, "SSH 代理 连接已关闭或失败", rc < 0 ? errno : 0);
        }
    }
    std::vector<std::uint8_t> agent_request(std::vector<std::uint8_t> body) {
        constexpr std::size_t max_agent_message = 1024 * 1024;
        if (body.size() > max_agent_message) fail(ErrorCode::Authentication, "SSH 代理 请求过大");
        std::vector<std::uint8_t> packet; put_u32(packet, static_cast<std::uint32_t>(body.size()));
        packet.insert(packet.end(), body.begin(), body.end()); agent_transfer(packet.data(), packet.size(), true);
        std::vector<std::uint8_t> prefix(4); agent_transfer(prefix.data(), 4, false);
        std::size_t at = 0; auto n = get_u32(prefix, at);
        if (!n || n > max_agent_message) fail(ErrorCode::Authentication, "SSH 代理 响应过大或为空");
        std::vector<std::uint8_t> response(n); agent_transfer(response.data(), n, false); return response;
    }
    static int agent_sign(LIBSSH2_SESSION*, unsigned char** signature, std::size_t* signature_length,
                          const unsigned char* data, std::size_t data_length, void** abstract) noexcept {
        auto& self = *static_cast<Impl*>(*abstract);
        try {
            std::vector<std::uint8_t> request{13}; // SSH2_AGENTC_SIGN_REQUEST
            put_u32(request, static_cast<std::uint32_t>(self.agent_key.size()));
            request.insert(request.end(), self.agent_key.begin(), self.agent_key.end());
            put_u32(request, static_cast<std::uint32_t>(data_length)); request.insert(request.end(), data, data + data_length);
            // Parse the exact algorithm field, not a substring (the user name
            // and session identifier are untrusted binary strings too).
            std::vector<std::uint8_t> signed_packet(data, data + data_length);
            std::size_t field = 0; (void)get_string(signed_packet, field);
            if (field >= signed_packet.size() || signed_packet[field++] != 50)
                fail(ErrorCode::Authentication, "SSH 代理 认证数据包无效");
            (void)get_string(signed_packet, field); (void)get_string(signed_packet, field); (void)get_string(signed_packet, field);
            if (field >= signed_packet.size() || signed_packet[field++] != 1)
                fail(ErrorCode::Authentication, "SSH 代理 签名标志无效");
            auto algorithm_bytes = get_string(signed_packet, field);
            std::string algorithm(algorithm_bytes.begin(), algorithm_bytes.end());
            const std::uint32_t flags = algorithm == "rsa-sha2-512" ? 4 : algorithm == "rsa-sha2-256" ? 2 : 0;
            put_u32(request, flags);
            auto response = self.agent_request(std::move(request));
            if (response[0] != 14) fail(ErrorCode::Authentication, "SSH 代理 拒绝签名请求");
            std::size_t at = 1; auto signature_blob = get_string(response, at);
            std::size_t sig_at = 0; auto returned_algorithm = get_string(signature_blob, sig_at);
            if (returned_algorithm != algorithm_bytes) fail(ErrorCode::Authentication, "SSH 代理 返回了不同的签名算法");
            auto sig = get_string(signature_blob, sig_at);
            if (sig.empty()) fail(ErrorCode::Authentication, "SSH 代理 返回空签名");
            auto* memory = static_cast<unsigned char*>(secure_alloc(sig.size(), nullptr));
            if (!memory) fail(ErrorCode::Internal, "无法分配 SSH 代理 签名内存");
            std::memcpy(memory, sig.data(), sig.size()); *signature = memory; *signature_length = sig.size(); return 0;
        } catch (const Failure& e) { self.agent_error = e.error; }
          catch (...) { self.agent_error = SessionError{ErrorCode::Internal, 0, "SSH 代理 签名失败"}; }
        return -1;
    }
    void authenticate_agent(const ConnectOptions& options, Clock::time_point deadline) {
        agent_deadline = deadline; agent_error.reset();
        const char* path = std::getenv("SSH_AUTH_SOCK");
        if (!path || !*path) fail(ErrorCode::Authentication, "未设置 SSH_AUTH_SOCK");
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        if (std::strlen(path) >= sizeof(address.sun_path)) fail(ErrorCode::Authentication, "SSH 代理 套接字路径过长");
        std::strcpy(address.sun_path, path);
        agent_socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (agent_socket < 0) fail(ErrorCode::Authentication, "创建 SSH 代理 套接字失败", errno);
        if (::connect(agent_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            if (errno != EINPROGRESS) fail(ErrorCode::Authentication, "连接 SSH 代理 失败", errno);
            agent_wait(POLLOUT); int error = 0; socklen_t size = sizeof(error);
            if (::getsockopt(agent_socket, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
                fail(ErrorCode::Authentication, "连接 SSH 代理 失败", error ? error : errno);
        }
        auto response = agent_request({11}); // SSH2_AGENTC_REQUEST_IDENTITIES
        if (response[0] != 12) fail(ErrorCode::Authentication, "SSH 代理 拒绝列出身份");
        std::size_t at = 1; auto count = get_u32(response, at);
        if (count > 2048) fail(ErrorCode::Authentication, "SSH 代理 身份数量过多");
        bool success = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            agent_key = get_string(response, at); (void)get_string(response, at);
            void* context = this;
            int rc;
            do {
                check_cancel();
                if (Clock::now() >= deadline) fail(ErrorCode::Timeout, "SSH 代理 认证超时");
                rc = libssh2_userauth_publickey(session, options.user.c_str(), agent_key.data(), agent_key.size(), agent_sign, &context);
                if (agent_error) throw Failure{*agent_error};
                if (rc == LIBSSH2_ERROR_EAGAIN) wait_socket(deadline);
            } while (rc == LIBSSH2_ERROR_EAGAIN);
            if (!rc) { success = true; break; }
        }
        ::close(agent_socket); agent_socket = -1; agent_key.clear();
        if (!success) fail(ErrorCode::Authentication, "没有 SSH 代理 身份被接受");
    }
#endif
    // Keep malloc-compatible addresses: crypto backends may release libssh2 buffers.
    static void* secure_alloc(std::size_t n, void**) { return std::malloc(n); }
    static void secure_free(void* data, void**) { std::free(data); }
    static void* secure_realloc(void* data, std::size_t n, void**) { return std::realloc(data, n); }
    void authenticate(ConnectOptions& options) {
        state(SessionState::Authenticating);
        const auto deadline = Clock::now() + options.operation_timeout;
        auto& auth = options.auth;
        CredentialsGuard guard{auth};
        if (auth.method == AuthMethod::Password) {
            retry([&] { return libssh2_userauth_password_ex(session, options.user.c_str(), static_cast<unsigned>(options.user.size()),
                auth.password.c_str(), static_cast<unsigned>(auth.password.size()), nullptr); }, deadline, ErrorCode::Authentication, "密码认证");
        } else if (auth.method == AuthMethod::Key) {
            if (auth.public_key_file.empty()) {
                std::error_code ec;
                auto candidate = auth.private_key_file + ".pub";
                if (std::filesystem::exists(candidate, ec) && !ec) auth.public_key_file = std::move(candidate);
            }
            retry([&] { return libssh2_userauth_publickey_fromfile_ex(session, options.user.c_str(), static_cast<unsigned>(options.user.size()),
                auth.public_key_file.empty() ? nullptr : auth.public_key_file.c_str(), auth.private_key_file.c_str(),
                auth.passphrase.empty() ? nullptr : auth.passphrase.c_str()); }, deadline, ErrorCode::Authentication, "公钥认证");
        } else {
#ifdef _WIN32
            fail(ErrorCode::Authentication, "Windows 下不支持 SSH 代理 认证");
#else
            // libssh2's built-in Unix agent uses blocking IPC. Implement the
            // small agent protocol here so deadlines/cancellation also cover
            // a stalled or malicious local agent, with all I/O on this owner.
            authenticate_agent(options, deadline);
#endif
        }
        if (!libssh2_userauth_authenticated(session)) fail(ErrorCode::Authentication, "服务器未完成会话认证");
    }
    void open_shell(const ConnectOptions& options) {
        const auto deadline = Clock::now() + options.operation_timeout;
        for (;;) {
            check_cancel();
            channel = libssh2_channel_open_session(session);
            if (channel) break;
            int rc = libssh2_session_last_errno(session);
            if (rc != LIBSSH2_ERROR_EAGAIN) ssh_error(ErrorCode::Protocol, "打开 SSH 通道", rc);
            wait_socket(deadline);
        }
        retry([&] { return libssh2_channel_request_pty_ex(channel, "xterm-256color", 14, nullptr, 0,
            options.columns, options.rows, 0, 0); }, deadline, ErrorCode::Protocol, "申请终端 PTY");
        retry([&] { return libssh2_channel_shell(channel); }, deadline, ErrorCode::Protocol, "启动远端命令行");
        libssh2_keepalive_config(session, 1, static_cast<unsigned>(options.keepalive_interval.count()));
        state(SessionState::Connected);
    }
    void publish_job(Job& j) {
        std::lock_guard lock(mutex);
        for (auto& t : view.tasks) if (t.id == j.id) {
            if (j.finishing) t = std::move(j.result);
            else {
                // Progress snapshots never copy growing output/directory vectors.
                // Full payload is moved exactly once after cleanup completes.
                t.id = j.id; t.kind = j.result.kind; t.state = j.result.state;
                t.transferred = j.result.transferred; t.total = j.result.total;
                t.output_truncated = j.result.output_truncated;
            }
            break;
        }
    }
    bool task_rc(int rc, const char* what) {
        if (rc == LIBSSH2_ERROR_EAGAIN) return false;
        if (rc < 0) {
            const auto status=sftp&&rc==LIBSSH2_ERROR_SFTP_PROTOCOL?libssh2_sftp_last_error(sftp):0;
            const char* detail=nullptr;
            switch(status) {
            case LIBSSH2_FX_NO_SUCH_FILE: case LIBSSH2_FX_NO_SUCH_PATH: detail="路径不存在"; break;
            case LIBSSH2_FX_PERMISSION_DENIED: detail="权限不足"; break;
            case LIBSSH2_FX_FILE_ALREADY_EXISTS: detail="目标已存在"; break;
            case LIBSSH2_FX_OP_UNSUPPORTED: detail="服务器不支持此操作"; break;
            }
            fail(ErrorCode::Protocol,std::string(what)+"失败（"+
                (detail?std::string(detail):status?"SFTP 状态 "+std::to_string(status):"错误代码 "+std::to_string(rc))+"）",rc);
        }
        return true;
    }
    FileInfo file_info(std::string name, const LIBSSH2_SFTP_ATTRIBUTES& a) {
        return {std::move(name), a.flags&LIBSSH2_SFTP_ATTR_SIZE?a.filesize:0,
            a.flags&LIBSSH2_SFTP_ATTR_ACMODTIME?a.mtime:0, a.flags&LIBSSH2_SFTP_ATTR_PERMISSIONS?a.permissions:0};
    }
    void finish_job(Job& j, TaskState state, std::string error = {}) {
        j.result.state = state; j.result.error = std::move(error); j.finishing = true;
        j.deadline = Clock::now(); // Independent five-second cleanup grace.
    }
    // Exactly one protocol step (or <=32KiB transfer) per terminal pass. EAGAIN
    // state and buffer addresses survive until that same operation is retried.
    bool task_step() {
        bool progressed = task_step_once();
        if (active) active->pending_call = !progressed && !(active->request.kind == TaskKind::Exec && active->phase == 2);
        return progressed;
    }
    bool task_step_once() {
        if (!active) {
            std::lock_guard lock(mutex);
            if (jobs.empty()) return false;
            auto item = std::move(jobs.front()); jobs.pop_front();
            active.emplace(); auto& j = *active;
            j.id = item.first; j.request = std::move(item.second);
            j.result.id = j.id; j.result.kind = j.request.kind; j.result.state = TaskState::Running;
            j.deadline = Clock::now() + j.request.timeout;
            for (auto& t : view.tasks) if (t.id == j.id) t = j.result;
        }
        auto& j = *active;
        bool stop;
        { std::lock_guard lock(mutex); stop = task_cancels.count(j.id); }
        if (stop && j.pending_call) j.deadline = std::min(j.deadline, Clock::now());
        if (j.pending_call && Clock::now() > j.deadline + std::chrono::seconds(5))
            fail(ErrorCode::Timeout, "辅助协议操作超时，正在关闭连接");
        if (!j.finishing && !j.pending_call && (stop || Clock::now() >= j.deadline)) {
            finish_job(j, stop ? TaskState::Cancelled : TaskState::Failed, stop ? "任务已取消" : "任务超时");
        }
        if (j.finishing) {
            // Never abandon an in-progress channel/SFTP close on EAGAIN.
            // Cleanup gets its own bounded grace; failure closes the transport,
            // rather than reusing corrupt libssh2 operation state.
            if (Clock::now() > j.deadline + std::chrono::seconds(5))
                fail(ErrorCode::Timeout, "辅助任务清理超时，连接已安全关闭");
            if (j.fd >= 0) { local_close(j.fd); j.fd = -1; }
            if (j.handle) {
                int rc = libssh2_sftp_close_handle(j.handle);
                if (rc == LIBSSH2_ERROR_EAGAIN) return false;
                j.handle = nullptr; return true;
            }
            if (j.exec) {
                int rc = libssh2_channel_free(j.exec);
                if (rc == LIBSSH2_ERROR_EAGAIN) return false;
                j.exec = nullptr; return true;
            }
            if (j.temp_created && !j.published) {
                if (j.request.kind == TaskKind::Download) {
#ifdef _WIN32
                    std::error_code ignored;
                    std::filesystem::remove(j.local_directory / j.temp, ignored);
#else
                    ::unlinkat(j.dirfd, j.temp.c_str(), 0);
#endif
                }
                else {
                    int rc = libssh2_sftp_unlink(sftp, j.temp.c_str());
                    if (rc == LIBSSH2_ERROR_EAGAIN) return false;
                    if (rc < 0) j.result.error += "；远端临时文件可能残留：" + j.temp;
                }
                j.temp_created = false; return true;
            }
            { std::lock_guard lock(mutex); task_cancels.erase(j.id); }
#ifndef _WIN32
            if (j.dirfd >= 0) ::close(j.dirfd);
#endif
            publish_job(j); active.reset(); return true;
        }
        try {
            auto& r = j.request;
            if (r.kind == TaskKind::Exec) {
                if (j.phase == 0) {
                    j.exec = libssh2_channel_open_session(session);
                    if (!j.exec) { task_rc(libssh2_session_last_errno(session), "打开远程命令通道"); return false; }
                    j.phase = 1; return true;
                }
                if (j.phase == 1) {
                    if (!task_rc(libssh2_channel_exec(j.exec, r.command.c_str()), "执行远程命令")) return false;
                    j.phase = 2; return true;
                }
                if (j.phase == 2) {
                    bool progress = false;
                    for (int stream = 0; stream < 2; ++stream) {
                        char data[16384]; auto n = libssh2_channel_read_ex(j.exec, stream, data, sizeof(data));
                        if (n == LIBSSH2_ERROR_EAGAIN) continue;
                        if (n < 0) task_rc(static_cast<int>(n), "读取远程命令输出");
                        if (n > 0) {
                            progress = true; auto& text = stream ? j.result.stderr_text : j.result.stdout_text;
                            auto count = std::min(static_cast<std::size_t>(n), r.output_limit - text.size());
                            text.append(data, count); if (count < static_cast<std::size_t>(n)) j.result.output_truncated = true;
                        }
                    }
                    publish_job(j);
                    if (!progress && libssh2_channel_eof(j.exec)) j.phase = 3;
                    return progress || j.phase == 3;
                }
                if (!task_rc(libssh2_channel_close(j.exec), "关闭远程命令通道")) return false;
                j.result.exit_status = libssh2_channel_get_exit_status(j.exec);
                finish_job(j, TaskState::Succeeded); return true;
            }
            if (!sftp) {
                sftp = libssh2_sftp_init(session);
                if (!sftp) { task_rc(libssh2_session_last_errno(session), "初始化文件传输"); return false; }
                return true;
            }
            if (r.kind == TaskKind::Mkdir || r.kind == TaskKind::Rename || r.kind == TaskKind::Remove || r.kind == TaskKind::Chmod) {
                int rc;
                if (r.kind == TaskKind::Mkdir) rc = libssh2_sftp_mkdir(sftp, r.remote_path.c_str(), r.permissions);
                else if (r.kind == TaskKind::Remove) rc = libssh2_sftp_unlink(sftp, r.remote_path.c_str());
                else if (r.kind == TaskKind::Rename) rc = r.overwrite ?
                    libssh2_sftp_posix_rename(sftp, r.remote_path.c_str(), r.destination_path.c_str()) :
                    libssh2_sftp_rename_ex(sftp, r.remote_path.data(), static_cast<unsigned>(r.remote_path.size()),
                        r.destination_path.data(), static_cast<unsigned>(r.destination_path.size()), 0);
                else { j.attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS; j.attrs.permissions = r.permissions;
                    rc = libssh2_sftp_setstat(sftp, r.remote_path.c_str(), &j.attrs); }
                if (!task_rc(rc, "修改远端文件")) return false;
                finish_job(j, TaskState::Succeeded); return true;
            }
            if (r.kind == TaskKind::Stat) {
                if (!task_rc(libssh2_sftp_lstat(sftp, r.remote_path.c_str(), &j.attrs), "读取远端文件状态")) return false;
                j.result.entries.push_back(file_info(r.remote_path, j.attrs)); finish_job(j, TaskState::Succeeded); return true;
            }
            if (r.kind == TaskKind::List) {
                if (j.phase==0) {
                    char path[16384];
                    int n=libssh2_sftp_realpath(sftp,r.remote_path.c_str(),path,sizeof(path));
                    if (!task_rc(n,"解析远端目录")) return false;
                    if (!n||path[0]!='/'||std::memchr(path,0,n)) fail(ErrorCode::Protocol,"服务器返回的远端路径无效");
                    j.result.listed_path.assign(path,n);
                    j.phase=1; return true;
                }
                if (!j.handle) {
                    j.handle = libssh2_sftp_opendir(sftp, j.result.listed_path.c_str());
                    if (!j.handle) { task_rc(libssh2_session_last_errno(session), "打开远端目录"); return false; }
                    return true;
                }
                j.attrs={};
                char name[4096]; int n = libssh2_sftp_readdir(j.handle, name, sizeof(name), &j.attrs);
                if (n == LIBSSH2_ERROR_EAGAIN) return false;
                task_rc(n, "读取远端目录");
                if (!n) finish_job(j, TaskState::Succeeded);
                else {
                    std::string entry(name, n);
                    if (entry == "." || entry == "..") return true;
                    if (entry.find('/') != std::string::npos || entry.find('\0') != std::string::npos)
                        fail(ErrorCode::Protocol, "远端目录名称不安全");
                    if (j.result.entries.size() >= 4096) { j.result.listing_truncated=true; finish_job(j,TaskState::Succeeded); }
                    else j.result.entries.push_back(file_info(std::move(entry), j.attrs));
                }
                return true;
            }
            bool upload = r.kind == TaskKind::Upload;
            if (j.phase == 0) {
                if (upload) {
#ifdef _WIN32
                    const auto source_path=app::native_file_path(r.local_path);
                    j.fd = ::_wopen(source_path.c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
                    if (j.fd < 0 || ::_fstat64(j.fd, &j.source) || !(j.source.st_mode & _S_IFREG))
                        fail(ErrorCode::Protocol, "上传源必须是可读的普通文件");
                    j.result.total = j.source.st_size;
#else
                    j.fd = ::open(r.local_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                    if (j.fd < 0 || ::fstat(j.fd, &j.source) || !S_ISREG(j.source.st_mode))
                        fail(ErrorCode::Protocol, "上传源必须是可读的普通文件");
                    j.result.total = j.source.st_size;
#endif
                } else {
#ifdef _WIN32
                    const auto local=app::native_file_path(r.local_path);
                    std::error_code ec;
                    const auto parent = std::filesystem::canonical(local.parent_path(), ec);
                    if (ec || !std::filesystem::is_directory(parent))
                        fail(ErrorCode::Protocol, "下载目录必须是真实目录");
                    const DWORD attributes = ::GetFileAttributesW(parent.c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        fail(ErrorCode::Protocol, "下载目录不能是重解析点");
                    j.local_directory = parent;
                    j.local_name = app::utf8_path(local.filename());
                    if (!app::valid_file_leaf(j.local_name,true))
                        fail(ErrorCode::Protocol, "下载路径包含有歧义的组成部分");
                    unsigned char random[16];
                    if (!fill_random(random, sizeof(random))) fail(ErrorCode::Internal, "无法生成下载临时文件名");
                    j.temp = ".jianssh-";
                    constexpr char hex[] = "0123456789abcdef";
                    for (auto byte : random) { j.temp += hex[byte >> 4]; j.temp += hex[byte & 15]; }
                    const auto temp_path = parent / j.temp;
                    // Publish using this exact handle instead of reopening
                    // its pathname after verification.
                    HANDLE file=::CreateFileW(temp_path.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,
                        nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                    if (file==INVALID_HANDLE_VALUE) fail(ErrorCode::Protocol,"无法创建下载临时文件",static_cast<int>(::GetLastError()));
                    j.temp_created = true;
                    j.fd=::_open_osfhandle(reinterpret_cast<intptr_t>(file),_O_WRONLY|_O_BINARY|_O_NOINHERIT);
                    if(j.fd<0){::CloseHandle(file);fail(ErrorCode::Protocol,"无法打开下载临时文件",errno);}
#else
                    // Pin every path component with no-follow dirfds. Ancestor
                    // symlink substitution cannot redirect writes/publication.
                    j.dirfd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    std::size_t at = 1;
                    for (;;) {
                        auto slash = r.local_path.find('/', at);
                        auto component = r.local_path.substr(at, slash == std::string::npos ? slash : slash - at);
                        if (component.empty() || component == "." || component == "..")
                            fail(ErrorCode::Protocol, "下载路径包含有歧义的组成部分");
                        if (slash == std::string::npos) { j.local_name = component; break; }
                        int next = ::openat(j.dirfd, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                        if (next < 0) fail(ErrorCode::Protocol, "下载目录必须是真实目录（不能是符号链接）", errno);
                        ::close(j.dirfd); j.dirfd = next; at = slash + 1;
                    }
                    struct stat directory{};
                    if (::fstat(j.dirfd, &directory) || directory.st_uid != ::geteuid() ||
                        (directory.st_mode & (S_IWGRP | S_IWOTH)))
                        fail(ErrorCode::Protocol, "下载目录必须属于当前用户，且组和其他用户不可写");
                    unsigned char random[16];
                    if (!fill_random(random, sizeof(random))) fail(ErrorCode::Internal, "无法生成下载临时文件名");
                    j.temp = ".jianssh-";
                    constexpr char hex[] = "0123456789abcdef";
                    for (auto byte : random) { j.temp += hex[byte >> 4]; j.temp += hex[byte & 15]; }
                    j.fd = ::openat(j.dirfd, j.temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                    if (j.fd < 0) fail(ErrorCode::Protocol, "无法创建下载临时文件", errno);
                    j.temp_created = true;
#endif
                }
                if (upload) {
                    unsigned char random[16];
                    if (!fill_random(random, sizeof(random))) fail(ErrorCode::Internal, "无法生成上传临时文件名");
                    const auto slash=r.remote_path.rfind('/');
                    j.temp = (slash==std::string::npos?std::string{}:r.remote_path.substr(0,slash+1)) + ".jianssh-";
                    constexpr char hex[] = "0123456789abcdef";
                    for (auto byte : random) { j.temp += hex[byte >> 4]; j.temp += hex[byte & 15]; }
                }
                j.phase = 1; return true;
            }
            if (j.phase == 1) {
                j.handle = libssh2_sftp_open(sftp, upload ? j.temp.c_str() : r.remote_path.c_str(),
                    upload ? LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL : LIBSSH2_FXF_READ, 0600);
                if (!j.handle) { task_rc(libssh2_session_last_errno(session), "打开传输文件"); return false; }
                if (upload) j.temp_created = true;
                j.phase = 2; return true;
            }
            if (j.phase == 2) {
                if (!task_rc(libssh2_sftp_fstat(j.handle, &j.attrs), "读取传输文件状态")) return false;
                if (!upload) {
                    if (!(j.attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !LIBSSH2_SFTP_S_ISREG(j.attrs.permissions))
                        fail(ErrorCode::Protocol, "下载源必须是普通文件");
                    if (!(j.attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) || !(j.attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME))
                        fail(ErrorCode::Protocol, "下载源缺少大小或修改时间");
                    j.result.total = j.attrs.filesize;
                }
                j.phase = 3; publish_job(j); return true;
            }
            if (j.phase == 3) {
                if (upload) {
                    if (j.buffer.empty()) {
                        j.buffer.resize(32768); auto n = local_read(j.fd, j.buffer.data(), j.buffer.size());
                        if (n < 0) fail(ErrorCode::Protocol, "读取上传源文件失败", errno);
                        j.buffer.resize(n); j.offset = 0;
                        if (!n) { j.phase = 4; return true; }
                    }
                    auto n = libssh2_sftp_write(j.handle, j.buffer.data() + j.offset, j.buffer.size() - j.offset);
                    if (n == LIBSSH2_ERROR_EAGAIN) return false;
                    task_rc(static_cast<int>(n), "上传");
                    if (!n) return false;
                    j.offset += n; j.result.transferred += n;
                    if (j.offset == j.buffer.size()) j.buffer.clear();
                } else {
                    char buffer[32768]; auto n = libssh2_sftp_read(j.handle, buffer, sizeof(buffer));
                    if (n == LIBSSH2_ERROR_EAGAIN) return false;
                    task_rc(static_cast<int>(n), "下载");
                    if (!n) { j.phase = 4; return true; }
                    auto written = local_write(j.fd, buffer, n);
                    if (written != n) fail(ErrorCode::Protocol, "写入下载临时文件失败", errno);
                    j.result.transferred += n;
                    if (j.result.transferred > j.result.total) fail(ErrorCode::Protocol, "下载源文件大小发生变化");
                }
                publish_job(j); return true;
            }
            if (j.phase == 4) {
                if (j.result.transferred != j.result.total) fail(ErrorCode::Protocol, "传输源文件大小发生变化");
                if (upload) {
#ifdef _WIN32
                    struct _stat64 now{};
                    struct _stat64 path_now{};
                    if (::_wstat64(app::native_file_path(r.local_path).c_str(), &path_now) || ::_fstat64(j.fd, &now) ||
                        now.st_size != j.source.st_size || now.st_mtime != j.source.st_mtime ||
                        path_now.st_size != j.source.st_size || path_now.st_mtime != j.source.st_mtime)
                        fail(ErrorCode::Protocol, "上传源文件在传输过程中发生变化");
#else
                    struct stat now{};
                    struct stat path_now{};
                    if (::lstat(r.local_path.c_str(), &path_now) || path_now.st_dev != j.source.st_dev || path_now.st_ino != j.source.st_ino ||
                        ::fstat(j.fd, &now) || now.st_size != j.source.st_size ||
                        now.st_mtim.tv_sec != j.source.st_mtim.tv_sec || now.st_mtim.tv_nsec != j.source.st_mtim.tv_nsec)
                        fail(ErrorCode::Protocol, "上传源文件在传输过程中发生变化");
#endif
                } else {
                    LIBSSH2_SFTP_ATTRIBUTES now{};
                    if (!task_rc(libssh2_sftp_fstat(j.handle, &now), "复查源文件")) return false;
                    if (now.filesize != j.attrs.filesize || now.mtime != j.attrs.mtime)
                        fail(ErrorCode::Protocol, "下载源文件在传输过程中发生变化");
                    j.phase = 41; return true;
                }
                j.phase = 5; return true;
            }
            if (j.phase == 41) {
                LIBSSH2_SFTP_ATTRIBUTES now{};
                if (!task_rc(libssh2_sftp_stat(sftp, r.remote_path.c_str(), &now), "复查下载源路径")) return false;
                if (now.filesize != j.attrs.filesize || now.mtime != j.attrs.mtime)
                    fail(ErrorCode::Protocol, "下载源路径在传输过程中发生变化");
                if (local_sync(j.fd)) fail(ErrorCode::Protocol, "同步下载文件失败", errno);
                j.phase = 5; return true;
            }
            if (j.phase == 5) {
                int rc = libssh2_sftp_close_handle(j.handle);
                if (rc == LIBSSH2_ERROR_EAGAIN) return false;
                // libssh2 consumes the handle on every non-EAGAIN return,
                // including server close errors; clear before throwing.
                j.handle = nullptr;
                task_rc(rc, "关闭传输文件");
                if (upload) { local_close(j.fd); j.fd = -1; }
                j.phase = 6; return true;
            }
            if (upload) {
                // Overwrite requires the advertised OpenSSH POSIX rename extension.
                // SFTP v3 rename flags alone do not guarantee atomic overwrite.
                int rc = r.overwrite ? libssh2_sftp_posix_rename(sftp, j.temp.c_str(), r.remote_path.c_str()) :
                    libssh2_sftp_rename_ex(sftp, j.temp.data(), static_cast<unsigned>(j.temp.size()),
                        r.remote_path.data(), static_cast<unsigned>(r.remote_path.size()), 0);
                if (!task_rc(rc, "发布上传文件（不提供不安全的覆盖回退）")) return false;
            } else {
#ifdef _WIN32
                struct _stat64 held{};
                if (::_fstat64(j.fd, &held) || static_cast<std::uint64_t>(held.st_size) != j.result.total)
                    fail(ErrorCode::Protocol, "发布前下载临时文件发生变化");
                const auto target_path = j.local_directory / app::native_file_path(j.local_name);
                const auto& name=target_path.native();
                const auto bytes=name.size()*sizeof(wchar_t);
                const auto length=sizeof(FILE_RENAME_INFO)+bytes;
                auto storage=std::make_unique<std::byte[]>(length);
                auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.get());
                rename->ReplaceIfExists=r.overwrite;
                rename->RootDirectory=nullptr;
                rename->FileNameLength=static_cast<DWORD>(bytes);
                std::memcpy(storage.get()+offsetof(FILE_RENAME_INFO,FileName),name.data(),bytes);
                const HANDLE pinned=reinterpret_cast<HANDLE>(::_get_osfhandle(j.fd));
                if (!::SetFileInformationByHandle(pinned,FileRenameInfo,rename,static_cast<DWORD>(length)))
                    fail(ErrorCode::Protocol, "下载目标冲突或发布失败", static_cast<int>(::GetLastError()));
                j.published = true;
#else
                struct stat held{}, named{}, directory{};
                if (::fstat(j.dirfd, &directory) || directory.st_uid != ::geteuid() ||
                    (directory.st_mode & (S_IWGRP | S_IWOTH)))
                    fail(ErrorCode::Protocol, "发布前下载目录变得不安全");
                if (::fstat(j.fd, &held) || ::fstatat(j.dirfd, j.temp.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
                    !S_ISREG(named.st_mode) || named.st_dev != held.st_dev || named.st_ino != held.st_ino ||
                    named.st_size != held.st_size || static_cast<std::uint64_t>(held.st_size) != j.result.total)
                    fail(ErrorCode::Protocol, "发布前下载临时文件被替换或修改");
                int rc = r.overwrite ? ::renameat(j.dirfd, j.temp.c_str(), j.dirfd, j.local_name.c_str()) :
                    ::linkat(j.dirfd, j.temp.c_str(), j.dirfd, j.local_name.c_str(), 0);
                if (rc) fail(ErrorCode::Protocol, "下载目标冲突或发布失败", errno);
                if (!r.overwrite) ::unlinkat(j.dirfd, j.temp.c_str(), 0);
                j.published = true;
                if (::fsync(j.dirfd)) fail(ErrorCode::Protocol, "下载已发布，但目录同步失败", errno);
#endif
            }
            j.published = true; finish_job(j, TaskState::Succeeded); return true;
        } catch (const Failure& error) { finish_job(j, TaskState::Failed, error.error.message); return true; }
    }
    void pump(const ConnectOptions& options) {
        std::string writing;
        StringGuard writing_guard{writing};
        std::size_t written = 0;
        std::optional<Clock::time_point> write_deadline;
        std::optional<std::pair<int,int>> resizing;
        std::optional<Clock::time_point> resize_deadline;
        auto keepalive_due = Clock::now() + options.keepalive_interval;
        std::optional<Clock::time_point> keepalive_deadline;
        bool remote_eof = false;
        while (!remote_eof) {
            check_cancel();
            bool progressed = false;
            if (writing.empty()) {
                std::lock_guard lock(mutex);
                if (!input.empty()) { writing = std::move(input.front()); input.pop_front(); written = 0;
                    write_deadline = Clock::now() + options.operation_timeout; }
            }
            if (!writing.empty()) {
                const auto rc = libssh2_channel_write_ex(channel, 0, writing.data() + written, writing.size() - written);
                if (rc > 0) {
                    written += static_cast<std::size_t>(rc); progressed = true;
                    { std::lock_guard lock(mutex); view.pending_input -= static_cast<std::size_t>(rc); }
                    if (written == writing.size()) { wipe(writing); written = 0; write_deadline.reset(); }
                } else if (rc != LIBSSH2_ERROR_EAGAIN && rc < 0) ssh_error(ErrorCode::Network, "写入终端输入", static_cast<int>(rc));
                if (write_deadline && Clock::now() >= *write_deadline) fail(ErrorCode::Timeout, "终端输入写入超时");
            }
            if (!resizing) { std::lock_guard lock(mutex); resizing = pending_size; pending_size.reset();
                if (resizing) resize_deadline = Clock::now() + options.operation_timeout; }
            if (resizing) {
                int rc = libssh2_channel_request_pty_size_ex(channel, resizing->first, resizing->second, 0, 0);
                if (!rc) { resizing.reset(); resize_deadline.reset(); progressed = true; }
                else if (rc != LIBSSH2_ERROR_EAGAIN) ssh_error(ErrorCode::Protocol, "调整终端大小", rc);
                if (resize_deadline && Clock::now() >= *resize_deadline) fail(ErrorCode::Timeout, "终端大小调整超时");
            }
            // Read only available queue capacity. Bounded backpressure rather
            // than discarding terminal bytes; SSH's channel window slows peer.
            bool can_read = false;
            for (int stream = 0; stream < 2; ++stream) {
                std::size_t room;
                { std::lock_guard lock(mutex); room = output_capacity - view.available_output; }
                if (!room) break;
                can_read = true;
                char buffer[16384];
                const auto rc = libssh2_channel_read_ex(channel, stream, buffer, std::min(room, sizeof(buffer)));
                if (rc > 0) {
                    std::lock_guard lock(mutex);
                    output.emplace_back(buffer, static_cast<std::size_t>(rc));
                    view.available_output += static_cast<std::size_t>(rc); progressed = true;
                } else if (rc < 0 && rc != LIBSSH2_ERROR_EAGAIN) ssh_error(ErrorCode::Network, "读取终端输出", static_cast<int>(rc));
            }
            // EOF may be received while buffered channel data remains: only
            // finish once a read pass has drained it (and queue has capacity).
            if (can_read && !progressed && libssh2_channel_eof(channel)) remote_eof = true;
            const auto now = Clock::now();
            if (options.keepalive_interval.count() > 0 && now >= keepalive_due) {
                if (!keepalive_deadline) keepalive_deadline = now + options.operation_timeout;
                int seconds = 0;
                int rc = libssh2_keepalive_send(session, &seconds);
                if (!rc) { keepalive_due = now + std::chrono::seconds(std::max(1, seconds)); keepalive_deadline.reset(); }
                else if (rc != LIBSSH2_ERROR_EAGAIN) ssh_error(ErrorCode::Network, "SSH 保活", rc);
                if (keepalive_deadline && now >= *keepalive_deadline) fail(ErrorCode::Timeout, "SSH 保活超时");
            }
            progressed = task_step() || progressed;
            if (progressed || remote_eof) continue;
            auto deadline = now + (active ? std::chrono::milliseconds(20) : std::chrono::milliseconds(1000));
            if (write_deadline) deadline = std::min(deadline, *write_deadline);
            if (resize_deadline) deadline = std::min(deadline, *resize_deadline);
            if (keepalive_deadline) deadline = std::min(deadline, *keepalive_deadline);
            if (options.keepalive_interval.count() > 0 && keepalive_due > now) deadline = std::min(deadline, keepalive_due);
            // Avoid spinning on readable socket while the output queue is full.
            // Pending writes/resizes must still be serviced under backpressure.
            bool watch = active.has_value() || can_read || !writing.empty() || resizing.has_value() || keepalive_deadline.has_value();
            try { wait_socket(deadline, 0, watch); } catch (const Failure& e) {
                if (e.error.code != ErrorCode::Timeout) throw;
            }
        }
        // EOF may precede exit-status and CLOSE in separate SSH packets.
        const auto close_deadline = Clock::now() + options.operation_timeout;
        retry([&] { return libssh2_channel_close(channel); }, close_deadline, ErrorCode::Protocol, "关闭远端终端通道");
        retry([&] { return libssh2_channel_wait_closed(channel); }, close_deadline, ErrorCode::Protocol, "等待远端终端关闭");
        std::lock_guard lock(mutex);
        view.exit_status = libssh2_channel_get_exit_status(channel);
        emit_locked({EventType::RemoteClosed, view.state, {}, {}});
    }
    void cleanup() noexcept {
        // Release transport first: free calls must never wait for a peer. On a
        // dead socket libssh2 finalization can no longer return EAGAIN forever.
        if (socket_valid(socket)) ::shutdown(socket, SHUT_RDWR);
#ifndef _WIN32
        if (agent_socket >= 0) { ::close(agent_socket); agent_socket = -1; }
        agent_key.clear();
#endif
        if (active) {
            if (active->fd >= 0) local_close(active->fd);
            if (active->temp_created && !active->published && active->request.kind == TaskKind::Download) {
#ifdef _WIN32
                std::error_code ignored;
                std::filesystem::remove(active->local_directory / active->temp, ignored);
#else
                ::unlinkat(active->dirfd, active->temp.c_str(), 0);
#endif
            }
#ifndef _WIN32
            if (active->dirfd >= 0) ::close(active->dirfd);
#endif
            if (active->handle) libssh2_sftp_close_handle(active->handle);
            if (active->exec) libssh2_channel_free(active->exec);
            active.reset();
        }
        if (sftp) { libssh2_sftp_shutdown(sftp); sftp = nullptr; }
        if (session) {
            libssh2_session_set_blocking(session, 0);
            if (channel) { libssh2_channel_free(channel); channel = nullptr; }
            libssh2_session_free(session); session = nullptr;
        }
        if (socket_valid(socket)) { close_socket(socket); socket = invalid_socket; }
    }
    void run(ConnectOptions options) noexcept {
        CredentialsGuard credentials{options.auth};
        SessionError error;
        try {
            static std::once_flag init;
            static int init_error = 0;
            std::call_once(init, [] { init_error = libssh2_init(0); });
            if (init_error) fail(ErrorCode::Internal, "SSH 库初始化失败", init_error);
            auto deadline = Clock::now() + options.connect_timeout;
            tcp_connect(options, deadline);
            session = libssh2_session_init_ex(secure_alloc, secure_free, secure_realloc, this);
            if (!session) fail(ErrorCode::Internal, "无法创建 SSH 会话");
            libssh2_session_set_blocking(session, 0);
            libssh2_session_flag(session, LIBSSH2_FLAG_SIGPIPE, 0);
            retry([&] { return libssh2_session_handshake(session, socket); }, deadline, ErrorCode::Protocol, "SSH 握手");
            verify_host(options); // MUST precede every authentication mechanism.
            authenticate(options); // Credentials wiped as soon as auth returns.
            open_shell(options);
            pump(options);
        } catch (const Failure& e) { error = e.error; }
          catch (const std::exception& e) { error = {ErrorCode::Internal, 0, e.what()}; }
          catch (...) { error = {ErrorCode::Internal, 0, "SSH 后台线程发生未知错误"}; }
        wipe_auth(options.auth);
        if (active && active->temp_created && !active->published && active->request.kind == TaskKind::Upload)
            error.message += "；远端临时文件可能残留：" + active->temp;
        cleanup();
        std::lock_guard lock(mutex);
        for (auto& s : input) wipe(s);
        input.clear(); view.pending_input = 0;
        pending_size.reset(); trust_answer.reset(); view.host_key_request.reset();
        jobs.clear(); task_cancels.clear();
        for (auto& task : view.tasks) if (task.state == TaskState::Queued || task.state == TaskState::Running) {
            task.state = TaskState::Cancelled; task.error = "会话已关闭";
        }
        view.error = std::move(error);
        view.state = view.error.code != ErrorCode::None && view.error.code != ErrorCode::Cancelled
            ? SessionState::Failed : SessionState::Disconnected;
        if (view.error.code != ErrorCode::None) emit_locked({EventType::Error, view.state, view.error, {}});
        emit_locked({EventType::StateChanged, view.state, {}, {}});
        running = false;
    }
};

SshSession::SshSession() : impl_(std::make_unique<Impl>()) {}
SshSession::~SshSession() = default;
bool SshSession::connect(ConnectOptions options) {
    CredentialsGuard guard{options.auth};
    auto& p = *impl_;
    std::lock_guard lifecycle(p.lifecycle);
    {
        std::lock_guard lock(p.mutex);
        if (p.running) return false;
    }
    if (p.owner.joinable()) p.owner.join();
    const bool invalid = options.host.empty() || options.host.find('\0') != std::string::npos ||
        options.user.empty() || options.user.find('\0') != std::string::npos || !options.port ||
        !options.input_capacity || !options.output_capacity || !options.event_capacity || !options.task_capacity || options.task_capacity > 256 ||
        options.columns < 1 || options.rows < 1 || options.columns > 65535 || options.rows > 65535 ||
        options.connect_timeout.count() <= 0 || options.operation_timeout.count() <= 0 || options.trust_timeout.count() <= 0 ||
        options.keepalive_interval.count() < 0 || options.keepalive_interval.count() > INT_MAX ||
        options.user.size() > UINT_MAX || options.auth.password.size() > UINT_MAX ||
        options.auth.private_key_file.find('\0') != std::string::npos || options.auth.public_key_file.find('\0') != std::string::npos ||
        options.auth.passphrase.find('\0') != std::string::npos ||
        (options.auth.method != AuthMethod::Password && options.auth.method != AuthMethod::Key && options.auth.method != AuthMethod::Agent) ||
        (options.auth.method == AuthMethod::Key && options.auth.private_key_file.empty());
    {
        std::lock_guard lock(p.mutex);
        p.events.clear(); p.output.clear(); p.output_offset = 0; p.input.clear(); p.input_offset = 0;
        p.view = {}; p.pending_size.reset(); p.trust_answer.reset();
        p.event_capacity = std::max<std::size_t>(1, options.event_capacity);
        if (invalid) {
            p.view.state = SessionState::Failed;
            p.view.error = {ErrorCode::InvalidConfiguration, 0, "SSH 连接参数无效"};
            p.emit_locked({EventType::Error, p.view.state, p.view.error, {}}); return false;
        }
        p.task_capacity = options.task_capacity;
        p.input_capacity = options.input_capacity; p.output_capacity = options.output_capacity;
        p.cancelled = false; p.running = true; p.view.state = SessionState::Connecting;
        p.emit_locked({EventType::StateChanged, p.view.state, {}, {}});
    }
    try {
        p.owner = std::thread([&p, options = std::move(options)]() mutable { p.run(std::move(options)); });
    } catch (const std::exception& e) {
        std::lock_guard lock(p.mutex); p.running = false; p.view.state = SessionState::Failed;
        p.view.error = {ErrorCode::Internal, 0, e.what()};
        p.emit_locked({EventType::Error, p.view.state, p.view.error, {}}); return false;
    }
    return true;
}
SessionSnapshot SshSession::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->view; }
std::vector<SessionEvent> SshSession::poll_events() {
    std::lock_guard lock(impl_->mutex);
    std::vector<SessionEvent> result;
    result.reserve(impl_->events.size());
    while (!impl_->events.empty()) { result.push_back(std::move(impl_->events.front())); impl_->events.pop_front(); }
    return result;
}
std::string SshSession::read_output(std::size_t max_bytes) {
    auto& p = *impl_;
    std::string result;
    {
        std::lock_guard lock(p.mutex);
        const auto n = std::min(max_bytes, p.view.available_output);
        result.reserve(n);
        while (result.size() < n) {
            auto& front = p.output.front();
            auto take = std::min(n - result.size(), front.size() - p.output_offset);
            result.append(front, p.output_offset, take); p.output_offset += take;
            if (p.output_offset == front.size()) { p.output.pop_front(); p.output_offset = 0; }
        }
        p.view.available_output -= n;
    }
    if (!result.empty()) p.wake();
    return result;
}
std::size_t SshSession::write_input(std::string_view bytes) {
    auto& p = *impl_;
    std::size_t n;
    {
        std::lock_guard lock(p.mutex);
        if (p.view.state != SessionState::Connected || p.cancelled) return 0;
        n = std::min(bytes.size(), p.input_capacity - p.view.pending_input);
        if (n) { p.input.emplace_back(bytes.substr(0, n)); p.view.pending_input += n; }
    }
    if (n) p.wake();
    return n;
}
bool SshSession::resize(int columns, int rows) {
    if (columns < 1 || rows < 1 || columns > 65535 || rows > 65535) return false;
    auto& p = *impl_;
    { std::lock_guard lock(p.mutex);
      if (p.view.state != SessionState::Connected || p.cancelled) return false;
      p.pending_size = std::pair{columns, rows}; }
    p.wake(); return true;
}
bool SshSession::answer_host_key(std::uint64_t request_id, bool accept) {
    auto& p = *impl_;
    { std::lock_guard lock(p.mutex);
      if (p.cancelled || p.view.state != SessionState::AwaitingHostKey || !p.view.host_key_request ||
          p.view.host_key_request->id != request_id || p.trust_answer.has_value()) return false;
      p.trust_answer = accept; }
    p.wake(); return true;
}
std::uint64_t SshSession::submit_task(TaskRequest request) {
    auto valid_path = [](const std::string& text) { return !text.empty() && text.size() <= 16384 && text.find('\0') == std::string::npos; };
    if (request.kind < TaskKind::List || request.kind > TaskKind::Exec || request.permissions > 07777 ||
        request.timeout.count() <= 0 || request.timeout > std::chrono::hours(24) || request.output_limit > 1024 * 1024) return 0;
    if (request.kind == TaskKind::Exec) {
        if (!valid_path(request.command)) return 0;
    } else if (!valid_path(request.remote_path)) return 0;
    if (request.kind == TaskKind::Rename && !valid_path(request.destination_path)) return 0;
    if ((request.kind == TaskKind::Upload || request.kind == TaskKind::Download) && !valid_path(request.local_path)) return 0;
    auto local_absolute = [](const std::string& path) {
#ifdef _WIN32
        if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
            (path[2] == '\\' || path[2] == '/')) return true;
        return path.size() >= 2 && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
#else
        return !path.empty() && path.front() == '/';
#endif
    };
    if (request.kind == TaskKind::Download && (!local_absolute(request.local_path) || request.local_path.back() == '/')) return 0;
    auto& p = *impl_; std::uint64_t id;
    {
        std::lock_guard lock(p.mutex);
        if (p.cancelled || p.view.state != SessionState::Connected || p.view.tasks.size() >= p.task_capacity) return 0;
        id = ++p.next_task;
        TaskSnapshot task; task.id = id; task.kind = request.kind;
        p.view.tasks.push_back(std::move(task)); p.jobs.emplace_back(id, std::move(request));
    }
    p.wake(); return id;
}
bool SshSession::cancel_task(std::uint64_t id) {
    auto& p = *impl_; bool found = false;
    {
        std::lock_guard lock(p.mutex);
        for (const auto& task : p.view.tasks) if (task.id == id && (task.state == TaskState::Queued || task.state == TaskState::Running)) found = true;
        if (found) p.task_cancels.insert(id);
    }
    if (found) p.wake();
    return found;
}
std::vector<TaskSnapshot> SshSession::poll_tasks() {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    std::vector<TaskSnapshot> result;
    auto& tasks = p.view.tasks;
    for (auto it = tasks.begin(); it != tasks.end();) {
        if (it->state != TaskState::Queued && it->state != TaskState::Running) {
            result.push_back(std::move(*it)); it = tasks.erase(it);
        } else ++it;
    }
    return result;
}
void SshSession::disconnect() {
    auto& p = *impl_;
    { std::lock_guard lock(p.mutex);
      if (!p.running) return;
      p.cancelled = true; p.view.state = SessionState::Disconnecting;
      p.emit_locked({EventType::StateChanged, p.view.state, {}, {}}); }
    p.wake();
}
} // namespace jianssh::net
