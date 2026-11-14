#pragma once
// Desktop light/dark preference detection.
//
// The application links neither Xlib nor DBus, so the preference is read from
// the configuration files the desktop environments already write, plus the
// GNOME gsettings value when it is reachable. The probe can be slow (it may
// fork gsettings), therefore it runs on a helper thread and the render thread
// only ever reads a cached answer.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace jianssh::ui {

// The desktop preference, or Unknown when the desktop does not publish one.
enum class SystemTheme { Unknown, Light, Dark };

namespace system_theme_detail {

inline std::string trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

inline bool contains(const std::string& haystack, const char* needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

// Read "key = value" from an ini style file, ignoring section headers.
inline bool read_ini_value(const std::filesystem::path& file, const char* key, std::string& out) {
    std::ifstream in(file);
    if (!in) return false;
    std::string line;
    const std::string wanted(lower(key));
    while (std::getline(in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '[' || trimmed.front() == '#') continue;
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(trimmed.substr(0, eq))) != wanted) continue;
        out = trim(trimmed.substr(eq + 1));
        return true;
    }
    return false;
}

inline std::filesystem::path config_home() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg);
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config";
    return {};
}

inline std::string read_command(const char* command) {
    std::string out;
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> pipe(popen(command, "r"), pclose);
    if (!pipe) return out;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), pipe.get())) out += buffer;
    return out;
}

inline bool gnome_desktop() {
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (!desktop || !*desktop) return false;
    return contains(desktop, "gnome") || contains(desktop, "unity") || contains(desktop, "pantheon") ||
           contains(desktop, "ubuntu");
}

} // namespace system_theme_detail

// Blocking probe. Exposed for tests and for the helper thread.
inline SystemTheme probe_system_theme() {
    using namespace system_theme_detail;
    // An explicit override always wins; it also keeps headless runs quiet.
    if (const char* forced = std::getenv("JIANSSH_THEME"); forced && *forced) {
        const std::string value = lower(forced);
        if (value == "dark") return SystemTheme::Dark;
        if (value == "light") return SystemTheme::Light;
    }
    // GTK_THEME may carry a variant suffix such as "Adwaita:dark".
    if (const char* gtk = std::getenv("GTK_THEME"); gtk && *gtk) {
        const std::string value = lower(gtk);
        if (value.find("dark") != std::string::npos) return SystemTheme::Dark;
        if (value.find("light") != std::string::npos) return SystemTheme::Light;
    }
    const std::filesystem::path home = config_home();
    if (!home.empty()) {
        for (const char* version : {"gtk-4.0", "gtk-3.0"}) {
            std::string value;
            const auto file = home / version / "settings.ini";
            if (read_ini_value(file, "gtk-application-prefer-dark-theme", value)) {
                if (value == "1" || lower(value) == "true" || lower(value) == "yes") return SystemTheme::Dark;
                if (value == "0" || lower(value) == "false" || lower(value) == "no") return SystemTheme::Light;
            }
        }
        std::string widget_style;
        if (read_ini_value(home / "kdeglobals", "widgetStyle", widget_style)) {
            if (contains(widget_style, "dark")) return SystemTheme::Dark;
            if (contains(widget_style, "light")) return SystemTheme::Light;
        }
    }
    // GNOME keeps the preference in dconf; ask gsettings only on GNOME.
    if (gnome_desktop()) {
        const std::string scheme = lower(read_command("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null"));
        if (scheme.find("prefer-dark") != std::string::npos) return SystemTheme::Dark;
        if (scheme.find("prefer-light") != std::string::npos) return SystemTheme::Light;
        const std::string theme = lower(read_command("gsettings get org.gnome.desktop.interface gtk-theme 2>/dev/null"));
        if (theme.find("dark") != std::string::npos) return SystemTheme::Dark;
        if (theme.find("light") != std::string::npos) return SystemTheme::Light;
    }
    return SystemTheme::Unknown;
}

namespace system_theme_detail {

struct Cache {
    std::atomic<int> state{static_cast<int>(SystemTheme::Unknown)};
    std::atomic<std::uint64_t> revision{0};
    std::once_flag started;
};

inline Cache& cache() {
    static Cache instance;
    return instance;
}

// The helper thread owns every slow operation; the render thread only reads.
inline void refresh(Cache& c) {
    const auto state = static_cast<int>(probe_system_theme());
    const int previous = c.state.exchange(state);
    if (previous != state) c.revision.fetch_add(1);
}

// Starts the helper thread exactly once; it keeps the cached answer fresh.
inline void ensure_started(Cache& c) {
    std::call_once(c.started, [] {
        std::thread worker([] {
            auto& cache = system_theme_detail::cache();
            for (;;) {
                system_theme_detail::refresh(cache);
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        });
        worker.detach();
    });
}

} // namespace system_theme_detail

// Cached answer for the render thread. Starts the helper thread on first use.
inline SystemTheme system_theme() {
    auto& c = system_theme_detail::cache();
    system_theme_detail::ensure_started(c);
    return static_cast<SystemTheme>(c.state.load());
}

// Convenience view of the cached answer.
inline bool system_prefers_dark() { return system_theme() == SystemTheme::Dark; }

// Increments whenever the detected preference changes, so the host loop can
// ask for a repaint even while the window is idle.
inline std::uint64_t system_theme_revision() {
    auto& c = system_theme_detail::cache();
    system_theme_detail::ensure_started(c);
    return c.revision.load();
}
} // namespace jianssh::ui
