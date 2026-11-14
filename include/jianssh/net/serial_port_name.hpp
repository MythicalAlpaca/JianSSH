#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace jianssh::net::detail {

inline bool is_serial_port_name(std::wstring_view name) noexcept {
    if (name.size() <= 3 || (name[0] != L'C' && name[0] != L'c') ||
        (name[1] != L'O' && name[1] != L'o') ||
        (name[2] != L'M' && name[2] != L'm')) return false;
    return std::all_of(name.begin() + 3, name.end(), [](wchar_t c) {
        return c >= L'0' && c <= L'9';
    });
}

inline std::wstring normalize_serial_port_name(std::wstring_view name) {
    if (!is_serial_port_name(name)) return {};
    std::wstring result(name);
    result[0] = L'C'; result[1] = L'O'; result[2] = L'M';
    return result;
}

// QueryDosDevice returns a bounded MULTI_SZ. Ignore a truncated final entry,
// and never inspect beyond the character count supplied by the API.
inline void append_serial_port_names_from_multisz(const wchar_t* data, std::size_t count,
                                                   std::vector<std::wstring>& result) {
    if (!data) return;
    std::size_t pos = 0;
    while (pos < count && data[pos] != L'\0') {
        std::size_t end = pos;
        while (end < count && data[end] != L'\0') ++end;
        if (end == count) break;
        auto name = normalize_serial_port_name(std::wstring_view(data + pos, end - pos));
        if (!name.empty()) result.push_back(std::move(name));
        pos = end + 1;
    }
}

inline std::wstring serial_port_device_path(std::wstring_view path) {
    constexpr std::wstring_view prefix = LR"(\\.\)";
    if (path.substr(0, prefix.size()) == prefix) return std::wstring(path);
    return std::wstring(prefix) + std::wstring(path);
}

inline bool serial_port_name_less(const std::wstring& a, const std::wstring& b) {
    auto an = std::wstring_view(a).substr(3), bn = std::wstring_view(b).substr(3);
    auto az = an.find_first_not_of(L'0'), bz = bn.find_first_not_of(L'0');
    if (az == std::wstring_view::npos) az = an.size() - 1;
    if (bz == std::wstring_view::npos) bz = bn.size() - 1;
    an.remove_prefix(az); bn.remove_prefix(bz);
    if (an.size() != bn.size()) return an.size() < bn.size();
    if (an != bn) return an < bn;
    // Equal numeric values with different zero padding still have a stable order.
    auto af = std::wstring_view(a).substr(3), bf = std::wstring_view(b).substr(3);
    if (af.size() != bf.size()) return af.size() < bf.size();
    return af < bf;
}

inline void sort_unique_serial_port_names(std::vector<std::wstring>& names) {
    for (auto& name : names) name = normalize_serial_port_name(name);
    std::erase(names, std::wstring{});
    std::sort(names.begin(), names.end(), serial_port_name_less);
    names.erase(std::unique(names.begin(), names.end()), names.end());
}

} // namespace jianssh::net::detail
