#pragma once
#include <string>
#include <string_view>

namespace jianssh::app {
// Synchronous filesystem I/O: call from a worker. UTF-8 path; empty result
// means success, otherwise a Chinese error. This is only a format preflight:
// it does not verify a passphrase or cryptographic correctness.
std::string validate_private_key_file(std::string_view utf8_path);
} // namespace jianssh::app
