// Linux controller integration test; run through test_session_recovery.py.
#include "jianssh/app/workspace_controller.hpp"
#include "jianssh/app/terminal.hpp"
#include "imgui_internal.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace app = jianssh::app;

// Deliver real mouse events through ImGui, without opening an SDL window.
enum class Target { Footer, FirstTab, HostName, HostAddress };
static std::uint64_t double_click(app::WorkspaceController& controller, Target target) {
    ImGui::CreateContext();
    struct Cleanup { ~Cleanup() { ImGui::DestroyContext(); } } cleanup;
    auto& io = ImGui::GetIO();
    io.DisplaySize = {1100, 700}; io.DeltaTime = 1.f/60; io.IniFilename = nullptr;
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsAlpha8(&pixels, &w, &h);
    jianssh::ui::Workspace workspace;
    auto callbacks = controller.callbacks();
    std::uint64_t copy = 0;
    int calls = 0;
    callbacks.duplicate = [&](std::uint64_t id) { ++calls; return copy = controller.callbacks().duplicate(id); };
    callbacks.open_host = [&](std::int64_t id) { ++calls; return copy = controller.callbacks().open_host(id); };
    auto frame = [&] { controller.tick(); ImGui::NewFrame(); workspace.draw(controller.model(), callbacks); ImGui::Render(); };
    auto click = [&] { io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame(); };
    for (int i = 0; i < 4; ++i) frame();
    if (target == Target::Footer) io.AddMousePosEvent(60, io.DisplaySize.y - 26);
    else {
        const bool tab = target == Target::FirstTab;
        ImGuiWindow* rail = nullptr;
        for (auto* w : ImGui::GetCurrentContext()->Windows)
            if (w->Active && std::string_view(w->Name).find(tab ? "/会话标签_" : "/设备滚动列表_") != std::string_view::npos) rail = w;
        if (!rail) throw std::runtime_error("click target not found");
        io.AddMousePosEvent(rail->WorkRect.Min.x + (tab ? 35 : 60),
            rail->WorkRect.Min.y + (target == Target::HostAddress ? 37 : 16));
    }
    frame();
    click(); if (calls) throw std::runtime_error("single click duplicated a session");
    click(); if (calls != 1) {
        auto* w = ImGui::GetCurrentContext()->HoveredWindow;
        std::cerr << "target=" << static_cast<int>(target) << " clicks=" << calls << " mouse=" << io.MousePos.x << "," << io.MousePos.y << " window=" << (w?w->Name: "none") << "\n";
        throw std::runtime_error("double click did not dispatch exactly once");
    }
    return copy;
}

