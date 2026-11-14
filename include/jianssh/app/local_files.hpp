#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jianssh::app {
using LocalCommandId = std::uint64_t;
enum class LocalFileOperation { List, Mkdir, Rename, RemoveFile, Chmod };
enum class LocalFileError {
    None, InvalidPath, NotFound, PermissionDenied, AlreadyExists, NotDirectory,
    IsDirectory, SymlinkNotAllowed, InvalidPermissions, Unsupported, IoError
};
struct LocalFileEntry {
    std::string name;
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0; // Unix seconds, from lstat (the link itself).
    std::uint32_t permissions = 0; // POSIX mode bits, including special bits.
    bool is_directory = false;
    bool is_symlink = false;
};
struct LocalFilesSnapshot {
    std::filesystem::path path;
    std::vector<LocalFileEntry> entries;
    std::string error;
    LocalFileError error_code = LocalFileError::None;
    LocalCommandId last_command = 0;
    std::uint64_t revision = 0;
    bool truncated = false;
    bool ready = false;
};
struct LocalFileResult {
    LocalCommandId command = 0;
    LocalFileOperation operation = LocalFileOperation::List;
    std::filesystem::path path;
    std::string error;
    LocalFileError error_code = LocalFileError::None;
    // List completion owns its exact immutable listing, even after newer lists.
    std::shared_ptr<const LocalFilesSnapshot> snapshot;
};
// One background owner does ALL filesystem IO. Construction, submissions,
// snapshot(), drain_results(), shutdown(), and destruction never wait for IO.
// Paths must be absolute and NUL-free. Canonical resolution/checks run on the
// worker; operations canonicalize the parent but never follow the final link.
class LocalFiles {
public:
    static constexpr std::size_t max_entries = 10000;
    static constexpr std::size_t command_capacity = 64;
    LocalFiles();
    ~LocalFiles();
    LocalFiles(const LocalFiles&) = delete;
    LocalFiles& operator=(const LocalFiles&) = delete;
    std::shared_ptr<const LocalFilesSnapshot> snapshot() const noexcept;
    // 0 means busy/full/shutdown. Every admitted id gets one result. Undrained
    // results count against capacity: drain regularly before retrying admission.
    LocalCommandId list(std::filesystem::path path);
    LocalCommandId mkdir(std::filesystem::path path);
    LocalCommandId rename(std::filesystem::path from, std::filesystem::path to);
    // Caller MUST obtain explicit user confirmation. Never recursive; unlink
    // removes a symlink itself, including a link to a directory, never its target.
    LocalCommandId remove_file(std::filesystem::path path);
    // Rejects symlinks; only permission bits 0000..07777 are accepted.
    LocalCommandId chmod(std::filesystem::path path, std::uint32_t permissions);
    std::vector<LocalFileResult> drain_results();
    // Stops admission; drains all admitted commands without waiting for result
    // consumption. Poll stopped() and drain_results() before process teardown.
    void shutdown() noexcept;
    bool stopped() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace jianssh::app
