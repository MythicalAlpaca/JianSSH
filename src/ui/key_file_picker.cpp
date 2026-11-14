#include "key_file_picker.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <vector>
#else
#include <spawn.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
extern char** environ;
#endif
namespace jianssh::ui {
KeyFilePickerResult choose_key_file() {
#ifdef _WIN32
    std::vector<wchar_t> path(32768, 0);
    OPENFILENAMEW ofn{}; ofn.lStructSize=sizeof(ofn); ofn.lpstrFile=path.data();
    ofn.nMaxFile=static_cast<DWORD>(path.size()); ofn.lpstrTitle=L"选择 SSH 私钥文件";
    ofn.lpstrFilter=L"所有文件\0*.*\0\0"; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    if(!GetOpenFileNameW(&ofn)) return {{},CommDlgExtendedError()?"无法打开系统文件选择器。":""};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),-1,nullptr,0,nullptr,nullptr);
    if(n<=0) return {{},"文件路径无法转换为 UTF-8。"};
    std::string result(static_cast<std::size_t>(n),'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),-1,result.data(),n,nullptr,nullptr)) return {{},"文件路径无法转换为 UTF-8。"};
    result.pop_back(); return {result,{}};
#else
    int fds[2]; if(pipe2(fds,O_CLOEXEC)!=0) return {{},"无法创建文件选择器通信通道。"};
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)!=0) {close(fds[0]);close(fds[1]);return {{},"无法启动文件选择器。"};}
    posix_spawn_file_actions_adddup2(&actions,fds[1],STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions,fds[0]); posix_spawn_file_actions_addclose(&actions,fds[1]);
    char program[]="zenity", file[]="--file-selection", title[]="--title=选择 SSH 私钥文件";
    char* argv[]={program,file,title,nullptr}; pid_t pid;
    int rc=posix_spawnp(&pid,program,&actions,nullptr,argv,environ);
    posix_spawn_file_actions_destroy(&actions);close(fds[1]);
    if(rc) {close(fds[0]);return {{},"无法启动文件选择器，请安装 zenity 并确保图形桌面可用。"};}
    std::string out; char buffer[1024]; bool too_long=false; ssize_t count;
    while((count=read(fds[0],buffer,sizeof(buffer)))!=0) {
        if(count<0) {if(errno==EINTR)continue; break;}
        if(out.size()+static_cast<std::size_t>(count)>32768)too_long=true;
        else out.append(buffer,static_cast<std::size_t>(count));
    }
    close(fds[0]); int status=0; pid_t waited; do {waited=waitpid(pid,&status,0);} while(waited<0&&errno==EINTR);
    if(waited<0||!WIFEXITED(status)) return {{},"文件选择器异常退出。"};
    if(WEXITSTATUS(status)==1) return {{},"已取消选择，或文件选择器无法使用（请检查 zenity 和图形桌面）。"};
    if(WEXITSTATUS(status)!=0) return {{},"文件选择器失败，请检查 zenity 和图形桌面。"};
    if(too_long) return {{},"选择的路径过长。"};
    if(!out.empty()&&out.back()=='\n')out.pop_back();
    if(out.empty()||out.find('\n')!=std::string::npos) return {{},"无法识别所选文件路径。"};
    return {out,{}};
#endif
}
}
