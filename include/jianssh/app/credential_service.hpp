#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace jianssh::app {
using CredentialCommandId = std::uint64_t;
enum class CredentialPolicy { Never, Session, Encrypted };
enum class CredentialKind : std::uint32_t { Password = 1, KeyPassphrase = 2 };
// Move-only owned secret. Caller must move UI buffers here and erase originals.
// Destruction and assignment explicitly wipe active bytes; no implicit copies.
class CredentialSecret {
public:
    CredentialSecret() = default;
    explicit CredentialSecret(std::string_view value);
    ~CredentialSecret();
    CredentialSecret(CredentialSecret&&) noexcept;
    CredentialSecret& operator=(CredentialSecret&&) noexcept;
    CredentialSecret(const CredentialSecret&) = delete;
    CredentialSecret& operator=(const CredentialSecret&) = delete;
    const std::uint8_t* data() const noexcept { return bytes_.data(); }
    std::size_t size() const noexcept { return bytes_.size(); }
    void clear() noexcept;
private:
    std::vector<std::uint8_t> bytes_;
};
struct CredentialRef {
    std::string credential_id; // caller stable ID; nonempty, <=128 bytes; NOT a secret
    std::int64_t host_id = 0; // existing positive SQLite host ID, never hostname/user
    CredentialKind kind = CredentialKind::Password;
};
struct CredentialMetadata { CredentialRef ref; CredentialPolicy policy = CredentialPolicy::Never; };
// Compile-time platform capability. Windows has no DPAPI vault yet; callers
// must still be able to edit/delete hosts and connect without persistence.
inline constexpr bool credential_vault_supported() noexcept {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}
struct CredentialSnapshot {
    // Metadata only; automatically authenticated/loaded at initialization.
    // Session entries live until erase, policy change or shutdown.
    std::vector<CredentialMetadata> entries;
    bool ready = false;
    bool platform_supported = true; // false when this OS has no vault backend
    bool failed = false; // initialization was attempted and failed permanently
    bool exists = false;
    bool unlocked = false;
    bool stopped = false;
    std::uint64_t revision = 0;
    std::string error; // fixed sanitized messages only; never secret/paths
};
struct CredentialResult {
    CredentialCommandId command = 0;
    bool success = false;
    std::string error;
    // Only load returns a secret, exactly once via drain_results, never snapshot.
    CredentialSecret secret;
};
// Linux local-key storage: random key32 in <path>.key (0600), private directory
// (0700), XChaCha20Poly1305, random nonce24, tag16. No user master
// password, no idle/screen lock, no plaintext fallback; Windows unsupported.
// Both files are needed for recovery. This does NOT protect against the same OS
// user/root, malware, swap/core/debuggers, or rollback of an old valid file pair.
// Entire metadata/record set is authenticated, records additionally bind exact
// credential_id + positive host_id + kind. Caller must erase obsolete references
// after host deletion or endpoint/user/auth/key-path changes; never reuse IDs.
// One worker owns crypto/IO/session cache, lifetime exclusive advisory file lock.
// Bounded 64 outstanding commands/results; nonblocking calls return 0 on
// contention/full/stopped. Drain results to release backpressure.
class CredentialService {
public:
    explicit CredentialService(std::filesystem::path encrypted_path);
    ~CredentialService();
    CredentialService(const CredentialService&) = delete;
    CredentialService& operator=(const CredentialService&) = delete;
    std::shared_ptr<const CredentialSnapshot> snapshot() const noexcept;
    CredentialCommandId store(CredentialRef ref, CredentialPolicy policy, CredentialSecret secret);
    CredentialCommandId load(CredentialRef ref, CredentialPolicy policy);
    // Removes both session and encrypted forms for this exact host/kind/id.
    CredentialCommandId erase(CredentialRef ref);
    std::vector<CredentialResult> drain_results();
    void shutdown() noexcept;
    bool stopped() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace jianssh::app
