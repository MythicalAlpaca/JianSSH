// Linux controller + libssh2 integration test. Invoke through test_sftp_pause.py.
#include "jianssh/app/workspace_controller.hpp"
#include "jianssh/app/terminal.hpp"
#include "jianssh/net/ssh_session.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace app = jianssh::app;
namespace net = jianssh::net;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

template<class Predicate>
static void until(const char* why, Predicate predicate) {
    const auto deadline = Clock::now() + 10s;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(2ms);
    } while (Clock::now() < deadline);
    throw std::runtime_error(why);
}

static void no_temp(const fs::path& dir) {
    for (const auto& file : fs::directory_iterator(dir))
        require(!file.path().filename().string().starts_with(".jianssh-"), "temporary file leaked");
}

static void controller_cases(int port, const fs::path& root) {
    using S = app::TransferState;
    const auto local = root / "local", remote = root / "remote";
    app::WorkspaceController controller(root / "jianssh.db");
    const auto& cb = controller.callbacks();
    const auto& model = controller.model();
    auto wait = [&](const char* why, auto predicate) {
        until(why, [&] { controller.tick(); return predicate(); });
    };
    auto task = [&](uint64_t id) -> const app::TransferTask& {
        for (auto& t : model.transfers) if (t.id == id) return t;
        throw std::runtime_error("transfer vanished");
    };
    app::ConnectRequest request;
    request.host.hostname = "127.0.0.1"; request.host.port = port;
    request.host.username = "fixture"; request.password = "pause-test";
    cb.connect_saved(std::move(request));
    wait("missing trust prompt", [&] { return model.trust.has_value(); });
    auto sid = model.trust->session_id;
    cb.trust(sid, model.trust->request_id, true);
    wait("SSH not connected", [&] { return !model.sessions.empty() && model.sessions.front().connected; });
    cb.monitor(sid, false);

    auto start = [&](bool upload, fs::path from, fs::path to) {
        require(cb.file_transfer(sid, upload, from.string(), to.string()), "transfer admission failed");
        return model.transfers.back().id;
    };
    auto expect = [&](uint64_t id, S state) {
        wait("expected transfer state not observed", [&] {
            auto& t = task(id);
            if (t.state == state) return true;
            if (!app::transfer_active(t.state)) throw std::runtime_error("unexpected end: " + t.source + " " + t.error);
            return false;
        });
    };
    auto pause = [&](uint64_t id) {
        wait("no mid-file progress", [&] {
            auto& t = task(id);
            if (!app::transfer_active(t.state)) throw std::runtime_error("ended before pause: " + t.source + " " + t.error);
            return t.state == S::Running && t.completed_bytes && t.completed_bytes < t.total_bytes;
        });
        const auto before = task(id).completed_bytes;
        require(cb.file_set_paused(id, true), "running pause rejected");
        require(task(id).state == S::Pausing && !cb.file_set_paused(id, false), "pause request not serialized");
        expect(id, S::Paused);
        auto t = task(id);
        require(t.completed_bytes >= before && t.completed_bytes < t.total_bytes, "bad acknowledged offset");
        require(!cb.file_transfer(sid, t.upload, t.source, t.destination), "paused duplicate admitted");
        cb.file_clear_completed(sid);
        std::string token = "echo-paused-" + std::to_string(id);
        cb.send(sid, token + "\r");
        wait("paused transfer froze terminal", [&] {
            return model.sessions.front().terminal->text().find(token) != std::string::npos;
        });
        auto hold = Clock::now();
        wait("pause hold did not finish", [&] {
            require(task(id).state == S::Paused && task(id).completed_bytes == t.completed_bytes, "paused progress changed");
            return Clock::now() - hold >= 180ms;
        });
        require(!fs::exists(t.upload ? remote / fs::path(t.destination).filename() : fs::path(t.destination)), "paused file published");
        std::cout << (t.upload ? "upload" : "download") << " paused at " << t.completed_bytes << '/' << t.total_bytes << std::endl;
        return t.completed_bytes;
    };
    auto resume = [&](uint64_t id, uint64_t previous) {
        require(cb.file_set_paused(id, false), "resume rejected");
        require(task(id).state == S::Resuming && !cb.file_set_paused(id, true), "resume request not serialized");
        wait("resumed transfer never finished", [&] {
            auto& t = task(id);
            require(t.completed_bytes >= previous, "resumed offset regressed");
            previous = t.completed_bytes;
            if (t.state == S::Completed) { require(previous == t.total_bytes, "wrong final count"); return true; }
            if (!app::transfer_active(t.state)) throw std::runtime_error("resume failed: " + t.error);
            return false;
        });
        require(!cb.file_set_paused(id, true), "completed task accepted pause");
    };
    // Pause the queue head before dispatch; the second entry must remain runnable.
    auto queued = start(false, "/uploaded", local / "downloaded");
    require(cb.file_set_paused(queued, true), "queued pause rejected");
    auto upload = start(true, local / "source", "/uploaded");
    const auto checkpoint = pause(upload);
    auto blocked = start(true, local / "source", "/never-started");
    // The paused upload still owns the only transfer slot. A new task stays
    // in the facade (pause there is synchronous, unlike an admitted net job).
    controller.tick();
    require(cb.file_set_paused(blocked, true) && task(blocked).state == S::Paused, "paused job lost dispatch slot");
    cb.file_cancel(blocked);
    expect(blocked, S::Cancelled);
    resume(upload, checkpoint);
    require(task(queued).state == S::Paused && task(queued).completed_bytes == 0, "queued pause lost");
    require(cb.file_set_paused(queued, false), "queued resume rejected");
    require(task(queued).state == S::Queued, "queued resume was not synchronous");
    resume(queued, pause(queued));
    for (bool up : {true, false}) {
        auto id = up ? start(true, local / "source", "/cancelled") : start(false, "/uploaded", local / "cancelled");
        pause(id);
        cb.file_cancel(id);
        expect(id, S::Cancelled);
        require(!fs::exists((up ? remote : local) / "cancelled"), "cancel published file");
        no_temp(up ? remote : local);
    }
    auto dropped = start(false, "/uploaded", local / "drop-result");
    pause(dropped);
    cb.send(sid, "drop-transport");
    wait("lost transport retained resumable task", [&] {
        return !app::transfer_active(task(dropped).state) && model.sessions.front().can_retry;
    });
    require(!cb.file_set_paused(dropped, false) && !fs::exists(local / "drop-result"), "disconnected task resumed");
    no_temp(local);
    controller.begin_shutdown();
    wait("controller shutdown stalled", [&] { return controller.shutdown_complete(); });
    std::cout << "PASS: controller queue, resume, cancel, drop and terminal echo" << std::endl;
}

