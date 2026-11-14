#include "jianssh/net/serial_session.hpp"
#include "jianssh/net/serial_port_name.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <devguid.h>
#include <ntddser.h>
#include <setupapi.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <asm/termbits.h>
#include <asm/ioctls.h>
#include <unistd.h>
#endif

namespace jianssh::net {
std::vector<std::string> enumerate_serial_ports() {
    std::vector<std::string> result;
#ifdef _WIN32
    std::vector<std::wstring> names;
    auto add_name = [&names](std::wstring_view value) {
        auto normalized = detail::normalize_serial_port_name(value);
        if (!normalized.empty()) names.push_back(std::move(normalized));
    };
    auto add_device_port_name = [&add_name](HDEVINFO devices, SP_DEVINFO_DATA& info) {
        HKEY key = SetupDiOpenDevRegKey(devices, &info, DICS_FLAG_GLOBAL, 0,
                                        DIREG_DEV, KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE) return;
        DWORD type = 0, bytes = 0;
        LONG rc = RegQueryValueExW(key, L"PortName", nullptr, &type, nullptr, &bytes);
        if (rc == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t) &&
            bytes <= 32768 * sizeof(wchar_t)) {
            std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
            DWORD actual = bytes;
            if (RegQueryValueExW(key, L"PortName", nullptr, &type,
                                 reinterpret_cast<BYTE*>(value.data()), &actual) == ERROR_SUCCESS &&
                type == REG_SZ) {
                value.back() = L'\0';
                add_name(value.data());
            }
        }
        RegCloseKey(key);
    };

