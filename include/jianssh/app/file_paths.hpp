#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace jianssh::app {
// SFTP paths use '/' even when the client runs on Windows. Backslash is an
// ordinary name character on POSIX; it is never copied into a Windows leaf.
inline std::string utf8_path(const std::filesystem::path& path) {
    const auto text=path.u8string();
    return {text.begin(),text.end()};
}
inline std::filesystem::path native_file_path(std::string_view text) {
#ifdef _WIN32
    return std::filesystem::path(std::u8string(text.begin(),text.end()));
#else
    return std::filesystem::path(text);
#endif
}

inline bool valid_file_leaf(std::string_view name,bool windows) {
    if(name.empty()||name=="."||name==".."||name.find('/')!=name.npos||name.find('\0')!=name.npos) return false;
    if(!windows) return true;
    if(name.find_first_of("\\:<>\"|?*")!=name.npos||name.back()=='.'||name.back()==' ') return false;
    for(unsigned char ch:name) if(ch<32) return false;
    auto stem=name.substr(0,name.find('.'));
    while(!stem.empty()&&stem.back()==' ') stem.remove_suffix(1);
    std::string upper(stem);
    for(char& c:upper) if(c>='a'&&c<='z') c-=('a'-'A');
    if(upper=="CON"||upper=="PRN"||upper=="AUX"||upper=="NUL"||upper=="CONIN$"||upper=="CONOUT$") return false;
    if(upper.starts_with("COM")||upper.starts_with("LPT")) {
        auto n=std::string_view(upper).substr(3);
        if((n.size()==1&&n.front()>='0'&&n.front()<='9')||n=="¹"||n=="²"||n=="³") return false;
    }
    return true;
}

inline bool windows_file_path(bool remote) {
#ifdef _WIN32
    return !remote;
#else
    (void)remote;
    return false;
#endif
}

inline std::string file_child(const std::string& base,const std::string& name,bool remote) {
    if(windows_file_path(remote)) return utf8_path(native_file_path(base)/native_file_path(name));
    return base.empty()?name:base+(base.back()=='/'?"":"/")+name;
}

inline std::string file_parent(const std::string& path,bool remote) {
    if(windows_file_path(remote)) {
        const auto native=native_file_path(path);
        if(native==native.root_path()||native.relative_path().empty()) return path;
        auto parent=native.parent_path();
        // A UNC share is the highest browsable directory on that share.
        if(native.has_root_name()&&native.root_name().native().size()>2&&parent==native.root_path()) return path;
        return parent.empty()?path:utf8_path(parent);
    }
    const auto end=path.find_last_not_of('/');
    if(end==path.npos) return path;
    const auto pos=path.rfind('/',end);
    return pos==path.npos?path:pos==0?"/":path.substr(0,pos);
}
}
