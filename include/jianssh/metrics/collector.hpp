#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jianssh::metrics {
using Clock = std::chrono::steady_clock;
struct Cpu { std::string name; std::optional<double> percent; };
struct Memory { std::uint64_t total{}, available{}, used{}, cache{}, swap_total{}, swap_used{}; };
struct Network { std::string name; std::uint64_t rx_bytes{}, tx_bytes{}; std::optional<double> rx_bytes_per_second, tx_bytes_per_second; };
struct Disk { std::string filesystem, mount; std::uint64_t total{}, used{}, available{}; std::optional<double> percent; };
struct Process { std::uint64_t pid{}, resident_bytes{}; std::string command; std::optional<double> cpu_percent; };
struct HistoryPoint {
    Clock::time_point time{};
    std::optional<double> cpu_percent, memory_percent, rx_bytes_per_second, tx_bytes_per_second;
};
enum class Status { Waiting, Ready, Unsupported, Error };
struct Snapshot {
    Status status{Status::Waiting};
    bool stale{true}, sampling{};
    std::string error;
    Clock::time_point sampled_at{};
    std::optional<double> sample_interval_seconds, cpu_percent;
    std::vector<Cpu> cores;
    std::optional<double> load1, load5, load15;
    std::optional<Memory> memory;
    std::vector<Network> networks;
    std::vector<Disk> disks;
    std::vector<Process> top;
    std::deque<HistoryPoint> history;
};
// Thread-safe value snapshots. No network, timers, remote files or user-supplied shell text.
class Collector {
public:
    static constexpr std::size_t output_limit = 1024 * 1024;
    static constexpr std::size_t stderr_limit = 16 * 1024;
    static std::string_view script();
    bool begin_sample(); // Non-reentrant scheduling gate; consume/fail release it.
    bool consume(std::string_view output, Clock::time_point monotonic_now);
    void fail(std::string error, Clock::time_point monotonic_now);
    Snapshot snapshot() const;
private:
    struct Counters { std::uint64_t total{}, idle{}; std::array<std::uint64_t,8> ticks{}; };
    struct ProcCounters { std::uint64_t ticks{}, start{}; };
    mutable std::mutex mutex_;
    Snapshot value_;
    bool baseline_{};
    Clock::time_point previous_{};
    std::map<std::string, Counters> cpus_;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> nets_;
    std::map<std::uint64_t, ProcCounters> procs_;
    void fail_locked(std::string error, Clock::time_point now);
};
} // namespace jianssh::metrics
