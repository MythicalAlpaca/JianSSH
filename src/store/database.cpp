#include "jianssh/store/database.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace jianssh::store {
namespace {
void check(int code, sqlite3* db) {
    (void)db;
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw std::runtime_error("数据库操作失败（代码 " + std::to_string(code) + "）");
}
void exec(sqlite3* db, const char* sql) { check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db); }
struct Statement {
    sqlite3* db; sqlite3_stmt* stmt = nullptr;
    Statement(sqlite3* d, const char* sql) : db(d) { check(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db); }
    ~Statement() { sqlite3_finalize(stmt); }
    void text(int i, const std::string& s) { check(sqlite3_bind_text(stmt, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT), db); }
    void number(int i, std::int64_t n) { check(sqlite3_bind_int64(stmt, i, n), db); }
    bool step() { int c = sqlite3_step(stmt); check(c, db); return c == SQLITE_ROW; }
    std::string text(int i) {
        const auto* p = sqlite3_column_text(stmt, i);
        return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(stmt, i)) : std::string{};
    }
};
struct Transaction {
    sqlite3* db; bool committed = false;
    explicit Transaction(sqlite3* d) : db(d) { exec(db, "BEGIN IMMEDIATE"); }
    void commit() { exec(db, "COMMIT"); committed = true; }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
};
void text_valid(const std::string& s, std::size_t max, bool required = false) {
    if (s.size() > max || s.find('\0') != std::string::npos ||
        (required && (s.empty() || std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }))))
        throw std::invalid_argument("设备字段无效或过长");
}
std::string identity(std::string host, int port) {
    text_valid(host, 1024, true);
    if (port < 1 || port > 65535) throw std::invalid_argument("端口必须在 1..65535 之间");
    if (std::any_of(host.begin(), host.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
        throw std::invalid_argument("主机名包含空白或控制字符");
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size()-2);
    for (char& c : host) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (host.size() > 1 && host.back() == '.') host.pop_back();
    if (host.empty()) throw std::invalid_argument("主机名不能为空");
    return host;
}
void validate(const Host& h) {
    text_valid(h.name, 512, true); text_valid(h.group, 512);
    text_valid(h.note, 65536); text_valid(h.key_path, 4096);
    if (h.connection_type == "ssh") {
        identity(h.hostname, h.port); text_valid(h.username, 256, true);
        if (h.auth != "password" && h.auth != "publickey" && h.auth != "agent")
            throw std::invalid_argument("不支持的认证方式");
        if (h.auth == "publickey") text_valid(h.key_path, 4096, true);
    } else if (h.connection_type == "serial") {
        text_valid(h.serial_port, 4096, true);
        if (!h.serial_baud || h.serial_baud > 4000000 || h.serial_data_bits < 5 || h.serial_data_bits > 8 ||
            (h.serial_stop_bits != 1 && h.serial_stop_bits != 2) ||
            (h.serial_data_bits == 5 && h.serial_stop_bits == 2) ||
            (h.serial_parity != "none" && h.serial_parity != "even" && h.serial_parity != "odd") ||
            (h.serial_flow != "none" && h.serial_flow != "software" && h.serial_flow != "hardware"))
            throw std::invalid_argument("串口参数无效");
    } else throw std::invalid_argument("不支持的连接类型");
    text_valid(h.hostname, 1024); text_valid(h.username, 256);
    text_valid(h.serial_port, 4096); text_valid(h.serial_parity, 16); text_valid(h.serial_flow, 16);
    if (h.tags.size() > 128) throw std::invalid_argument("标签数量过多");
    for (const auto& tag : h.tags) text_valid(tag, 256, true);
}
#ifndef _WIN32
void secure_file(const std::filesystem::path& path, bool create) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
    if (fd < 0) { if (!create && errno == ENOENT) return; throw std::runtime_error("无法打开专用数据库文件"); }
    struct stat st{};
    bool valid = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && st.st_nlink == 1 && ::fchmod(fd, 0600) == 0;
    ::close(fd);
    if (!valid) throw std::runtime_error("数据库文件的属主、类型或权限不安全");
}
#endif
}
struct Database::Impl {
    sqlite3* db = nullptr;
    ~Impl() { if (db) sqlite3_close(db); }
};
Database::Database(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    if (path.empty() || path.filename().empty() || path == ":memory:") throw std::invalid_argument("需要专用数据库文件路径");
    const auto absolute = std::filesystem::absolute(path);
    const auto parent = absolute.parent_path();
    std::filesystem::create_directories(parent);
#ifndef _WIN32
    struct stat st{};
    if (::lstat(parent.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || ::chmod(parent.c_str(), 0700))
        throw std::runtime_error("数据库目录的属主、类型或权限不安全");
    secure_file(absolute, true);
    secure_file(absolute.string() + "-wal", false);
    secure_file(absolute.string() + "-shm", false);
#endif
    auto* &db = impl_->db;
    check(sqlite3_open_v2(absolute.string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr), db);
    check(sqlite3_busy_timeout(db, 1000), db);
    exec(db, "PRAGMA foreign_keys=ON");
    { Statement s(db, "PRAGMA journal_mode=WAL"); if (!s.step() || s.text(0) != "wal") throw std::runtime_error("SQLite WAL 模式不可用"); }
    exec(db, "PRAGMA synchronous=FULL");
    Transaction migration(db);
    int version;
    { Statement s(db, "PRAGMA user_version"); s.step(); version = sqlite3_column_int(s.stmt, 0); }
    if (version < 0 || version > 3) throw std::runtime_error("不支持的数据库结构版本");
    if (version == 0) {
        exec(db, "CREATE TABLE hosts(id INTEGER PRIMARY KEY,name TEXT NOT NULL,hostname TEXT NOT NULL,port INTEGER NOT NULL CHECK(port BETWEEN 1 AND 65535),username TEXT NOT NULL,group_name TEXT NOT NULL DEFAULT '',note TEXT NOT NULL DEFAULT '',auth TEXT NOT NULL DEFAULT 'password',key_path TEXT NOT NULL DEFAULT '')");
        exec(db, "CREATE TABLE host_tags(host_id INTEGER NOT NULL REFERENCES hosts(id) ON DELETE CASCADE,position INTEGER NOT NULL,tag TEXT NOT NULL,PRIMARY KEY(host_id,position))");
        exec(db, "PRAGMA user_version=1");
    }
    if (version < 2) {
        exec(db, "CREATE TABLE known_hosts(hostname TEXT NOT NULL,port INTEGER NOT NULL CHECK(port BETWEEN 1 AND 65535),raw_key BLOB NOT NULL CHECK(length(raw_key)>0),PRIMARY KEY(hostname,port))");
        exec(db, "PRAGMA user_version=2");
    }
    if (version < 3) {
        exec(db, "ALTER TABLE hosts ADD COLUMN connection_type TEXT NOT NULL DEFAULT 'ssh'");
        exec(db, "ALTER TABLE hosts ADD COLUMN serial_port TEXT NOT NULL DEFAULT ''");
        exec(db, "ALTER TABLE hosts ADD COLUMN serial_baud INTEGER NOT NULL DEFAULT 115200");
        exec(db, "ALTER TABLE hosts ADD COLUMN serial_data_bits INTEGER NOT NULL DEFAULT 8");
        exec(db, "ALTER TABLE hosts ADD COLUMN serial_stop_bits INTEGER NOT NULL DEFAULT 1");
        exec(db, "ALTER TABLE hosts ADD COLUMN serial_parity TEXT NOT NULL DEFAULT 'none'");
        exec(db, "ALTER TABLE hosts ADD COLUMN serial_flow TEXT NOT NULL DEFAULT 'none'");
        exec(db, "PRAGMA user_version=3");
    }
    migration.commit();
#ifndef _WIN32
    secure_file(absolute.string() + "-wal", false);
    secure_file(absolute.string() + "-shm", false);
#endif
}
Database::~Database() = default;
std::vector<Host> Database::hosts() {
    auto* db = impl_->db;
    std::vector<Host> result;
    Statement s(db, "SELECT id,name,hostname,port,username,group_name,note,auth,key_path,connection_type,serial_port,serial_baud,serial_data_bits,serial_stop_bits,serial_parity,serial_flow FROM hosts ORDER BY id");
    while (s.step()) {
        Host h; h.id = sqlite3_column_int64(s.stmt,0); h.name=s.text(1); h.hostname=s.text(2); h.port=sqlite3_column_int(s.stmt,3);
        h.username=s.text(4); h.group=s.text(5); h.note=s.text(6); h.auth=s.text(7); h.key_path=s.text(8);
        h.connection_type=s.text(9); h.serial_port=s.text(10); h.serial_baud=static_cast<unsigned>(sqlite3_column_int64(s.stmt,11));
        h.serial_data_bits=static_cast<unsigned>(sqlite3_column_int64(s.stmt,12)); h.serial_stop_bits=static_cast<unsigned>(sqlite3_column_int64(s.stmt,13));
        h.serial_parity=s.text(14); h.serial_flow=s.text(15);
        Statement tags(db, "SELECT tag FROM host_tags WHERE host_id=? ORDER BY position"); tags.number(1,h.id);
        while(tags.step()) h.tags.push_back(tags.text(0));
        result.push_back(std::move(h));
    }
    return result;
}
namespace {
void bind_host(Statement& s, const Host& h) {
    s.text(1,h.name); s.text(2,h.hostname); s.number(3,h.port); s.text(4,h.username); s.text(5,h.group); s.text(6,h.note); s.text(7,h.auth); s.text(8,h.key_path);
    s.text(9,h.connection_type); s.text(10,h.serial_port); s.number(11,h.serial_baud); s.number(12,h.serial_data_bits); s.number(13,h.serial_stop_bits); s.text(14,h.serial_parity); s.text(15,h.serial_flow);
}
void put_tags(sqlite3* db, std::int64_t id, const std::vector<std::string>& tags) {
    int i=0;
    for(const auto& tag: tags) { Statement s(db,"INSERT INTO host_tags(host_id,position,tag) VALUES(?,?,?)"); s.number(1,id); s.number(2,i++); s.text(3,tag); s.step(); }
}
}
std::int64_t Database::add(const Host& h) {
    validate(h); if(h.id != 0) throw std::invalid_argument("新设备编号必须为零");
    auto* db=impl_->db; Transaction t(db);
    Statement s(db,"INSERT INTO hosts(name,hostname,port,username,group_name,note,auth,key_path,connection_type,serial_port,serial_baud,serial_data_bits,serial_stop_bits,serial_parity,serial_flow) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"); bind_host(s,h); s.step();
    auto id=sqlite3_last_insert_rowid(db); put_tags(db,id,h.tags); t.commit(); return id;
}
void Database::update(const Host& h) {
    validate(h); if(h.id<=0) throw std::invalid_argument("设备编号无效");
    auto* db=impl_->db; Transaction t(db);
    Statement s(db,"UPDATE hosts SET name=?,hostname=?,port=?,username=?,group_name=?,note=?,auth=?,key_path=?,connection_type=?,serial_port=?,serial_baud=?,serial_data_bits=?,serial_stop_bits=?,serial_parity=?,serial_flow=? WHERE id=?"); bind_host(s,h); s.number(16,h.id); s.step();
    if(sqlite3_changes(db)!=1) throw std::invalid_argument("设备不存在");
    Statement d(db,"DELETE FROM host_tags WHERE host_id=?"); d.number(1,h.id); d.step(); put_tags(db,h.id,h.tags); t.commit();
}
void Database::remove(std::int64_t id) {
    if(id<=0) throw std::invalid_argument("设备编号无效");
    auto* db=impl_->db; Transaction t(db); Statement s(db,"DELETE FROM hosts WHERE id=?"); s.number(1,id); s.step();
    if(sqlite3_changes(db)!=1) throw std::invalid_argument("设备不存在");
    t.commit();
}
std::optional<RawHostKey> Database::known_host(const std::string& hostname,int port) {
    auto host=identity(hostname,port); Statement s(impl_->db,"SELECT raw_key FROM known_hosts WHERE hostname=? AND port=?"); s.text(1,host); s.number(2,port);
    if(!s.step()) return std::nullopt;
    const auto* p=static_cast<const std::uint8_t*>(sqlite3_column_blob(s.stmt,0)); auto n=sqlite3_column_bytes(s.stmt,0);
    return RawHostKey(p,p+n);
}
void Database::save_known_host(const std::string& hostname,int port,const RawHostKey& key) {
    auto host=identity(hostname,port); if(key.empty() || key.size()>65536) throw std::invalid_argument("SSH 主机密钥长度无效");
    auto* db=impl_->db; Transaction t(db);
    // Compare under the same write transaction as the insert. A key changed
    // between UI confirmation and persistence must never silently replace trust.
    auto existing = known_host(host, port);
    if (existing) {
        if (*existing != key) throw std::runtime_error("SSH 主机密钥冲突，未替换已有信任记录");
        t.commit();
        return;
    }
    Statement s(db,"INSERT INTO known_hosts(hostname,port,raw_key) VALUES(?,?,?)");
    s.text(1,host); s.number(2,port); check(sqlite3_bind_blob(s.stmt,3,key.data(),static_cast<int>(key.size()),SQLITE_TRANSIENT),db); s.step(); t.commit();
}
} // namespace jianssh::store
