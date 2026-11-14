#include "jianssh/app/local_files.hpp"
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

// Pinned Zig libc++ lacks atomic<shared_ptr>; the C++20 free functions are
// intentional (newer libstdc++ marks them deprecated ahead of C++26 removal).
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
namespace jianssh::app {
namespace {
struct Failure { LocalFileError code; const char* message; };
[[noreturn]] void fail(LocalFileError code, const char* message) { throw Failure{code,message}; }
#ifndef _WIN32
[[noreturn]] void posix_error(int e) {
    switch(e) {
    case ENOENT: fail(LocalFileError::NotFound,"路径不存在");
    case EACCES: case EPERM: fail(LocalFileError::PermissionDenied,"权限不足");
    case EEXIST: case ENOTEMPTY: fail(LocalFileError::AlreadyExists,"目标已存在");
    case ENOTDIR: fail(LocalFileError::NotDirectory,"不是目录");
    case EISDIR: fail(LocalFileError::IsDirectory,"不允许删除目录");
    case ELOOP: fail(LocalFileError::SymlinkNotAllowed,"此处不允许符号链接");
    case ENAMETOOLONG: case EINVAL: fail(LocalFileError::InvalidPath,"路径无效");
    case ENOSYS: case EOPNOTSUPP: fail(LocalFileError::Unsupported,"此文件系统不支持该安全操作");
    default: fail(LocalFileError::IoError,"本地文件操作失败");
    }
}
struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) { if(fd<0) posix_error(errno); }
    ~Fd() { if(value>=0) ::close(value); }
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
};
#else
[[noreturn]] void win_error(unsigned long e) {
    switch(e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: fail(LocalFileError::NotFound,"路径不存在");
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: fail(LocalFileError::PermissionDenied,"权限不足");
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: fail(LocalFileError::AlreadyExists,"目标已存在");
    case ERROR_DIR_NOT_EMPTY: fail(LocalFileError::AlreadyExists,"目标已存在");
    case ERROR_DIRECTORY: fail(LocalFileError::NotDirectory,"不是目录");
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_INVALID_NAME: fail(LocalFileError::InvalidPath,"路径无效");
    case ERROR_NOT_SUPPORTED: case ERROR_INVALID_FUNCTION: fail(LocalFileError::Unsupported,"此文件系统不支持该安全操作");
    default: fail(LocalFileError::IoError,"本地文件操作失败");
    }
}
std::string narrow(const std::filesystem::path& p) {
    const auto s=p.u8string();
    return std::string(s.begin(),s.end());
}
std::int64_t unix_seconds(std::filesystem::file_time_type t) {
    using namespace std::chrono;
    const auto sys=time_point_cast<seconds>(t-decltype(t)::clock::now()+system_clock::now());
    return sys.time_since_epoch().count();
}
#endif
[[noreturn]] void filesystem_error(const std::error_code& ec) {
#ifdef _WIN32
    win_error(static_cast<unsigned long>(ec.value()));
#else
    posix_error(ec.value());
#endif
}
void validate(const std::filesystem::path& p) {
    if(p.empty() || !p.is_absolute() || p.native().find('\0') != std::string::npos)
        fail(LocalFileError::InvalidPath,"需要不含空字符的绝对路径");
}
std::filesystem::path checked_canonical(const std::filesystem::path& p) {
    validate(p);
    std::error_code ec;
    auto result=std::filesystem::canonical(p,ec);
    if(ec) filesystem_error(ec);
    if(!result.is_absolute()) fail(LocalFileError::InvalidPath,"无法解析绝对路径");
    return result;
}
struct Target {
    std::filesystem::path parent;
    std::string leaf;
    explicit Target(const std::filesystem::path& p) {
        validate(p);
#ifdef _WIN32
        leaf=narrow(p.filename());
#else
        leaf=p.filename().native();
#endif
        if(leaf.empty() || leaf=="." || leaf=="..")
            fail(LocalFileError::InvalidPath,"需要非根目录的文件名");
        parent=checked_canonical(p.parent_path());
    }
};
#ifndef _WIN32
int open_parent(const Target& t) {
#ifdef __linux__
    return ::open(t.parent.c_str(),O_PATH|O_DIRECTORY|O_CLOEXEC);
#else
    return ::open(t.parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
#endif
}
void no_replace(int from_fd,const char* from,int to_fd,const char* to) {
#if defined(__linux__) && defined(SYS_renameat2)
    // RENAME_NOREPLACE is atomic, including when another process creates the
    // destination between validation and mutation. Never emulate with exists().
    if(::syscall(SYS_renameat2,from_fd,from,to_fd,to,1u)<0) posix_error(errno);
#else
    (void)from_fd; (void)from; (void)to_fd; (void)to;
    fail(LocalFileError::Unsupported,"当前文件系统不支持原子覆盖保护的重命名");
#endif
}
#endif
}
struct LocalFiles::State {
    struct Command {
        LocalFileOperation operation = LocalFileOperation::List;
        LocalCommandId id = 0;
        std::filesystem::path path, destination;
        std::uint32_t permissions = 0;
    };
    std::atomic<bool> stopping{false}, finished{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Command> commands;
    std::vector<LocalFileResult> results;
    std::size_t outstanding=0;
    LocalCommandId next=1;
    std::uint64_t revision=0;
    std::shared_ptr<const LocalFilesSnapshot> published=std::make_shared<const LocalFilesSnapshot>();
    State() { results.reserve(command_capacity); }
    LocalCommandId submit(Command c) {
        std::unique_lock lock(mutex,std::try_to_lock);
        if(!lock || stopping.load() || outstanding>=command_capacity) return 0;
        c.id=next++;
        if(next==0) ++next;
        const auto id=c.id;
        commands.push_back(std::move(c)); ++outstanding;
        lock.unlock(); wake.notify_one(); return id;
    }
    void listing(const Command& c,LocalFilesSnapshot& out) {
#ifndef _WIN32
        out.path=checked_canonical(c.path);
        struct Directory {
            DIR* p;
            explicit Directory(const std::filesystem::path& path) : p(::opendir(path.c_str())) { if(!p) posix_error(errno); }
            ~Directory() { ::closedir(p); }
        } dir(out.path);
        out.entries.reserve(256);
        for(;;) {
            errno=0;
            auto* item=::readdir(dir.p);
            if(!item) { if(errno) posix_error(errno); break; }
            const std::string name=item->d_name;
            if(name=="." || name=="..") continue;
            if(out.entries.size()==max_entries) { out.truncated=true; break; }
            struct stat st{};
            if(::fstatat(::dirfd(dir.p),name.c_str(),&st,AT_SYMLINK_NOFOLLOW)<0) {
                if(errno==ENOENT) continue; // Concurrent deletion is harmless.
                posix_error(errno);
            }
            LocalFileEntry entry;
            entry.name=name; entry.path=out.path/name;
            entry.size=st.st_size<0 ? 0 : static_cast<std::uint64_t>(st.st_size);
            entry.mtime=static_cast<std::int64_t>(st.st_mtime);
            entry.permissions=static_cast<std::uint32_t>(st.st_mode&07777);
            entry.is_directory=S_ISDIR(st.st_mode); entry.is_symlink=S_ISLNK(st.st_mode);
            out.entries.push_back(std::move(entry));
        }
        std::sort(out.entries.begin(),out.entries.end(),[](const auto& a,const auto& b) {
            if(a.is_directory!=b.is_directory) return a.is_directory;
            return a.name<b.name;
        });
        out.ready=true;
#else
        out.path=checked_canonical(c.path);
        std::error_code ec;
        std::filesystem::directory_iterator it(out.path,std::filesystem::directory_options::skip_permission_denied,ec);
        if(ec) filesystem_error(ec);
        out.entries.reserve(256);
        for(const auto& item:it) {
            if(out.entries.size()==max_entries) { out.truncated=true; break; }
            std::error_code item_ec;
            const auto status=item.symlink_status(item_ec);
            if(item_ec) { if(item_ec==std::errc::no_such_file_or_directory) continue; filesystem_error(item_ec); }
            LocalFileEntry entry;
            entry.name=narrow(item.path().filename());
            if(entry.name.empty()) continue;
            entry.path=out.path/item.path().filename();
            entry.is_symlink=std::filesystem::is_symlink(status);
            entry.is_directory=std::filesystem::is_directory(status);
            entry.permissions=entry.is_symlink?0777u:entry.is_directory?0755u:0644u;
            if(!entry.is_symlink) {
                const auto size=std::filesystem::file_size(item.path(),item_ec);
                if(!item_ec) entry.size=size;
                item_ec.clear();
                const auto mtime=std::filesystem::last_write_time(item.path(),item_ec);
                if(!item_ec) entry.mtime=unix_seconds(mtime);
            }
            out.entries.push_back(std::move(entry));
        }
        std::sort(out.entries.begin(),out.entries.end(),[](const auto& a,const auto& b) {
            if(a.is_directory!=b.is_directory) return a.is_directory;
            return a.name<b.name;
        });
        out.ready=true;
#endif
    }
    void mutation(const Command& c) {
#ifndef _WIN32
        Target target(c.path);
        Fd parent(open_parent(target));
        switch(c.operation) {
        case LocalFileOperation::Mkdir:
            if(::mkdirat(parent.value,target.leaf.c_str(),0777)<0) posix_error(errno);
            break;
        case LocalFileOperation::Rename: {
            Target destination(c.destination);
            Fd dest_parent(open_parent(destination));
            no_replace(parent.value,target.leaf.c_str(),dest_parent.value,destination.leaf.c_str());
            break;
        }
        case LocalFileOperation::RemoveFile: {
            struct stat st{};
            if(::fstatat(parent.value,target.leaf.c_str(),&st,AT_SYMLINK_NOFOLLOW)<0) posix_error(errno);
            if(S_ISDIR(st.st_mode)) fail(LocalFileError::IsDirectory,"不允许删除目录");
            // No AT_REMOVEDIR: even a concurrent replacement with a directory
            // cannot cause directory deletion. Symlinks are unlinked, not followed.
            if(::unlinkat(parent.value,target.leaf.c_str(),0)<0) posix_error(errno);
            break;
        }
        case LocalFileOperation::Chmod: {
            if(c.permissions>07777) fail(LocalFileError::InvalidPermissions,"权限必须在 0000..07777 范围内");
            // Pin without opening a device or requiring read permission. The
            // proc fd alias refers to this exact inode even if its name changes;
            // the user's final symlink is never followed. Fail closed if procfs
            // is unavailable. This also permits chmod(0000) followed by restore.
#ifdef __linux__
            Fd file(::openat(parent.value,target.leaf.c_str(),O_PATH|O_NOFOLLOW|O_CLOEXEC));
#else
            Fd file(::openat(parent.value,target.leaf.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
#endif
            struct stat st{};
            if(::fstat(file.value,&st)<0) posix_error(errno);
            if(S_ISLNK(st.st_mode)) fail(LocalFileError::SymlinkNotAllowed,"此处不允许符号链接");
            if(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
                fail(LocalFileError::Unsupported,"只能修改普通文件或目录的权限");
#ifdef __linux__
            const auto pinned=std::string("/proc/self/fd/")+std::to_string(file.value);
            if(::chmod(pinned.c_str(),static_cast<mode_t>(c.permissions))<0) posix_error(errno);
#else
            if(::fchmod(file.value,static_cast<mode_t>(c.permissions))<0) posix_error(errno);
#endif
            break;
        }
        case LocalFileOperation::List: break;
        }
#else
        Target target(c.path);
        const auto source=target.parent/target.leaf;
        switch(c.operation) {
        case LocalFileOperation::Mkdir: {
            std::error_code ec;
            if(!std::filesystem::create_directory(source,ec)) {
                if(ec) filesystem_error(ec);
                fail(LocalFileError::AlreadyExists,"目标已存在");
            }
            break;
        }
        case LocalFileOperation::Rename: {
            Target destination(c.destination);
            const auto target_path=destination.parent/destination.leaf;
            const auto from=source.native(), to=target_path.native();
            // No MOVEFILE_REPLACE_EXISTING: an existing destination is never overwritten.
            if(!::MoveFileExW(from.c_str(),to.c_str(),0)) win_error(::GetLastError());
            break;
        }
        case LocalFileOperation::RemoveFile: {
            std::error_code ec;
            const auto status=std::filesystem::symlink_status(source,ec);
            if(ec) filesystem_error(ec);
            if(std::filesystem::is_directory(status)) fail(LocalFileError::IsDirectory,"不允许删除目录");
            if(!std::filesystem::remove(source,ec)) {
                if(ec) filesystem_error(ec);
                fail(LocalFileError::NotFound,"路径不存在");
            }
            break;
        }
        case LocalFileOperation::Chmod:
            fail(LocalFileError::Unsupported,"Windows 下不支持修改权限");
        case LocalFileOperation::List: break;
        }
#endif
    }
    void run() noexcept {
        for(;;) {
            Command c;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock,std::chrono::milliseconds(25),[&] { return stopping.load() || !commands.empty(); });
                if(commands.empty()) { if(stopping.load()) break; else continue; }
                c=std::move(commands.front()); commands.pop_front();
            }
            LocalFileResult result;
            result.command=c.id; result.operation=c.operation; result.path=c.path;
            // Allocation exhaustion is process-fatal, as with std containers on
            // the UI. Operational exceptions always become correlated results.
            auto out=std::make_shared<LocalFilesSnapshot>();
            out->path=c.path; out->last_command=c.id; out->revision=++revision;
            try {
                if(c.operation==LocalFileOperation::List) listing(c,*out);
                else mutation(c);
            } catch(const Failure& e) { result.error=e.message; result.error_code=e.code; }
              catch(const std::filesystem::filesystem_error& e) {
                  try { filesystem_error(e.code()); }
                  catch(const Failure& f) { result.error=f.message; result.error_code=f.code; }
              }
              catch(...) { result.error="本地文件系统发生未知错误"; result.error_code=LocalFileError::IoError; }
            if(c.operation==LocalFileOperation::List) {
                out->error=result.error; out->error_code=result.error_code;
                result.snapshot=out;
                std::atomic_store_explicit(&published,std::shared_ptr<const LocalFilesSnapshot>(std::move(out)),std::memory_order_release);
            }
            { std::lock_guard lock(mutex); results.push_back(std::move(result)); }
        }
        finished.store(true,std::memory_order_release);
    }
};
LocalFiles::LocalFiles() : state_(std::make_shared<State>()) { std::thread([s=state_] { s->run(); }).detach(); }
LocalFiles::~LocalFiles() { shutdown(); }
std::shared_ptr<const LocalFilesSnapshot> LocalFiles::snapshot() const noexcept { return std::atomic_load_explicit(&state_->published,std::memory_order_acquire); }
void LocalFiles::shutdown() noexcept { state_->stopping.store(true); state_->wake.notify_one(); }
bool LocalFiles::stopped() const noexcept { return state_->finished.load(std::memory_order_acquire); }
LocalCommandId LocalFiles::list(std::filesystem::path p) { State::Command c; c.path=std::move(p); return state_->submit(std::move(c)); }
LocalCommandId LocalFiles::mkdir(std::filesystem::path p) { State::Command c; c.operation=LocalFileOperation::Mkdir; c.path=std::move(p); return state_->submit(std::move(c)); }
LocalCommandId LocalFiles::rename(std::filesystem::path p,std::filesystem::path to) { State::Command c; c.operation=LocalFileOperation::Rename; c.path=std::move(p); c.destination=std::move(to); return state_->submit(std::move(c)); }
LocalCommandId LocalFiles::remove_file(std::filesystem::path p) { State::Command c; c.operation=LocalFileOperation::RemoveFile; c.path=std::move(p); return state_->submit(std::move(c)); }
LocalCommandId LocalFiles::chmod(std::filesystem::path p,std::uint32_t bits) { State::Command c; c.operation=LocalFileOperation::Chmod; c.path=std::move(p); c.permissions=bits; return state_->submit(std::move(c)); }
std::vector<LocalFileResult> LocalFiles::drain_results() {
    std::unique_lock lock(state_->mutex,std::try_to_lock);
    if(!lock) return {};
    std::vector<LocalFileResult> results;
    results.swap(state_->results); state_->outstanding-=results.size();
    return results;
}
} // namespace jianssh::app