static void deadlines(int port, const fs::path& local) {
    using S = net::TaskState;
    net::ConnectOptions options;
    options.host = "127.0.0.1"; options.port = port; options.user = "fixture"; options.auth.password = "pause-test";
    net::SshSession ssh;
    require(ssh.connect(std::move(options)), "low-level connect refused");
    until("missing low-level trust prompt", [&] {
        auto req = ssh.snapshot().host_key_request; return req && ssh.answer_host_key(req->id, true);
    });
    until("low-level SSH failed", [&] { return ssh.snapshot().state == net::SessionState::Connected; });
    auto lookup = [&](uint64_t id) {
        for (auto& t : ssh.snapshot().tasks) if (t.id == id) return t;
        throw std::runtime_error("network task missing");
    };
    auto download = [&](std::string remote, std::string dest, std::chrono::milliseconds timeout) {
        net::TaskRequest r; r.kind = net::TaskKind::Download;
        r.remote_path = remote; r.local_path = (local / dest).string(); r.timeout = timeout;
        auto id = ssh.submit_task(std::move(r)); require(id != 0, "network admission failed"); return id;
    };
    auto id = download("/timed", "timed-result", 2s);
    until("timed task did not start", [&] { return lookup(id).transferred > 0; });
    require(ssh.set_task_paused(id, true), "network pause rejected");
    until("network pause not acknowledged", [&] { return lookup(id).state == S::Paused; });
    const auto checkpoint = lookup(id).transferred;
    auto queued = download("/timed", "never-created", 2s);
    require(ssh.set_task_paused(queued, true), "network queued pause rejected");
    until("queued pause not acknowledged", [&] { return lookup(queued).state == S::Paused; });
    require(ssh.cancel_task(queued), "queued paused cancellation rejected");
    until("queued cancellation blocked by paused head", [&] {
        for (auto& t : ssh.poll_tasks()) {
            require(t.id == queued && t.state == S::Cancelled, "wrong queued cancellation result");
            return true;
        }
        return false;
    });
    auto held = Clock::now();
    until("timed hold did not finish", [&] {
        auto t = lookup(id);
        require(t.state == S::Paused && t.transferred == checkpoint && ssh.poll_tasks().empty(), "pause expired");
        return Clock::now() - held > 2400ms;
    });
    require(ssh.set_task_paused(id, false), "network resume rejected after timeout interval");
    until("timed resume never completed", [&] {
        for (auto& t : ssh.poll_tasks()) {
            if (t.id != id || t.state != S::Succeeded) throw std::runtime_error("timed resume: " + t.error);
            return true;
        }
        return false;
    });
    auto slow = download("/slow", "expired", 200ms);
    until("active timeout disabled", [&] {
        for (auto& t : ssh.poll_tasks()) {
            require(t.id == slow && t.state == S::Failed && t.error.find("超时") != std::string::npos, "unexpected slow result");
            return true;
        }
        return false;
    });
    require(ssh.snapshot().state == net::SessionState::Connected && !fs::exists(local / "expired"), "timeout damaged transport");
    no_temp(local);
    ssh.disconnect();
    std::cout << "PASS: 2.4 s pause excluded from 2 s budget; unpaused 200 ms task timed out" << std::endl;
}

int main(int argc, char** argv) try {
    require(argc == 3, "run via test_sftp_pause.py");
    const auto port = std::stoi(argv[1]); const auto root = fs::canonical(argv[2]);
    controller_cases(port, root);
    deadlines(port, root / "local");
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
