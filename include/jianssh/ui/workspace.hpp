#pragma once

#include "jianssh/store/host.hpp"
#include "jianssh/net/serial_session.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ImFont;
namespace jianssh::app {
class Terminal;
enum class AuthMethod { Password, Key, Agent };
enum class SessionType { Ssh, Serial };
struct SessionCapabilities { bool file_transfer = true, monitor = true, resize = true; };
struct SessionView {
    std::uint64_t id{};
    std::string title, status;
    std::shared_ptr<Terminal> terminal;
    std::string error;
    bool connected = false;
    SessionType type = SessionType::Ssh;
    SessionCapabilities capabilities;
    bool can_retry = false; // Terminal failure / unrequested disconnect, not an in-flight attempt.
    std::uint64_t password_request_id{}; // Nonzero only while an explicit credential response is needed.
    bool key_passphrase = false;
    std::string auth_target; // Display identity, never a secret.
};
struct TrustRequest {
    std::uint64_t session_id{}, request_id{};
    std::string host, algorithm, fingerprint;
    int port = 22;
    bool mismatch = false;
};
enum class RememberPolicy { Never, Session, Encrypted };
struct ConnectionSettings { std::int64_t host_id{}; RememberPolicy policy = RememberPolicy::Never; bool has_saved_password = false; };
struct ConnectRequest {
    store::Host host;
    std::string password, key_path;
    AuthMethod method = AuthMethod::Password;
    RememberPolicy policy = RememberPolicy::Never;
    // New quick connections are transient unless explicitly saved by the controller.
    bool save_to_workspace = false;
    // Password/Key only: nonempty input + non-Never policy saves; empty input
    // + non-Never policy + has_saved on an existing device requests reuse.
    // Never store credentials in Host.
    bool save_password = false, use_saved_password = false;
};
struct FileEntry {
    std::string name;
    std::uint64_t size{};
    std::int64_t mtime{};
    unsigned permissions{};
    bool directory = false, symlink = false, regular = false;
};
struct FilePane {
    std::uint64_t session_id{}, revision{};
    bool remote = false, loading = false, truncated = false;
    std::string path, requested_path, error, operation_message;
    bool operation_failed = false;
    std::vector<FileEntry> entries;
};
enum class TransferState { Queued, Running, Completed, Failed, Cancelled, Pausing, Paused, Resuming };
// Active includes suspended work: keep it visible, deduplicated and cancellable.
inline bool transfer_active(TransferState state) {
    return state!=TransferState::Completed && state!=TransferState::Failed && state!=TransferState::Cancelled;
}
struct TransferTask {
    std::uint64_t id{}, session_id{}, completed_bytes{}, total_bytes{};
    std::string source, destination, error; bool upload = true;
    TransferState state = TransferState::Queued;
    bool cancel_requested = false;
};
enum class FileOperation { Mkdir, Rename, Delete, Chmod };
struct FileOperationRequest {
    std::uint64_t session_id{}; bool remote = false;
    FileOperation operation = FileOperation::Mkdir;
    std::string path, target; unsigned permissions{};
    bool recursive = false; // UI never enables recursive deletion.
};
struct MetricSample {
    double timestamp{}; // Monotonic seconds; facade and samples use the same clock.
    std::optional<double> cpu_percent, memory_percent, disk_percent, net_rx_bytes_sec, net_tx_bytes_sec;
};
struct MetricsView {
    std::uint64_t session_id{}; double now{}; bool enabled = false;
    std::vector<MetricSample> samples;
    std::string cpu_missing, memory_missing, disk_missing, network_missing, error;
};
// Appearance preference. System follows the desktop light/dark setting and
// is re-evaluated while the application is running.
enum class ThemeMode : int { System = 0, Dark = 1, Light = 2 };
struct WorkspaceSettings { bool reduced_motion = false, power_saving = true; ThemeMode theme = ThemeMode::System; };
struct WorkspaceModel {
    std::vector<store::Host> hosts;
    std::vector<SessionView> sessions;
    std::optional<TrustRequest> trust;
    WorkspaceSettings settings;
    std::string notice;
    std::vector<ConnectionSettings> connection_settings;
    std::vector<FilePane> file_panes;
    std::vector<TransferTask> transfers;
    std::vector<MetricsView> metrics;
    // Updated by the controller after asynchronous device enumeration.
    std::vector<std::string> serial_ports;
    bool serial_ports_loading = false;
    std::string serial_ports_error;
};
// Callbacks must enqueue/nonblock. Credential ownership transfers to connect;
// its receiver must clear credentials after authentication. Only connect_saved
// may remember them according to ConnectRequest; never persist secrets in Host.
struct WorkspaceCallbacks {
    std::function<void(const store::Host&)> save_host;
    std::function<void(std::int64_t)> delete_host;
    std::function<void(const store::Host&, std::string password, std::string key_path, AuthMethod)> connect;
    std::function<void(std::uint64_t)> disconnect;
    std::function<void(std::uint64_t, std::string_view)> send;
    std::function<void(std::uint64_t, int columns, int rows)> resize;
    std::function<void(std::uint64_t, std::uint64_t, bool)> trust;
    std::function<void(WorkspaceSettings)> setting;
    // New facade callbacks enqueue work only. Receivers must wipe owned secrets.
    std::function<void(ConnectRequest)> connect_saved;
    std::function<void(std::int64_t, RememberPolicy)> save_connection_settings;
    std::function<void(std::uint64_t, bool remote, std::string path)> file_list;
    // Source/destination are full paths; never overwrite an existing destination.
    // Returns false for requests not added to the queue (including duplicates).
    std::function<bool(std::uint64_t, bool upload, std::string source, std::string destination)> file_transfer;
    std::function<void(std::uint64_t)> file_cancel;
    std::function<void(std::uint64_t)> file_clear_completed;
    std::function<void(FileOperationRequest)> file_operation;
    std::function<void(std::uint64_t, bool enabled)> monitor;
    std::function<void(net::SerialOptions)> serial_connect;
    // Enqueue enumeration off the UI thread; publish results through WorkspaceModel.
    std::function<void()> serial_refresh;
    std::function<void(std::uint64_t)> retry;
    // Open an independent SSH connection; return its new id, or 0 on refusal.
    std::function<std::uint64_t(std::uint64_t)> duplicate;
    // The receiver checks both ids and wipes its owned secret. Cancel uses disconnect.
    std::function<void(std::uint64_t, std::uint64_t, std::string)> answer_password;
    // Open a saved SSH device by id, using its bound credential or prompting first.
    // Returns the new session id, or 0 (including serial: use its explicit connect form).
    std::function<std::uint64_t(std::int64_t)> open_host;
    // Uses the view transfer ID, not the native task ID. True accepts a new request;
    // for submitted work, wait for the worker to confirm pause/resume before toggling again.
    std::function<bool(std::uint64_t transfer_id, bool paused)> file_set_paused;
};
}
namespace jianssh::ui {
struct WorkspaceFonts { ImFont* regular = nullptr; ImFont* heading = nullptr; ImFont* mono = nullptr; };
// Framebuffer clear colour matching the active theme, in linear 0..1 floats.
struct Rgba { float r = 0.f, g = 0.f, b = 0.f, a = 1.f; };
// Owns only transient presentation state; model and terminal remain facade-owned.
// draw runs on the ImGui/main thread. Terminal write callbacks must already be
// installed by the facade; input is encoded by Terminal, never sent twice.
class Workspace {
public:
    Workspace();
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    void draw(const app::WorkspaceModel&, const app::WorkspaceCallbacks&, WorkspaceFonts = {});
    [[nodiscard]] bool animation_active() const noexcept;
    // Previous frame's terminal input state, used before ImGui::NewFrame().
    [[nodiscard]] bool terminal_input_active() const noexcept;
    // Background of the current theme; the host clears the framebuffer with it
    // so no colour seam can appear at the window edge.
    [[nodiscard]] Rgba background_color() const noexcept;
    // True while the effective palette is the dark one. During a theme
    // transition this reports the palette that is currently on screen.
    [[nodiscard]] bool theme_is_dark() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
