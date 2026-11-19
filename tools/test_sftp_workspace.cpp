// Linux integration test. Accepts a loopback OpenSSH port, user and client key.
// Use a disposable SSH server and an absolute, private temp directory; both
// sides of the test access subdirectories of that same directory.
#include "jianssh/app/workspace_controller.hpp"
#include "jianssh/app/file_paths.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs=std::filesystem;
namespace app=jianssh::app;
using namespace std::chrono_literals;

static void check(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}

int main(int argc,char** argv) try {
    check(argc==5,"usage: test_sftp_workspace PORT USER PRIVATE_KEY PRIVATE_TEST_DIR");
    const fs::path root=fs::canonical(argv[4]);
    const auto local=root/"local",remote=root/"remote";
    check(fs::create_directory(local)&&fs::create_directory(remote)&&fs::create_directory(root/"data"),"test directories must be fresh");
    fs::permissions(local,fs::perms::owner_all);
    const std::string leaf="中文\\文件";
    std::ofstream(local/leaf)<<"first payload";
    std::ofstream(local/"cancel.txt")<<"do not send";
    fs::create_directory(remote/"nested");
    app::WorkspaceController controller(root/"data"/"jianssh.db");
    const auto& cb=controller.callbacks();const auto& model=controller.model();
    auto wait=[&](auto test,const char* message) {
        const auto deadline=std::chrono::steady_clock::now()+20s;
        do {controller.tick();if(test())return;std::this_thread::sleep_for(5ms);}while(std::chrono::steady_clock::now()<deadline);
        std::cerr<<model.notice<<'\n';
        for(const auto& s:model.sessions)std::cerr<<s.status<<": "<<s.error<<'\n';
        for(const auto& p:model.file_panes)std::cerr<<p.requested_path<<": "<<p.error<<'\n';
        for(const auto& t:model.transfers)std::cerr<<t.source<<": "<<t.error<<'\n';
        throw std::runtime_error(message);
    };
    app::ConnectRequest request;
    request.host.hostname="127.0.0.1";request.host.port=std::stoi(argv[1]);request.host.username=argv[2];
    request.method=app::AuthMethod::Key;request.key_path=argv[3];
    cb.connect_saved(std::move(request));
    wait([&]{return model.trust.has_value();},"host key request was not delivered");
    const auto session=model.trust->session_id;
    cb.trust(session,model.trust->request_id,true);
    wait([&]{return !model.sessions.empty()&&model.sessions.front().connected;},"connection did not open");
    auto pane=[&](bool is_remote)->const app::FilePane& {
        const auto it=std::find_if(model.file_panes.begin(),model.file_panes.end(),[&](const auto& p){return p.session_id==session&&p.remote==is_remote;});
        check(it!=model.file_panes.end(),"pane not found");return *it;
    };
    cb.file_list(session,false,local.string());cb.file_list(session,true,(remote/"nested"/"..").string());
    wait([&]{return !pane(false).loading&&!pane(true).loading;},"listing did not finish");
    check(pane(false).path==local&&pane(true).path==remote,"directory not canonicalized");
    check(std::any_of(pane(false).entries.begin(),pane(false).entries.end(),[&](const auto& e){return e.name==leaf&&e.regular&&e.mtime>0;}),"local metadata missing");
    const auto revision=pane(true).revision;
    cb.file_list(session,true,(remote/"nonexistent").string());
    wait([&]{return !pane(true).loading;},"navigation failure not delivered");
    check(!pane(true).error.empty()&&pane(true).path==remote&&pane(true).revision==revision&&pane(true).entries.size()==1,"failed navigation replaced a valid listing");

    cb.file_transfer(session,true,(local/leaf).string(),(remote/leaf).string());
    cb.file_transfer(session,true,(local/leaf).string(),(remote/leaf).string());
    cb.file_transfer(session,true,(local/"cancel.txt").string(),(remote/"cancel.txt").string());
    check(model.transfers.size()==2,"duplicate admission");
    cb.file_cancel(model.transfers.back().id);
    wait([&]{return !app::transfer_active(model.transfers.front().state)&&!pane(true).loading;},"upload stalled");
    check(model.transfers.front().state==app::TransferState::Completed&&model.transfers.back().state==app::TransferState::Cancelled,"upload or cancellation failed");
    check(fs::exists(remote/leaf)&&!fs::exists(remote/"cancel.txt"),"unexpected upload destination contents");
    check(std::any_of(pane(true).entries.begin(),pane(true).entries.end(),[&](const auto& e){return e.name==leaf&&e.regular&&e.size==13;}),"target pane did not refresh");

    cb.file_clear_completed(session);check(model.transfers.empty(),"finished tasks were not cleared");
    const auto downloaded=local/"下载.dat";
    cb.file_transfer(session,false,(remote/leaf).string(),downloaded.string());
    cb.file_transfer(session,false,(remote/leaf).string(),(local/leaf).string());
    wait([&]{return std::none_of(model.transfers.begin(),model.transfers.end(),[](const auto& t){return app::transfer_active(t.state);});},"download stalled");
    if(model.transfers[0].state!=app::TransferState::Completed)std::cerr<<model.transfers[0].error<<'\n';
    check(model.transfers[0].state==app::TransferState::Completed&&model.transfers[1].state==app::TransferState::Failed,"download or overwrite protection failed");
    for(const auto& file:{downloaded,local/leaf}){
        std::ifstream in(file);std::string text;std::getline(in,text);check(text=="first payload","file contents changed");
    }
    cb.file_operation({session,true,app::FileOperation::Mkdir,(remote/"新目录").string(),{},0755});
    wait([&]{return !pane(true).operation_message.empty()&&!pane(true).loading;},"mkdir result missing");
    check(!pane(true).operation_failed&&fs::is_directory(remote/"新目录"),"remote mkdir failed");
    controller.begin_shutdown();wait([&]{return controller.shutdown_complete();},"shutdown stalled");
    std::cout<<"SFTP workspace integration passed\n";
    return 0;
} catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 1;
}
