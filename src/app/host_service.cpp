#include "jianssh/app/host_service.hpp"
#include "jianssh/store/database.hpp"
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <stdexcept>
#include <deque>
#include <mutex>
#include <thread>

namespace jianssh::app {
struct HostService::State {
    enum class Kind { Add, Update, Remove, Refresh, GetKey, SaveKey };
    struct Command {
        Kind kind; CommandId id = 0; Host host; std::string hostname;
        int port = 22; store::RawHostKey key;
        explicit Command(Kind k = Kind::Refresh) : kind(k) {}
    };
    std::filesystem::path path;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    // C++20 atomic<shared_ptr> is not implemented by the pinned Zig libc++.
    // Use the standardized shared_ptr atomic free functions instead.
    std::shared_ptr<const HostSnapshot> published{std::make_shared<const HostSnapshot>()};
    std::shared_ptr<const HostSnapshot> load_snapshot() const noexcept {
        return std::atomic_load_explicit(&published, std::memory_order_acquire);
    }
    void publish(std::shared_ptr<const HostSnapshot> value) noexcept {
        std::atomic_store_explicit(&published, std::move(value), std::memory_order_release);
    }
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Command> queue;
    std::vector<HostResult> results;
    CommandId next = 1;
    std::size_t outstanding = 0;
    static constexpr std::size_t capacity = 256;
    explicit State(std::filesystem::path p) : path(std::move(p)) { results.reserve(capacity); }
    CommandId submit(Command command) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock || stop.load() || outstanding >= capacity) return 0;
        command.id = next++;
        auto id = command.id;
        queue.push_back(std::move(command)); ++outstanding;
        lock.unlock(); wake.notify_one(); return id;
    }
    void run() noexcept {
        // No exception is permitted to escape the background owner.
        try { work(); }
        catch (const std::exception& e) { publish_fatal(e.what()); }
        catch (...) { publish_fatal("存储后台线程发生未知错误"); }
        finished.store(true);
    }
    void publish_fatal(const char* message) noexcept {
        stop.store(true);
        try {
            auto snapshot = std::make_shared<HostSnapshot>(*load_snapshot());
            snapshot->error = message; ++snapshot->revision;
            publish(std::move(snapshot));
        } catch (...) { /* Allocation failure cannot be represented safely. */ }
    }
    void work() {
        std::unique_ptr<store::Database> database;
        HostSnapshot current;
        std::string init_error;
        try {
            database = std::make_unique<store::Database>(path);
            current.hosts = database->hosts(); current.ready = true;
        } catch(const std::exception& e) { init_error = e.what(); current.error = init_error; }
          catch(...) { init_error = "数据库初始化发生未知错误"; current.error = init_error; }
        ++current.revision; publish(std::make_shared<const HostSnapshot>(current));
        for (;;) {
            Command c;
            {
                std::unique_lock lock(mutex);
                // shutdown() cannot acquire this mutex on the UI thread.
                // A bounded wait also handles notify racing predicate-to-wait.
                wake.wait_for(lock, std::chrono::milliseconds(25), [&] { return stop.load() || !queue.empty(); });
                if (queue.empty()) {
                    if (stop.load()) break;
                    continue;
                }
                c = std::move(queue.front()); queue.pop_front();
            }
            HostResult result; result.command = c.id;
            try {
                if (!init_error.empty()) throw std::runtime_error(init_error);
                switch(c.kind) {
                case Kind::Add: result.host_id = database->add(c.host); break;
                case Kind::Update: database->update(c.host); result.host_id=c.host.id; break;
                case Kind::Remove: database->remove(c.host.id); result.host_id=c.host.id; break;
                case Kind::Refresh: break;
                case Kind::GetKey: result.known_host=database->known_host(c.hostname,c.port); break;
                case Kind::SaveKey: database->save_known_host(c.hostname,c.port,c.key); break;
                }
                if(c.kind == Kind::Add || c.kind == Kind::Update || c.kind == Kind::Remove || c.kind == Kind::Refresh)
                    current.hosts=database->hosts();
                current.error.clear();
            } catch(const std::exception& e) { result.error=e.what(); current.error=result.error; }
              catch(...) { result.error="存储操作发生未知错误"; current.error=result.error; }
            current.last_command=c.id; ++current.revision;
            publish(std::make_shared<const HostSnapshot>(current));
            { std::lock_guard lock(mutex); results.push_back(std::move(result)); }
        }
        // Database and its WAL/checkpoint are closed only here, never on UI.
    }
};
HostService::HostService(std::filesystem::path path) : state_(std::make_shared<State>(std::move(path))) {
    std::thread([s=state_] { s->run(); }).detach();
}
HostService::~HostService() { shutdown(); }
void HostService::shutdown() noexcept { state_->stop.store(true); state_->wake.notify_one(); }
bool HostService::stopped() const noexcept { return state_->finished.load(); }
std::shared_ptr<const HostSnapshot> HostService::snapshot() const noexcept { return state_->load_snapshot(); }
CommandId HostService::add(Host host) { State::Command c{State::Kind::Add}; c.host=std::move(host); return state_->submit(std::move(c)); }
CommandId HostService::update(Host host) { State::Command c{State::Kind::Update}; c.host=std::move(host); return state_->submit(std::move(c)); }
CommandId HostService::remove(std::int64_t id) { State::Command c{State::Kind::Remove}; c.host.id=id; return state_->submit(std::move(c)); }
CommandId HostService::refresh() { return state_->submit(State::Command{State::Kind::Refresh}); }
CommandId HostService::get_known_host(std::string host,int port) { State::Command c{State::Kind::GetKey}; c.hostname=std::move(host); c.port=port; return state_->submit(std::move(c)); }
CommandId HostService::save_known_host(std::string host,int port,store::RawHostKey key) { State::Command c{State::Kind::SaveKey}; c.hostname=std::move(host); c.port=port; c.key=std::move(key); return state_->submit(std::move(c)); }
std::vector<HostResult> HostService::drain_results() {
    std::unique_lock lock(state_->mutex,std::try_to_lock);
    if(!lock) return {};
    std::vector<HostResult> result; result.swap(state_->results);
    state_->outstanding-=result.size(); return result;
}
} // namespace jianssh::app
