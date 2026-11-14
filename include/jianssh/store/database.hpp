#pragma once
#include "jianssh/store/host.hpp"
#include <filesystem>
#include <memory>

namespace jianssh::store {
// Blocking API: use ONLY on the storage owner thread. Throws on failure;
// HostService catches all storage exceptions and publishes error results.
// Schema v1: hosts + ordered tags. v2 adds known_hosts (raw SSH key blobs).
class Database {
public:
    // Use a dedicated application directory: the immediate parent is secured
    // to 0700 on POSIX; database, WAL and SHM are 0600. Symlinked files and
    // foreign-owned/non-regular files are rejected. Windows ACLs are inherited.
    explicit Database(const std::filesystem::path& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    std::vector<Host> hosts();
    std::int64_t add(const Host& host);
    void update(const Host& host);
    void remove(std::int64_t id);
    std::optional<RawHostKey> known_host(const std::string& hostname, int port);
    // Caller must first obtain explicit trust confirmation. Atomically inserts
    // absent keys, accepts identical existing keys, rejects changed keys.
    void save_known_host(const std::string& hostname, int port, const RawHostKey& key);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace jianssh::store