    // Enumerate present COM-port interfaces and Ports-class devnodes. Some
    // USB/virtual drivers register only one of these, so merge and deduplicate.
    HDEVINFO interfaces = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_COMPORT, nullptr, nullptr,
                                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (interfaces != INVALID_HANDLE_VALUE) {
        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA interface_data{};
            interface_data.cbSize = sizeof(interface_data);
            if (!SetupDiEnumDeviceInterfaces(interfaces, nullptr, &GUID_DEVINTERFACE_COMPORT,
                                             index, &interface_data)) break;
            DWORD required = 0;
            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof(info);
            SetupDiGetDeviceInterfaceDetailW(interfaces, &interface_data, nullptr, 0,
                                             &required, &info);
            if (!required || required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
            std::vector<BYTE> storage(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            info = {};
            info.cbSize = sizeof(info);
            if (SetupDiGetDeviceInterfaceDetailW(interfaces, &interface_data, detail, required,
                                                 nullptr, &info))
                add_device_port_name(interfaces, info);
        }
        SetupDiDestroyDeviceInfoList(interfaces);
    }

    HDEVINFO ports = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (ports != INVALID_HANDLE_VALUE) {
        for (DWORD index = 0;; ++index) {
            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof(info);
            if (!SetupDiEnumDeviceInfo(ports, index, &info)) break;
            add_device_port_name(ports, info);
        }
        SetupDiDestroyDeviceInfoList(ports);
    }

    // QueryDosDevice is a fallback for nonstandard devnodes and is retried with
    // a growing buffer instead of failing silently on a fixed-size MULTI_SZ.
    if (names.empty()) {
        DWORD capacity = 1024;
        while (capacity <= 1024 * 1024) {
            std::vector<wchar_t> aliases(capacity, L'\0');
            DWORD length = QueryDosDeviceW(nullptr, aliases.data(), capacity);
            if (length) {
                detail::append_serial_port_names_from_multisz(aliases.data(), length, names);
                break;
            }
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
            capacity *= 2;
        }
    }
    detail::sort_unique_serial_port_names(names);
    result.reserve(names.size());
    for (const auto& name : names) result.emplace_back(name.begin(), name.end());
#else
    if (DIR* dir = opendir("/dev")) {
        while (auto* ent = readdir(dir)) {
            std::string s(ent->d_name);
            if (s.compare(0,6,"ttyUSB") == 0 || s.compare(0,6,"ttyACM") == 0 ||
                s.compare(0,4,"ttyS") == 0 || s.compare(0,8,"ttyAMA") == 0 ||
                s.compare(0,7,"rfcomm") == 0) result.push_back("/dev/" + s);
        }
        closedir(dir);
    }
    std::sort(result.begin(), result.end());
#endif
    return result;
}
struct SerialSession::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    SerialOptions options;
    SerialSnapshot status;
    std::deque<char> input, output;
    bool stop{}, failed{};
    void fail(const std::string& message, int code) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stop) { failed = true; status.state = SerialState::Disconnecting; status.error = message; status.native_code = code; }
    }
    void run();
};
void SerialSession::Impl::run() {
#ifdef _WIN32
    HANDLE fd = INVALID_HANDLE_VALUE;
    auto native_error = [this](const char* action, DWORD code) {
        const char* reason = "系统错误";
        switch (code) {
        case ERROR_ACCESS_DENIED: reason = "权限不足或串口已被占用"; break;
        case ERROR_SHARING_VIOLATION: case ERROR_BUSY: reason = "串口已被占用"; break;
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: reason = "串口不存在"; break;
        case ERROR_DEVICE_NOT_CONNECTED: case ERROR_DEV_NOT_EXIST: case ERROR_INVALID_HANDLE: reason = "串口设备已断开或拔出"; break;
        case ERROR_INVALID_PARAMETER: case ERROR_NOT_SUPPORTED: reason = "串口参数或操作不受支持"; break;
        default: break;
        }
        fail(std::string(action) + "：" + reason + "（系统错误码 " + std::to_string(code) + "）", static_cast<int>(code));
    };
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, options.port.c_str(), -1, nullptr, 0);
    if (!wlen) { native_error("串口名称不是有效的 UTF-8", GetLastError()); goto done; }
    {
        std::wstring path(static_cast<std::size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, options.port.c_str(), -1, &path[0], wlen); path.pop_back();
        path = detail::serial_port_device_path(path);
        fd = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    if (fd == INVALID_HANDLE_VALUE) { native_error("无法打开串口", GetLastError()); goto done; }
    {
        DCB d{}; d.DCBlength = sizeof(d);
        if (!GetCommState(fd, &d)) { native_error("无法读取串口设置", GetLastError()); goto done; }
        d.BaudRate = options.baud; d.ByteSize = static_cast<BYTE>(options.data_bits);
        d.StopBits = options.stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
        d.Parity = options.parity == SerialParity::Even ? EVENPARITY : options.parity == SerialParity::Odd ? ODDPARITY : NOPARITY;
        d.fBinary = TRUE; d.fParity = options.parity != SerialParity::None;
        d.fOutxCtsFlow = options.flow == SerialFlow::Hardware;
        d.fRtsControl = options.flow == SerialFlow::Hardware ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_DISABLE;
        d.fDtrControl = DTR_CONTROL_DISABLE; d.fOutxDsrFlow = FALSE; d.fDsrSensitivity = FALSE;
        d.fOutX = d.fInX = options.flow == SerialFlow::Software;
        d.fErrorChar = FALSE; d.fNull = FALSE; d.fAbortOnError = FALSE;
        if (!SetCommState(fd, &d)) { native_error("无法应用串口设置", GetLastError()); goto done; }
        COMMTIMEOUTS t{}; t.ReadIntervalTimeout = MAXDWORD;
        if (!SetCommTimeouts(fd, &t)) { native_error("无法设置串口超时", GetLastError()); goto done; }
    }
#else
    int fd = -1;
    auto native_error = [this](const char* action, int code) {
        const char* reason = "系统错误";
        switch (code) {
        case EACCES: case EPERM: reason = "权限不足"; break;
        case EBUSY: reason = "串口已被占用"; break;
        case ENOENT: case ENOTDIR: reason = "串口不存在"; break;
        case ENODEV: case ENXIO: case EIO: reason = "串口设备已断开、拔出或发生输入输出错误"; break;
        case EINVAL: case ENOTTY: case EOPNOTSUPP: reason = "串口参数或操作不受支持"; break;
        default: break;
        }
        fail(std::string(action) + "：" + reason + "（系统错误码 " + std::to_string(code) + "）", code);
    };
    fd = open(options.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { native_error("无法打开串口", errno); goto done; }
    if (ioctl(fd, TIOCEXCL) < 0) { native_error("无法独占串口", errno); goto done; }
    {
        termios2 t{};
        if (ioctl(fd, TCGETS2, &t) < 0) { native_error("无法读取串口设置", errno); goto done; }
        t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        t.c_oflag &= ~OPOST;
        t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        t.c_cflag &= ~(CSIZE | PARENB); t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
        switch(options.data_bits) { case 5:t.c_cflag |= CS5;break; case 6:t.c_cflag |= CS6;break; case 7:t.c_cflag |= CS7;break; default:t.c_cflag |= CS8; }
        if (options.stop_bits == 2) t.c_cflag |= CSTOPB;
        if (options.parity != SerialParity::None) { t.c_cflag |= PARENB; if (options.parity == SerialParity::Odd) t.c_cflag |= PARODD; }
#ifdef CRTSCTS
        t.c_cflag &= ~CRTSCTS; if (options.flow == SerialFlow::Hardware) t.c_cflag |= CRTSCTS;
#else
        if (options.flow == SerialFlow::Hardware) { fail("不支持硬件流控", 0); goto done; }
#endif
        t.c_iflag &= ~(IXON | IXOFF | IXANY);
        if (options.flow == SerialFlow::Software) t.c_iflag |= IXON | IXOFF;
        t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
        t.c_cflag = (t.c_cflag & ~CBAUD) | BOTHER;
        t.c_ispeed = t.c_ospeed = options.baud;
        if (ioctl(fd, TCSETS2, &t) < 0) { native_error("无法应用串口设置", errno); goto done; }
        termios2 actual{};
        if (ioctl(fd, TCGETS2, &actual) < 0) { native_error("无法验证串口设置", errno); goto done; }
        if ((actual.c_cflag & (CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS)) != (t.c_cflag & (CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS)) || (actual.c_iflag & (IXON | IXOFF)) != (t.c_iflag & (IXON | IXOFF)) || actual.c_ispeed != options.baud || actual.c_ospeed != options.baud) { fail("串口驱动不支持所请求的参数", 0); goto done; }
    }
#endif
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stop) status.state = SerialState::Connected;
    }
    while (true) {
        std::string writebuf; std::size_t room;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stop) break;
            room = options.output_capacity - output.size();
            auto n = std::min<std::size_t>(input.size(), 4096);
            writebuf.assign(input.begin(), input.begin() + n);
        }
