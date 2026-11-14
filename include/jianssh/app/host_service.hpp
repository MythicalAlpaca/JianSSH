#pragma once
#include "jianssh/store/host.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jianssh::app {
using Host = store::Host;
using CommandId = std::uint64_t;
struct HostSnapshot {
    std::vector<Host> hosts; // UI may freely search/filter this immutable list.
    bool ready = false;
    std::string error; // Latest operation/init error; cleared on next success.
    std::uint64_t revision = 0;
    CommandId last_command = 0;
};
struct HostResult {
    CommandId command = 0;
    std::string error;
    std::int64_t host_id = 0;
    // For get_known_host: absent = not trusted; present = exact raw key blob.
    std::optional<store::RawHostKey> known_host;
};
// Constructor/destructor and all UI methods do no filesystem/SQLite IO and
// never wait for the storage worker. One private owner thread does all IO.
// Destructor requests graceful shutdown; queued work completes on the worker.
// Destroy before tearing down process dependencies; no callbacks access UI.
class HostService {
public:
    explicit HostService(std::filesystem::path database_path);
    ~HostService();
    HostService(const HostService&) = delete;
    HostService& operator=(const HostService&) = delete;
    std::shared_ptr<const HostSnapshot> snapshot() const noexcept;
    // Stop accepting new commands, complete all admitted commands, then close
    // SQLite. Keep draining results and poll stopped() before process exit.
    void shutdown() noexcept;
    bool stopped() const noexcept;
    // Returns 0 when the bounded queue is full/busy/stopped: retry next frame.
    CommandId add(Host host);
    CommandId update(Host host);
    CommandId remove(std::int64_t id);
    CommandId refresh();
    CommandId get_known_host(std::string hostname, int port);
    // Only call after explicit user trust confirmation. Existing different
    // keys are rejected, never overwritten; await success before connecting.
    CommandId save_known_host(std::string hostname, int port, store::RawHostKey key);
    // Nonblocking; empty if busy. Drain regularly: undrained results apply
    // backpressure to command admission, rather than losing completions.
    std::vector<HostResult> drain_results();
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace jianssh::app
