#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jianssh::store {
// Deliberately has no password/passphrase or other credential field.
struct Host {
    std::int64_t id = 0;
    std::string name;
    std::string hostname;
    int port = 22;
    std::string username;
    std::string group;
    std::vector<std::string> tags;
    std::string note;
    std::string auth = "password"; // password, publickey, agent; legacy keyboard-interactive is read-only until changed
    std::string key_path;
    // Connection-specific settings; existing hosts default to SSH.
    std::string connection_type = "ssh"; // ssh, serial
    std::string serial_port;
    unsigned serial_baud = 115200, serial_data_bits = 8, serial_stop_bits = 1;
    std::string serial_parity = "none"; // none, even, odd
    std::string serial_flow = "none"; // none, software, hardware
};
using RawHostKey = std::vector<std::uint8_t>;
} // namespace jianssh::store