#ifdef _WIN32
        // Overlapped operations are cancelled and reaped before their storage leaves scope.
        auto io = [&](bool reading, char* data, DWORD size, DWORD& transferred) -> bool {
            OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) { native_error("无法创建串口事件", GetLastError()); return false; }
            BOOL ok = reading ? ReadFile(fd, data, size, &transferred, &ov) : WriteFile(fd, data, size, &transferred, &ov);
            if (!ok) {
                DWORD e = GetLastError();
                if (e == ERROR_IO_PENDING) {
                    DWORD wait = WaitForSingleObject(ov.hEvent, 20);
                    if (wait == WAIT_TIMEOUT) { CancelIoEx(fd, &ov); if (!GetOverlappedResult(fd, &ov, &transferred, TRUE)) { e = GetLastError(); if (e != ERROR_OPERATION_ABORTED) { CloseHandle(ov.hEvent); native_error("串口读写失败", e); return false; } } }
                    else if (wait != WAIT_OBJECT_0 || !GetOverlappedResult(fd, &ov, &transferred, FALSE)) { e = GetLastError(); CloseHandle(ov.hEvent); native_error("串口读写失败", e); return false; }
                } else { CloseHandle(ov.hEvent); native_error("串口读写失败", e); return false; }
            }
            CloseHandle(ov.hEvent); return true;
        };
        if (!writebuf.empty()) { DWORD n = 0; if (!io(false, &writebuf[0], static_cast<DWORD>(writebuf.size()), n)) break; std::lock_guard<std::mutex> lock(mutex); input.erase(input.begin(), input.begin() + n); }
        if (room) { char buf[4096]; DWORD n = 0; if (!io(true, buf, static_cast<DWORD>(std::min(room, sizeof(buf))), n)) break; std::lock_guard<std::mutex> lock(mutex); output.insert(output.end(), buf, buf + n); }
