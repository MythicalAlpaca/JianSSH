#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jianssh::net {

enum class AuthMethod { Password, Key, Agent };
enum class TaskKind { List, Stat, Mkdir, Rename, Remove, Chmod, Upload, Download, Exec };
enum class TaskState { Queued, Running, Succeeded, Failed, Cancelled, Paused };
struct FileInfo {
    std::string name;
    std::uint64_t size{}, mtime{};
    unsigned long permissions{};
};
struct TaskRequest {
    TaskKind kind{TaskKind::Stat};
    std::string remote_path, destination_path, local_path, command;
    bool overwrite{false}; // Explicit confirmation; never delete destination as fallback.
    unsigned long permissions{0700}; // Explicit mkdir/chmod mode; transfer temps are always 0600.
    std::chrono::milliseconds timeout{30000};
    std::size_t output_limit{256 * 1024}; // Per stdout/stderr; excess drained, not retained.
};
struct TaskSnapshot {
    std::uint64_t id{};
    TaskKind kind{};
    TaskState state{TaskState::Queued};
    std::uint64_t transferred{}, total{};
    std::vector<FileInfo> entries;
    std::string stdout_text, stderr_text, error;
    bool output_truncated{};
    int exit_status{};
    std::string listed_path;
    bool listing_truncated{};
    bool conflict{};
};
struct AuthConfig {
    AuthMethod method{AuthMethod::Password};
    std::string password;
    std::string private_key_file;
    std::string public_key_file; // Empty lets libssh2 derive the public key.
    std::string passphrase;
};
struct ConnectOptions {
    std::string host;
    std::uint16_t port{22};
    std::string user;
    AuthConfig auth;
    // Exact SSH wire-format host public key, NOT a fingerprint or OpenSSH line.
    // Empty means TOFU: no authentication until answer_host_key accepts.
    std::vector<std::uint8_t> expected_host_key;
    std::chrono::milliseconds connect_timeout{15000}; // Includes DNS + TCP + handshake.
    std::chrono::milliseconds operation_timeout{15000};
    std::chrono::milliseconds trust_timeout{120000};
    std::size_t task_capacity{32}; // Includes retained results; poll_tasks consumes terminal results.
    std::chrono::seconds keepalive_interval{15};
    std::size_t input_capacity{256 * 1024};
    std::size_t output_capacity{1024 * 1024};
    std::size_t event_capacity{64};
    int columns{80};
    int rows{24};
};
enum class SessionState { Disconnected, Connecting, AwaitingHostKey, Authenticating, AwaitingPassword, Connected, Disconnecting, Failed };
enum class ErrorCode { None, InvalidConfiguration, UnsupportedPlatform, Resolve, Network, Timeout, Cancelled, HostKeyMismatch, HostKeyRejected, Authentication, Protocol, Internal };
struct SessionError {
    ErrorCode code{ErrorCode::None};
    int native_code{};
    std::string message;
};
struct HostKeyRequest {
    std::uint64_t id{};
    std::string host;
    std::uint16_t port{};
    std::string algorithm;
    std::string sha256_fingerprint; // SHA256:<unpadded base64>
    std::vector<std::uint8_t> raw_key;
};
struct SessionSnapshot {
    SessionState state{SessionState::Disconnected};
    SessionError error;
    std::optional<HostKeyRequest> host_key_request;
    // Present only after a rejected password, while the peer still offers password auth.
    std::optional<std::uint64_t> password_request_id;
    std::vector<TaskSnapshot> tasks;
    std::size_t pending_input{};
    std::size_t available_output{};
    std::uint64_t dropped_events{};
    int exit_status{};
};
enum class EventType { StateChanged, HostKeyConfirmation, Error, RemoteClosed };
struct SessionEvent {
    EventType type{EventType::StateChanged};
    SessionState state{SessionState::Disconnected};
    SessionError error;
    std::optional<HostKeyRequest> host_key_request;
};

// Desktop implementation for Linux and Windows. All libssh2 objects and sockets
// belong to one background owner. Public methods are thread-safe; destroying the
// object requires callers to stop using it. No UI types, persistence, automatic
// reconnect or trust bypass.
class SshSession {
public:
    SshSession();
    ~SshSession();
    SshSession(const SshSession&) = delete;
    SshSession& operator=(const SshSession&) = delete;
    // Takes ownership of credentials; move options to avoid a caller-side copy.
    // Returns false if already running or options are invalid. Retry after a
    // terminal snapshot is allowed; unread old output/events are cleared.
    bool connect(ConnectOptions options);
    [[nodiscard]] SessionSnapshot snapshot() const;
    std::vector<SessionEvent> poll_events();
    // Binary-safe combined stdout/stderr PTY stream. Reading wakes backpressure.
    std::string read_output(std::size_t max_bytes = 64 * 1024);
    // Returns accepted prefix length; zero when disconnected/full. Never blocks.
    std::size_t write_input(std::string_view bytes);
    bool resize(int columns, int rows);
    // Only the current request ID is valid. Acceptance is session-local; the
    // caller must persist raw_key separately if durable trust is desired.
    bool answer_host_key(std::uint64_t request_id, bool accept);
    // Submit exactly one replacement password on this transport, or disconnect to cancel.
    bool answer_password(std::uint64_t request_id, std::string_view password);
    // Zero means rejected (invalid/disconnected/full). Serial auxiliary jobs,
    // one bounded network chunk per terminal pump pass; no recursive removal.
    // Download local_path MUST be an explicit absolute filename (no directory inference).
    // Its parent must be current-uid owned, not group/other writable (including
    // sticky shared dirs); symlink ancestors are rejected. Same-uid processes
    // are the permission trust boundary; fd/name identity is rechecked at publish.
    // Upload overwrite requires server posix-rename capability; unsupported is
    // an error, never a destructive fallback. Remote temporary files can remain
    // after transport loss (no unauthenticated/reconnected cleanup is attempted).
    // Downloads/upload source checks detect size/mtime changes, not same-metadata
    // adversarial rewrites. Local filesystem I/O is chunked, not real-time.
    std::uint64_t submit_task(TaskRequest request);
    // Set the desired state of a live upload/download on this connection. A true
    // return accepts the request; snapshot().tasks reports Paused only at a safe
    // boundary (completion can win the race). Paused queued jobs are skipped. A
    // paused active transfer retains its serial job slot and handles; the timeout
    // clock stops, terminal I/O continues. No resume after disconnect.
    bool set_task_paused(std::uint64_t id, bool paused);
    // Cancellation waits for any EAGAIN continuation, then closes task handles.
    // A stalled peer exceeding a five-second cleanup grace fails the whole
    // transport rather than reusing ambiguous protocol state (snapshot.error).
    bool cancel_task(std::uint64_t id);
    // Returns/removes completed results only; running progress is in snapshot().
    // Running snapshots omit stdout/stderr/entries (payload moved once at finish).
    std::vector<TaskSnapshot> poll_tasks();
    // Async, idempotent cancellation (also while resolving/awaiting trust).
    void disconnect();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace jianssh::net