int main(int argc, char** argv) try {
    if (argc != 3) throw std::runtime_error("expected loopback port and temporary data directory");
    app::WorkspaceController controller(std::filesystem::path(argv[2]) / "jianssh.db");
    const auto& cb = controller.callbacks();
    const auto& model = controller.model();
    auto wait = [&](auto predicate, const char* message) {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        do {
            controller.tick();
            if (predicate()) return;
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        for (const auto& s : model.sessions) std::cerr << s.id << ": " << s.status << " " << s.error << '\n';
        throw std::runtime_error(message);
    };
    auto connect = [&](const char* user) {
        app::ConnectRequest request;
        request.host.hostname = "127.0.0.1";
        request.host.port = std::stoi(argv[1]);
        request.host.username = user;
        request.password = "wrong-1";
        cb.connect_saved(std::move(request));
    };
    auto session = [&](std::uint64_t id) -> const app::SessionView& {
        for (const auto& s : model.sessions) if (s.id == id) return s;
        throw std::runtime_error("session unexpectedly missing");
    };
    auto prompt = [&](std::uint64_t id, std::uint64_t previous = 0) {
        wait([&] { auto q = session(id).password_request_id; return q && q != previous; }, "missing password prompt");
        return session(id).password_request_id;
    };
    connect("recover");
    wait([&] { return model.trust.has_value(); }, "missing initial trust prompt");
    const auto original = model.trust->session_id;
    cb.trust(original, model.trust->request_id, true);
    const auto first = prompt(original);
    cb.answer_password(original, first, "wrong-2");
    const auto second = prompt(original, first);
    cb.answer_password(original, first, "stale-answer");
    cb.answer_password(original, second, "fixture-ok");
    wait([&] { return session(original).connected; }, "corrected password did not open a shell");

    const auto copy = double_click(controller, Target::Footer);
    if (!copy || copy == original) throw std::runtime_error("no independent session");
    wait([&] { return model.sessions.size() == 2; }, "missing new tab");
    cb.answer_password(copy, prompt(copy), "fixture-ok");
    wait([&] { return session(copy).connected && session(original).connected; }, "copy disturbed original");
    cb.send(original, "drop-fixture");
    wait([&] { return session(original).can_retry; }, "transport loss did not enable retry");
    if (cb.duplicate(original)) throw std::runtime_error("duplicated a disconnected tab");
    cb.retry(original);
    cb.retry(original);  // A second click must not start another transport.
    cb.answer_password(original, prompt(original), "fixture-ok");
    wait([&] { return model.sessions.size() == 2 && session(original).connected; }, "retry did not restore tab");
    cb.send(copy, "still-alive");
    wait([&] { return session(copy).terminal->text().find("still-alive") != std::string::npos; }, "other shell was lost");

    connect("withdraw");
    wait([&] { return model.sessions.size() == 3 && model.sessions.back().can_retry; }, "peer withdrew password auth but client kept prompting");
    if (model.sessions.back().password_request_id) throw std::runtime_error("stale password prompt after refusal");
    connect("timeout");
    wait([&] { return model.sessions.size() == 4 && model.sessions.back().password_request_id; }, "missing prompt before peer timeout");
    wait([&] { const auto& s = model.sessions.back(); return s.can_retry && !s.password_request_id; }, "dead peer left an active password prompt");

    // Persist only in the test process. The rejected value must never be
    // reused when duplicating a session with a remembered credential.
    app::ConnectRequest remembered;
    remembered.host.hostname = "127.0.0.1";
    remembered.host.port = std::stoi(argv[1]);
    remembered.host.username = "remember";
    remembered.password = "wrong-1";
    remembered.save_to_workspace = remembered.save_password = true;
    remembered.policy = app::RememberPolicy::Session;
    cb.connect_saved(std::move(remembered));
    wait([&] { return model.sessions.size() == 5 && model.sessions.back().password_request_id; }, "missing saved-session prompt");
    const auto saved = model.sessions.back().id;
    cb.answer_password(saved, session(saved).password_request_id, "fixture-ok");
    wait([&] { return session(saved).connected && !model.connection_settings.empty() && model.connection_settings.front().has_saved_password; }, "corrected credential not remembered");
    auto auto_copy = cb.duplicate(saved);
    wait([&] { return model.sessions.size() == 6 && session(auto_copy).connected; }, "remembered session required a new password");
    for (auto target : {Target::HostName, Target::HostAddress, Target::FirstTab}) {
        const auto count = model.sessions.size();
        const auto id = double_click(controller, target);
        if (!id) throw std::runtime_error("name/address did not create a session");
        wait([&] { return model.sessions.size() == count + 1; }, "double-click did not add one tab");
        // First tab is the transient 'recover' connection (not the active/last
        // tab or the saved 'remember' device). Its password must be requested.
        if (target == Target::FirstTab) cb.answer_password(id, prompt(id), "fixture-ok");
        wait([&] { return session(id).connected && session(saved).connected; }, "name/address connection failed");
    }
    // Home (no open tabs): double-click the saved address with Never policy.
    // Create the new tab and prompt locally without sending a blank password.
    auto host_id = model.hosts.front().id;
    cb.save_connection_settings(host_id, app::RememberPolicy::Never);
    for (const auto& s : model.sessions) cb.disconnect(s.id);
    wait([&] { return model.sessions.empty(); }, "did not return to the homepage");
    const auto home_id = double_click(controller, Target::HostAddress);
    if (!home_id) throw std::runtime_error("homepage double click did not open the device");
    cb.answer_password(home_id, prompt(home_id), "fixture-ok");
    wait([&] { return session(home_id).connected; }, "homepage shortcut did not connect");
    controller.begin_shutdown();
    wait([&] { return controller.shutdown_complete(); }, "shutdown stalled");
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