#else
        pollfd p{fd, static_cast<short>((room ? POLLIN : 0) | (!writebuf.empty() ? POLLOUT : 0)), 0};
        int ready = poll(&p, 1, 20);
        if (ready < 0) { if (errno == EINTR) continue; native_error("串口轮询失败", errno); break; }
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { fail("串口设备已断开、拔出或发生输入输出错误", 0); break; }
        if ((p.revents & POLLOUT) && !writebuf.empty()) {
            ssize_t n = write(fd, writebuf.data(), writebuf.size());
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { native_error("串口写入失败", errno); break; }
            if (n > 0) { std::lock_guard<std::mutex> lock(mutex); input.erase(input.begin(), input.begin() + n); }
        }
        if ((p.revents & POLLIN) && room) {
            char buf[4096]; ssize_t n = read(fd, buf, std::min(room, sizeof(buf)));
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { native_error("串口读取失败", errno); break; }
            if (n > 0) { std::lock_guard<std::mutex> lock(mutex); output.insert(output.end(), buf, buf+n); }
        }
#endif
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(10), [this]{ return stop || !input.empty(); });
    }
done:
#ifdef _WIN32
    if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd);
#else
    if (fd >= 0) { ioctl(fd, TIOCNXCL); close(fd); }
#endif
    { std::lock_guard<std::mutex> lock(mutex); status.discarded_input += input.size(); input.clear(); status.state = failed ? SerialState::Failed : SerialState::Disconnected; }
}
SerialSession::SerialSession() : impl_(new Impl) {}
SerialSession::~SerialSession() { disconnect(); if (impl_->worker.joinable()) impl_->worker.join(); }
bool SerialSession::connect(SerialOptions options) {
    // Five data bits with two stop bits means 1.5 on Windows.
    if (options.data_bits == 5 && options.stop_bits == 2) return false;
    if (options.port.empty() || options.port.find('\0') != std::string::npos || !options.baud || options.data_bits < 5 || options.data_bits > 8 || (options.stop_bits != 1 && options.stop_bits != 2) || !options.input_capacity || !options.output_capacity || (options.parity != SerialParity::None && options.parity != SerialParity::Even && options.parity != SerialParity::Odd) || (options.flow != SerialFlow::None && options.flow != SerialFlow::Software && options.flow != SerialFlow::Hardware)) return false;
    { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->status.state != SerialState::Disconnected && impl_->status.state != SerialState::Failed) return false; }
    if (impl_->worker.joinable()) impl_->worker.join();
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->options = std::move(options); impl_->status = {}; impl_->status.state = SerialState::Connecting; impl_->stop = false; impl_->failed = false; impl_->input.clear(); impl_->output.clear(); }
    impl_->worker = std::thread(&Impl::run, impl_.get()); return true;
}
SerialSnapshot SerialSession::snapshot() const { std::lock_guard<std::mutex> lock(impl_->mutex); auto s = impl_->status; s.pending_input = impl_->input.size(); s.available_output = impl_->output.size(); return s; }
std::string SerialSession::read_output(std::size_t n) { std::lock_guard<std::mutex> lock(impl_->mutex); n = std::min(n, impl_->output.size()); std::string s(impl_->output.begin(), impl_->output.begin()+n); impl_->output.erase(impl_->output.begin(), impl_->output.begin()+n); impl_->wake.notify_one(); return s; }
std::size_t SerialSession::write_input(std::string_view bytes) { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->status.state != SerialState::Connected) return 0; auto n = std::min(bytes.size(), impl_->options.input_capacity - impl_->input.size()); impl_->input.insert(impl_->input.end(), bytes.begin(), bytes.begin()+n); impl_->wake.notify_one(); return n; }
void SerialSession::disconnect() { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->status.state == SerialState::Connected || impl_->status.state == SerialState::Connecting) { impl_->stop = true; impl_->status.state = SerialState::Disconnecting; impl_->wake.notify_one(); } }
}
