#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jianssh::net {
enum class SerialParity { None, Even, Odd };
enum class SerialFlow { None, Software, Hardware };
enum class SerialState { Disconnected, Connecting, Connected, Disconnecting, Failed };
struct SerialOptions {
    std::string port;
    unsigned baud{115200}, data_bits{8}, stop_bits{1};
    SerialParity parity{SerialParity::None};
    SerialFlow flow{SerialFlow::None};
    std::size_t input_capacity{256 * 1024}, output_capacity{1024 * 1024};
};
struct SerialSnapshot {
    SerialState state{SerialState::Disconnected};
    std::string error;
    int native_code{};
    std::size_t pending_input{}, available_output{}, discarded_input{};
};
// Synchronous device discovery: invoke on a background thread, never the UI thread.
std::vector<std::string> enumerate_serial_ports();
// Handle owned exclusively by a worker. Opening a port may change DTR/RTS and
// reset attached hardware, depending on the OS and driver. No data is sent on open.
class SerialSession {
public:
    SerialSession();
    ~SerialSession();
    SerialSession(const SerialSession&) = delete;
    SerialSession& operator=(const SerialSession&) = delete;
    bool connect(SerialOptions options);
    SerialSnapshot snapshot() const;
    std::string read_output(std::size_t max_bytes = 64 * 1024);
    // Returns accepted prefix; caller must retry the remainder. Never blocks on I/O.
    std::size_t write_input(std::string_view bytes);
    // Async; pending unsent bytes are discarded and counted in snapshot.
    void disconnect();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
