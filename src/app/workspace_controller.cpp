#include "jianssh/app/workspace_controller.hpp"
#include "jianssh/app/host_service.hpp"
#include "jianssh/app/terminal.hpp"
#include "jianssh/app/credential_service.hpp"
#include "jianssh/app/local_files.hpp"
#include "jianssh/app/file_paths.hpp"
#include "jianssh/metrics/collector.hpp"
#include "jianssh/net/ssh_session.hpp"
#include "jianssh/net/serial_session.hpp"
#include <mutex>
#include <thread>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>
#include <sys/stat.h>

#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0170000) == 0120000)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#endif

namespace jianssh::app {
namespace {
void wipe(std::string& s){volatile char*p=s.data();for(std::size_t i=0;i<s.size();++i)p[i]=0;s.clear();}
void clear_auth(net::ConnectOptions& o){wipe(o.auth.password);wipe(o.auth.passphrase);}
CredentialPolicy policy(RememberPolicy p){return static_cast<CredentialPolicy>(p);}
CredentialKind kind(AuthMethod m){return m==AuthMethod::Key?CredentialKind::KeyPassphrase:CredentialKind::Password;}
std::string binding(const Host& h){std::string v;for(auto&s:{h.hostname,std::to_string(h.port),h.username,h.auth,h.key_path}){v+=std::to_string(s.size())+":"+s;}unsigned char digest[32];if(mbedtls_sha256(reinterpret_cast<const unsigned char*>(v.data()),v.size(),digest,0))throw std::runtime_error("凭据绑定失败");wipe(v);std::string out;const char*hex="0123456789abcdef";for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;}
CredentialRef ref(const Host&h,CredentialKind k){return {"host:"+std::to_string(h.id)+(k==CredentialKind::Password?":password:":":passphrase:")+binding(h),h.id,k};}
std::string state_name(net::SessionState s){switch(s){case net::SessionState::Disconnected:return "已断开";case net::SessionState::Connecting:return "正在连接";case net::SessionState::AwaitingHostKey:return "等待主机验证";case net::SessionState::Authenticating:return "正在认证";case net::SessionState::Connected:return "已连接";case net::SessionState::Disconnecting:return "正在断开";case net::SessionState::Failed:return "连接失败";}return "未知状态";}
double seconds(metrics::Clock::time_point t){return std::chrono::duration<double>(t.time_since_epoch()).count();}
}
struct WorkspaceController::Impl {
 struct Session {
  std::uint64_t id{};Host host;std::unique_ptr<net::SshSession> ssh;std::unique_ptr<net::SerialSession> serial;std::string serial_port;net::SerialState serial_previous=net::SerialState::Disconnected;std::shared_ptr<Terminal> terminal;
  net::SessionState previous=net::SessionState::Disconnected;CommandId lookup{};net::ConnectOptions options;
  bool waiting=true,credential_wait=false,closing=false;std::string pending_input,local_error;
  CredentialSecret remember_secret;CredentialRef remember_ref;RememberPolicy remember_policy=RememberPolicy::Never;bool save_after_auth=false;
  metrics::Collector collector;bool monitor=false;int interval=2,failures=0;std::uint64_t monitor_task{};metrics::Clock::time_point next_sample{};std::map<double,std::optional<double>> disk_history;
  ~Session(){clear_auth(options);wipe(pending_input);}
 };
 struct TrustCommit{std::uint64_t session,request;};
 enum class JobKind{List,Transfer,Operation,Monitor};
  struct Job{JobKind kind;std::uint64_t generation{},transfer{};std::string path;};
  struct LocalJob{std::uint64_t session,generation;bool listing;std::string path;};
 enum class CredentialJobKind{Load,Store,Erase,MutationErase};
 struct CredentialJob{CredentialJobKind kind;std::uint64_t session{},epoch{},mutation{};std::int64_t host_id{};};
 struct Mutation{Host host;bool remove=false;std::size_t remaining{};bool failed=false;};
 HostService hosts;LocalFiles local;CredentialService credentials;WorkspaceModel view;WorkspaceCallbacks cb;
 std::vector<std::unique_ptr<Session>> sessions;
 std::map<CommandId,TrustCommit> trust_commits;std::map<CommandId,std::uint64_t> lookups;
 std::map<std::pair<std::uint64_t,std::uint64_t>,Job> jobs;
 std::map<LocalCommandId,LocalJob> local_jobs;
 std::map<std::pair<std::uint64_t,bool>,std::uint64_t> generations;
  std::map<std::uint64_t,std::pair<std::uint64_t,std::uint64_t>> transfer_jobs;
  std::set<std::tuple<std::uint64_t,bool,std::string>> pending_refresh;
 std::map<CredentialCommandId,CredentialJob> credential_jobs;
 std::map<std::uint64_t,Mutation> mutations;
 struct PendingConnect { ConnectRequest request; CredentialSecret secret; };
 std::map<CommandId,PendingConnect> pending_connections;
 struct SerialDiscovery {std::mutex mutex;bool done=false;std::vector<std::string> ports;std::string error;};
 std::shared_ptr<SerialDiscovery> serial_discovery;
 std::map<std::int64_t,RememberPolicy> policies;
 std::uint64_t next=1,next_transfer=1,next_mutation=1,revision=0,credential_revision=0,epoch=1;bool shutting_down=false,host_shutdown=false;
 explicit Impl(std::filesystem::path p):hosts(p),credentials(p.parent_path()/"credentials.enc"){
  cb.save_host=[this](const Host&h){mutate(h,false);};
  cb.delete_host=[this](std::int64_t id){auto*h=host(id);if(h)mutate(*h,true);};
  cb.connect=[this](const Host&h,std::string secret,std::string key,AuthMethod method){ConnectRequest r;r.host=h;r.password=std::move(secret);r.key_path=std::move(key);r.method=method;connect(std::move(r),false);};
  cb.connect_saved=[this](ConnectRequest r){connect(std::move(r));};
  cb.serial_connect=[this](net::SerialOptions o){connect_serial(std::move(o));};
  cb.serial_refresh=[this]{refresh_serial_ports();};
  cb.save_connection_settings=[this](std::int64_t id,RememberPolicy p){if(id<=0||!host(id))return;if(p==RememberPolicy::Never){if(!erase_for_host(id))return;for(auto&s:sessions)if(s->host.id==id){s->remember_secret.clear();s->save_after_auth=false;if(s->credential_wait){s->credential_wait=false;s->waiting=false;clear_auth(s->options);s->ssh->disconnect();s->local_error="凭据策略已更改，请重新连接";}}}policies[id]=p;};
  cb.disconnect=[this](std::uint64_t id){if(auto*s=find(id)){
   if(s->closing)return;
   clear_session_secrets(*s);
   s->waiting=false;s->credential_wait=false;s->monitor=false;s->closing=true;
   if(s->serial)s->serial->disconnect();else s->ssh->disconnect();if(!s->pending_input.empty())view.notice="会话已关闭，未发送输入已清除";wipe(s->pending_input);
   {
    for(auto i=lookups.begin();i!=lookups.end();)if(i->second==id)i=lookups.erase(i);else ++i;
    for(auto&t:view.transfers)if(t.session_id==id&&transfer_active(t.state))finish_transfer(t.id,TransferState::Cancelled,"会话已关闭");
    std::erase_if(jobs,[id](const auto&entry){return entry.first.first==id;});
    std::erase_if(local_jobs,[id](const auto&entry){return entry.second.session==id;});
    std::erase_if(generations,[id](const auto&entry){return entry.first.first==id;});
    std::erase_if(view.file_panes,[id](auto&p){return p.session_id==id;});
   }
  }};
  cb.send=[this](std::uint64_t id,std::string_view b){if(auto*s=find(id);s&&!s->closing)s->terminal->write_utf8(b);};
  cb.resize=[this](std::uint64_t id,int cols,int rows){if(auto*s=find(id)){cols=std::clamp(cols,10,400);rows=std::clamp(rows,2,200);if(s->terminal->cols()!=cols||s->terminal->rows()!=rows){s->terminal->resize(rows,cols);if(s->ssh)s->ssh->resize(cols,rows);}}};
  cb.trust=[this](std::uint64_t id,std::uint64_t request,bool accept){if(auto*s=find(id);s&&s->ssh){auto snap=s->ssh->snapshot();if(snap.host_key_request&&snap.host_key_request->id==request){
   if(accept){for(auto&[command,t]:trust_commits)if(t.session==id&&t.request==request)return;auto q=hosts.save_known_host(s->host.hostname,s->host.port,snap.host_key_request->raw_key);if(!q){view.notice="无法保存主机信任记录";s->ssh->answer_host_key(request,false);return;}trust_commits[q]={id,request};view.notice="正在提交主机信任记录，提交成功后才认证";}else s->ssh->answer_host_key(request,false);
  }}};
  cb.setting=[this](WorkspaceSettings s){view.settings=s;};
  cb.file_list=[this](std::uint64_t id,bool remote,std::string p){list(id,remote,std::move(p));};
   cb.file_transfer=[this](std::uint64_t id,bool upload,std::string source,std::string dest){return transfer(id,upload,std::move(source),std::move(dest));};
   cb.file_cancel=[this](std::uint64_t id){for(auto&t:view.transfers)if(t.id==id&&transfer_active(t.state)){
    auto it=transfer_jobs.find(id);
    if(it==transfer_jobs.end())finish_transfer(id,TransferState::Cancelled,"任务已取消");
    else if(auto*s=find(it->second.first);s&&s->ssh->cancel_task(it->second.second))t.cancel_requested=true;
    break;
   }};
   cb.file_clear_completed=[this](std::uint64_t id){std::erase_if(view.transfers,[id](const auto& t){return t.session_id==id&&!transfer_active(t.state);});};
  cb.file_operation=[this](FileOperationRequest r){operation(std::move(r));};
  cb.monitor=[this](std::uint64_t id,bool enabled){if(auto*s=find(id);s&&s->ssh){s->monitor=enabled;s->next_sample=metrics::Clock::now();if(!enabled&&s->monitor_task)s->ssh->cancel_task(s->monitor_task);}};
 }
 Host* host(std::int64_t id){for(auto&h:view.hosts)if(h.id==id)return &h;return nullptr;}
 Session* find(std::uint64_t id){for(auto&s:sessions)if(s->id==id)return s.get();return nullptr;}
 void clear_session_secrets(Session&s){clear_auth(s.options);s.remember_secret.clear();s.save_after_auth=false;}
 void track(CredentialCommandId id,CredentialJobKind kind,std::uint64_t session=0,std::uint64_t mutation=0){if(id){auto*s=find(session);credential_jobs.emplace(id,CredentialJob{kind,session,epoch,mutation,s?s->host.id:0});}else view.notice="口令存储队列繁忙，请重试";}
 std::vector<CredentialRef> refs_for(std::int64_t id){std::vector<CredentialRef> out;auto v=credentials.snapshot();for(auto&m:v->entries)if(m.ref.host_id==id)out.push_back(m.ref);return out;}
 bool erase_for_host(std::int64_t id){auto v=credentials.snapshot();if(!v->platform_supported||v->failed||!credential_vault_supported())return true;for(auto&[command,j]:credential_jobs)if(j.kind==CredentialJobKind::Store){if(j.host_id==id){view.notice="凭据保存尚未完成，请稍后清除";return false;}}if(!v->ready){view.notice="口令存储尚在初始化，请稍后重试";return false;}bool ok=true;for(auto&r:refs_for(id)){auto c=credentials.erase(r);if(!c)ok=false;track(c,CredentialJobKind::Erase);}return ok;}
 void mutate(const Host&h,bool remove){
  if(shutting_down)return;
  auto*old=host(h.id);bool sensitive=old&&(remove||old->connection_type!=h.connection_type||(old->connection_type=="ssh"&&binding(*old)!=binding(h)));
  if(!sensitive){auto c=h.id?hosts.update(h):hosts.add(h);if(!c)view.notice="设备存储队列繁忙";return;}
  auto v=credentials.snapshot();
  // No encryption backend means there can be no persistent credentials to erase;
  // host identity changes and deletions must still work.
  if(!v->platform_supported||!credential_vault_supported()){auto c=remove?hosts.remove(h.id):hosts.update(h);if(!c)view.notice="设备存储队列繁忙";return;}
  if(v->failed){auto c=remove?hosts.remove(h.id):hosts.update(h);if(!c)view.notice="设备存储队列繁忙";else view.notice=remove?"设备已删除；口令存储不可用，旧加密凭据未清除":"设备信息已更新；口令存储不可用，旧加密凭据未清除";return;}
  for(auto&[command,j]:credential_jobs)if(j.kind==CredentialJobKind::Store){if(j.host_id==h.id){view.notice="此设备的凭据保存尚未完成，请稍后再修改";return;}}
  if(!v->ready){view.notice="口令存储尚在初始化，请稍后再修改设备身份";return;}
  for(auto&s:sessions)if(s->host.id==h.id){clear_session_secrets(*s);if(s->credential_wait){s->credential_wait=false;s->waiting=false;s->ssh->disconnect();s->local_error="设备身份已更改，已取消旧凭据连接";}}
  auto refs=refs_for(h.id);if(refs.empty()){auto c=remove?hosts.remove(h.id):hosts.update(h);if(!c)view.notice="设备存储队列繁忙";return;}
  auto mutation=next_mutation++;mutations.emplace(mutation,Mutation{h,remove,refs.size(),false});
  for(auto&r:refs){auto c=credentials.erase(r);if(!c){auto&m=mutations.at(mutation);m.failed=true;--m.remaining;view.notice="清除旧凭据失败，设备未修改";}else track(c,CredentialJobKind::MutationErase,0,mutation);}
  finish_mutations();
 }
 void finish_mutations(){for(auto i=mutations.begin();i!=mutations.end();){if(i->second.remaining){++i;continue;}auto&m=i->second;if(!m.failed){auto c=m.remove?hosts.remove(m.host.id):hosts.update(m.host);if(!c){view.notice="凭据已清除，正在等待设备存储队列";++i;continue;}}i=mutations.erase(i);}}
 CredentialPolicy saved_policy(const CredentialRef& ref,RememberPolicy fallback){auto snapshot=credentials.snapshot();for(const auto& entry:snapshot->entries)if(entry.ref.credential_id==ref.credential_id&&entry.ref.host_id==ref.host_id&&entry.ref.kind==ref.kind)return entry.policy;return policy(fallback);}
 void refresh_serial_ports(){
  if(shutting_down||serial_discovery)return;
  auto task=std::make_shared<SerialDiscovery>();serial_discovery=task;view.serial_ports_loading=true;view.serial_ports_error.clear();
  try{std::thread([task]{std::vector<std::string> ports;std::string error;try{ports=net::enumerate_serial_ports();}catch(...){error="枚举串口失败";}std::lock_guard lock(task->mutex);task->ports=std::move(ports);task->error=std::move(error);task->done=true;}).detach();}catch(...){serial_discovery.reset();view.serial_ports_loading=false;view.serial_ports_error="无法启动串口枚举";}
 }
 void connect_serial(net::SerialOptions o){
  if(shutting_down||sessions.size()+pending_connections.size()>=10){view.notice="会话数量已达上限或应用正在关闭";return;}
  auto s=std::make_unique<Session>();s->id=next++;s->waiting=false;s->serial_port=o.port;s->serial=std::make_unique<net::SerialSession>();s->terminal=std::make_shared<Terminal>(24,80,2000);
  const auto id=s->id;s->terminal->set_write_callback([this,id](std::string_view b){if(auto*t=find(id)){if(b.size()>256*1024-t->pending_input.size())throw std::runtime_error("输入队列已满，此次输入未提交");t->pending_input.append(b);}});
  if(!s->serial->connect(std::move(o))){view.notice="串口连接参数无效";return;}sessions.push_back(std::move(s));
 }
 void connect(ConnectRequest r,bool touch_policy=true){
  if(shutting_down||sessions.size()+pending_connections.size()>=10){wipe(r.password);view.notice="会话数量已达上限或应用正在关闭";return;}
  if(r.host.connection_type!="ssh"){wipe(r.password);view.notice="串口设备不能使用 SSH 连接";return;}
  if(r.host.port<1||r.host.port>65535||r.host.hostname.empty()||r.host.username.empty()){wipe(r.password);view.notice="连接参数无效";return;}
  if(r.host.auth=="keyboard-interactive" || (r.host.id>0 && host(r.host.id) && host(r.host.id)->auth=="keyboard-interactive")){wipe(r.password);view.notice="此设备使用已不支持的 keyboard-interactive 认证，请先编辑并保存其他认证方式";return;}
  const auto& key_path=r.key_path.empty()?r.host.key_path:r.key_path;
  const bool invalid=r.host.hostname.find('\0')!=std::string::npos||r.host.username.find('\0')!=std::string::npos||key_path.find('\0')!=std::string::npos||
   (r.method==AuthMethod::Key&&(key_path.empty()||r.password.find('\0')!=std::string::npos))||
   (r.method!=AuthMethod::Password&&r.method!=AuthMethod::Key&&r.method!=AuthMethod::Agent);
  if(invalid){wipe(r.password);view.notice="连接参数无效";return;}
  const std::string auth=r.method==AuthMethod::Key?"publickey":r.method==AuthMethod::Agent?"agent":"password";
  if(r.save_to_workspace&&r.host.id==0){
   r.host.auth=auth;if(!r.key_path.empty())r.host.key_path=r.key_path;
   if(r.host.name.empty())r.host.name=r.host.hostname;
   auto command=hosts.add(r.host);
   if(!command){wipe(r.password);view.notice="设备存储队列繁忙，未连接";return;}
   PendingConnect pending;pending.secret=CredentialSecret(r.password);wipe(r.password);pending.request=std::move(r);
   pending_connections.emplace(command,std::move(pending));view.notice="正在保存到工作空间设备";return;
  }
  const bool retained=r.host.id>0&&host(r.host.id)&&host(r.host.id)->connection_type=="ssh"&&binding(*host(r.host.id))==binding(r.host)&&r.host.auth==auth&&(r.method!=AuthMethod::Key||r.key_path.empty()||r.key_path==r.host.key_path);
  if(!retained){r.policy=RememberPolicy::Never;r.save_password=false;r.use_saved_password=false;}
  else if(touch_policy&&r.policy==RememberPolicy::Never){
   if(!erase_for_host(r.host.id)){wipe(r.password);return;}
   for(auto&old:sessions)if(old->host.id==r.host.id){old->remember_secret.clear();old->save_after_auth=false;}
  }
  if(r.method==AuthMethod::Agent){wipe(r.password);r.save_password=false;r.use_saved_password=false;r.policy=RememberPolicy::Never;}
  auto s=std::make_unique<Session>();s->id=next++;s->host=r.host;s->ssh=std::make_unique<net::SshSession>();s->terminal=std::make_shared<Terminal>(24,80,2000);
  s->options.host=r.host.hostname;s->options.port=static_cast<std::uint16_t>(r.host.port);s->options.user=r.host.username;
  s->options.auth.method=r.method==AuthMethod::Key?net::AuthMethod::Key:r.method==AuthMethod::Agent?net::AuthMethod::Agent:net::AuthMethod::Password;
  s->options.auth.private_key_file=r.key_path.empty()?r.host.key_path:std::move(r.key_path);
  s->remember_ref=ref(r.host,kind(r.method));s->remember_policy=r.policy;
  const auto id=s->id;s->terminal->set_write_callback([this,id](std::string_view b){if(auto*t=find(id)){if(b.size()>256*1024-t->pending_input.size())throw std::runtime_error("输入队列已满，此次输入未提交");t->pending_input.append(b);}});
  if(r.use_saved_password&&r.policy!=RememberPolicy::Never){s->credential_wait=true;auto c=credentials.load(s->remember_ref,saved_policy(s->remember_ref,r.policy));if(!c){wipe(r.password);view.notice="口令存储繁忙，未发起连接";return;}track(c,CredentialJobKind::Load,id);}
  else {if(r.save_password&&r.policy!=RememberPolicy::Never){s->remember_secret=CredentialSecret(r.password);s->save_after_auth=true;}if(r.method==AuthMethod::Key)s->options.auth.passphrase=std::move(r.password);else s->options.auth.password=std::move(r.password);}
  wipe(r.password);if(retained&&touch_policy)policies[r.host.id]=r.policy;
  sessions.push_back(std::move(s));
 }
  FilePane& pane(std::uint64_t id,bool remote){for(auto&p:view.file_panes)if(p.session_id==id&&p.remote==remote)return p;FilePane p;p.session_id=id;p.remote=remote;view.file_panes.push_back(std::move(p));return view.file_panes.back();}
 std::string local_path(std::string p){
#ifdef _WIN32
  if(p=="."||p.empty()){
   if(auto*home=std::getenv("USERPROFILE");home&&*home)return home;
   std::string combined;
   if(auto*drive=std::getenv("HOMEDRIVE");drive&&*drive)combined=drive;
   if(auto*rest=std::getenv("HOMEPATH");rest&&*rest)combined+=rest;
   if(!combined.empty())return combined;
   std::error_code ec;auto cwd=std::filesystem::current_path(ec);
   if(!ec)return utf8_path(cwd);
   return "C:\\";
  }
  if(p=="/"||p=="\\"){
   std::error_code ec;auto root=std::filesystem::current_path(ec).root_path();
   if(!ec&&!root.empty())return utf8_path(root);
  }
  return p;
#else
  if(p=="."||p.empty()){auto*h=std::getenv("HOME");return h&&*h?h:"/";}
  return p;
#endif
 }
 void list(std::uint64_t id,bool remote,std::string p){auto*s=find(id);if(!s||!s->ssh||s->closing||shutting_down)return;auto generation=++generations[{id,remote}];auto&v=pane(id,remote);v.requested_path=remote?(p.empty()?".":p):local_path(std::move(p));v.loading=true;v.error.clear();
  if(remote){net::TaskRequest r;r.kind=net::TaskKind::List;r.remote_path=v.requested_path;auto t=s->ssh->submit_task(std::move(r));if(t)jobs[{id,t}]={JobKind::List,generation,0,{}};else{v.loading=false;v.error="远端未连接或任务队列已满";}}
  else{auto t=local.list(native_file_path(v.requested_path));if(t)local_jobs[t]={id,generation,true,{}};else{v.loading=false;v.error="本地文件队列繁忙";}}
 }
 void refresh_changed(std::uint64_t id,bool remote,const std::string& changed_path){
  pending_refresh.emplace(id,remote,file_parent(changed_path,remote));
 }
 void operation_result(std::uint64_t id,bool remote,const std::string& path,std::string error){
  auto& p=pane(id,remote);p.operation_failed=!error.empty();
  p.operation_message=(p.operation_failed?error:"操作成功")+" · "+path;
  if(!p.operation_failed)refresh_changed(id,remote,path);
 }
 void finish_transfer(std::uint64_t id,TransferState state,std::string error){for(auto&t:view.transfers)if(t.id==id){t.state=state;t.error=std::move(error);break;}transfer_jobs.erase(id);}
 bool transfer(std::uint64_t id,bool upload,std::string source,std::string dest){auto*s=find(id);if(!s||!s->ssh||s->closing||shutting_down||s->previous!=net::SessionState::Connected)return false;
  if(std::any_of(view.transfers.begin(),view.transfers.end(),[&](const auto& t){return t.session_id==id&&t.upload==upload&&transfer_active(t.state)&&t.source==source&&t.destination==dest;}))return false;
  if(view.transfers.size()>=256){auto i=std::find_if(view.transfers.begin(),view.transfers.end(),[](auto&t){return !transfer_active(t.state);});if(i!=view.transfers.end())view.transfers.erase(i);else return false;}
  TransferTask t;t.id=next_transfer++;t.session_id=id;t.upload=upload;t.source=std::move(source);t.destination=std::move(dest);view.transfers.push_back(std::move(t));
  return true;
 }
 bool dispatch_transfers(Session& s,net::SessionState state){
  bool changed=false;
  if(state!=net::SessionState::Connected){
   if(state==net::SessionState::Disconnected||state==net::SessionState::Failed)
    for(auto&t:view.transfers)if(t.session_id==s.id&&transfer_active(t.state)&&!transfer_jobs.count(t.id)){finish_transfer(t.id,TransferState::Failed,"SSH 连接已断开");changed=true;}
   return changed;
  }
  if(shutting_down||std::any_of(transfer_jobs.begin(),transfer_jobs.end(),[&](const auto& job){return job.second.first==s.id;}))return false;
  for(const auto& t:view.transfers)if(t.session_id==s.id&&t.state==TransferState::Queued){
   net::TaskRequest r;r.kind=t.upload?net::TaskKind::Upload:net::TaskKind::Download;r.local_path=t.upload?t.source:t.destination;r.remote_path=t.upload?t.destination:t.source;
   auto valid=[](const std::string& p){return !p.empty()&&p.size()<=16384&&p.find('\0')==std::string::npos;};
   if(!valid(r.local_path)||!valid(r.remote_path)||!native_file_path(r.local_path).is_absolute()||r.local_path.back()=='/'){
    finish_transfer(t.id,TransferState::Failed,"文件路径无效或过长");changed=true;continue;
   }
   auto task=s.ssh->submit_task(std::move(r));if(task){jobs[{s.id,task}]={JobKind::Transfer,0,t.id,{}};transfer_jobs[t.id]={s.id,task};changed=true;}
   break;
  }
  return changed;
 }
 void operation(FileOperationRequest r){auto*s=find(r.session_id);if(!s||!s->ssh||shutting_down)return;if(r.recursive){view.notice="不允许递归删除";return;}
  if(r.remote){net::TaskRequest t;t.remote_path=r.path;t.destination_path=r.target;t.permissions=r.permissions;t.overwrite=false;t.kind=r.operation==FileOperation::Mkdir?net::TaskKind::Mkdir:r.operation==FileOperation::Rename?net::TaskKind::Rename:r.operation==FileOperation::Delete?net::TaskKind::Remove:net::TaskKind::Chmod;auto id=s->ssh->submit_task(std::move(t));if(id)jobs[{r.session_id,id}]={JobKind::Operation,0,0,r.path};else operation_result(r.session_id,true,r.path,"远端文件操作未接受");}
  else{LocalCommandId id=0;const auto path=native_file_path(r.path);switch(r.operation){case FileOperation::Mkdir:id=local.mkdir(path);break;case FileOperation::Rename:id=local.rename(path,native_file_path(r.target));break;case FileOperation::Delete:id=local.remove_file(path);break;case FileOperation::Chmod:id=local.chmod(path,r.permissions);break;}if(id)local_jobs[id]={r.session_id,0,false,r.path};else operation_result(r.session_id,false,r.path,"本地文件操作队列繁忙");}
 }
 void credentials_tick(){
  auto v=credentials.snapshot();
  for(auto&r:credentials.drain_results()){
   auto it=credential_jobs.find(r.command);if(it==credential_jobs.end())continue;auto j=std::move(it->second);credential_jobs.erase(it);
   if(j.kind==CredentialJobKind::MutationErase){auto m=mutations.find(j.mutation);if(m!=mutations.end()){--m->second.remaining;m->second.failed|=!r.success;}if(!r.success)view.notice=r.error;continue;}
   if(j.epoch!=epoch)continue;
   if(!r.success){view.notice=r.error;if(auto*s=find(j.session)){clear_session_secrets(*s);s->credential_wait=false;s->waiting=false;s->local_error="凭据读取/保存失败："+r.error;}continue;}
   if(j.kind==CredentialJobKind::Load){if(auto*s=find(j.session);s&&s->credential_wait){s->credential_wait=false;if(saved_policy(s->remember_ref,s->remember_policy)!=policy(s->remember_policy)){s->remember_secret=CredentialSecret(std::string_view(reinterpret_cast<const char*>(r.secret.data()),r.secret.size()));s->save_after_auth=true;}std::string secret(reinterpret_cast<const char*>(r.secret.data()),r.secret.size());if(s->options.auth.method==net::AuthMethod::Key)s->options.auth.passphrase=std::move(secret);else s->options.auth.password=std::move(secret);wipe(secret);}}
   else if(j.kind==CredentialJobKind::Store)view.notice="认证成功，凭据已按所选策略保存";
  }
  finish_mutations();
  view.connection_settings.clear();for(auto&h:view.hosts){if(h.connection_type!="ssh")continue;ConnectionSettings s;s.host_id=h.id;auto p=policies.find(h.id);s.policy=p==policies.end()?RememberPolicy::Never:p->second;for(auto&m:v->entries)if(m.ref.host_id==h.id&&m.ref.credential_id==ref(h,m.ref.kind).credential_id){s.has_saved_password=true;if(p==policies.end())s.policy=static_cast<RememberPolicy>(m.policy);}view.connection_settings.push_back(s);}
 }
 void task_result(Session&s,const net::TaskSnapshot&t,bool complete){auto it=jobs.find({s.id,t.id});if(it==jobs.end())return;auto j=it->second;
  if(j.kind==JobKind::Transfer){for(auto&v:view.transfers)if(v.id==j.transfer){v.completed_bytes=t.transferred;v.total_bytes=t.total;if(!complete)v.state=t.state==net::TaskState::Running?TransferState::Running:TransferState::Queued;else if(t.state==net::TaskState::Succeeded)refresh_changed(s.id,v.upload,v.destination);break;}if(complete)finish_transfer(j.transfer,t.state==net::TaskState::Succeeded?TransferState::Completed:t.state==net::TaskState::Cancelled?TransferState::Cancelled:TransferState::Failed,t.error);}
  else if(complete&&j.kind==JobKind::List&&generations[{s.id,true}]==j.generation){auto&p=pane(s.id,true);p.loading=false;p.error=t.error;if(t.state==net::TaskState::Succeeded){p.path=t.listed_path;p.truncated=t.listing_truncated;++p.revision;p.entries.clear();for(auto&e:t.entries)p.entries.push_back({e.name,e.size,static_cast<std::int64_t>(e.mtime),static_cast<unsigned>(e.permissions),S_ISDIR(e.permissions),S_ISLNK(e.permissions),S_ISREG(e.permissions)});}}
  else if(complete&&j.kind==JobKind::Operation){operation_result(s.id,true,j.path,t.error);}
  else if(complete&&j.kind==JobKind::Monitor){auto now=metrics::Clock::now();bool ok=t.state==net::TaskState::Succeeded&&t.exit_status==0&&!t.output_truncated;if(ok)ok=s.collector.consume(t.stdout_text,now);else s.collector.fail(t.error.empty()?"采集执行失败或输出被截断":t.error,now);if(ok){auto snap=s.collector.snapshot();std::optional<double>d;for(auto&disk:snap.disks)if(disk.percent&&(!d||*disk.percent>*d))d=disk.percent;s.disk_history[seconds(snap.sampled_at)]=d;while(!s.disk_history.empty()&&s.disk_history.begin()->first<seconds(now)-300)s.disk_history.erase(s.disk_history.begin());}else s.disk_history.clear();s.failures=ok?0:std::min(s.failures+1,5);s.monitor_task=0;s.next_sample=now+std::chrono::seconds(s.interval*(1<<s.failures));}
  if(complete)jobs.erase(it);
 }
 void metrics_view(Session&s,metrics::Clock::time_point now){MetricsView m;m.session_id=s.id;m.enabled=s.monitor;m.now=seconds(now);auto snap=s.collector.snapshot();m.error=snap.error;if(!s.monitor)m.error="监控已停止，历史数据仅供参考";else if(s.ssh->snapshot().state!=net::SessionState::Connected)m.error="连接已断开，历史数据仅供参考";else if(snap.stale&&m.error.empty())m.error="采集数据尚未就绪或已过期";if(!snap.cpu_percent)m.cpu_missing="首次采样需要差分或采集不可用";if(!snap.memory)m.memory_missing="内存数据缺失";if(snap.disks.empty())m.disk_missing="存储数据缺失";if(snap.networks.empty())m.network_missing="网络计数缺失";
  for(auto&p:snap.history){MetricSample v;v.timestamp=seconds(p.time);v.cpu_percent=p.cpu_percent;v.memory_percent=p.memory_percent;v.net_rx_bytes_sec=p.rx_bytes_per_second;v.net_tx_bytes_sec=p.tx_bytes_per_second;auto d=s.disk_history.find(v.timestamp);if(d!=s.disk_history.end())v.disk_percent=d->second;m.samples.push_back(v);}
  view.metrics.push_back(std::move(m));
 }
 bool tick(){bool dirty=false;auto snapshot=hosts.snapshot();if(snapshot&&snapshot->revision!=revision){revision=snapshot->revision;view.hosts=snapshot->hosts;dirty=true;}if(snapshot&&!snapshot->error.empty()&&view.notice!=snapshot->error){view.notice=snapshot->error;dirty=true;}
  auto before=credentials.snapshot()->revision;credentials_tick();if(before!=credential_revision){credential_revision=before;dirty=true;}
  if(serial_discovery){auto task=serial_discovery;bool done=false;{std::lock_guard lock(task->mutex);done=task->done;if(done){view.serial_ports=std::move(task->ports);view.serial_ports_error=std::move(task->error);}}if(done){serial_discovery.reset();view.serial_ports_loading=false;dirty=true;}}
  for(auto&r:hosts.drain_results()){
   auto pending=pending_connections.find(r.command);if(pending!=pending_connections.end()){auto item=std::move(pending->second);pending_connections.erase(pending);if(r.error.empty()&&!shutting_down){auto latest=hosts.snapshot();view.hosts=latest->hosts;item.request.host.id=r.host_id;item.request.save_to_workspace=false;item.request.password.assign(reinterpret_cast<const char*>(item.secret.data()),item.secret.size());item.secret.clear();connect(std::move(item.request));}else view.notice=r.error.empty()?"应用正在关闭，已取消连接":r.error;dirty=true;continue;}if(!r.error.empty()){view.notice=r.error;dirty=true;}auto trust=trust_commits.find(r.command);if(trust!=trust_commits.end()){if(auto*s=find(trust->second.session))s->ssh->answer_host_key(trust->second.request,r.error.empty());if(r.error.empty())view.notice="主机信任已保存";trust_commits.erase(trust);dirty=true;}auto it=lookups.find(r.command);if(it!=lookups.end()){if(auto*s=find(it->second);s&&s->waiting&&!s->credential_wait){s->waiting=false;if(r.error.empty()){if(r.known_host)s->options.expected_host_key=*r.known_host;if(!s->ssh->connect(std::move(s->options))){clear_session_secrets(*s);s->local_error="连接参数无效";}}else{clear_session_secrets(*s);s->local_error=r.error;}}lookups.erase(it);dirty=true;}}
   for(auto&r:local.drain_results()){auto i=local_jobs.find(r.command);if(i==local_jobs.end())continue;auto j=i->second;local_jobs.erase(i);if(!find(j.session))continue;if(j.listing&&generations[{j.session,false}]==j.generation){auto&p=pane(j.session,false);p.loading=false;p.error=r.error;if(r.snapshot&&r.snapshot->ready){p.path=utf8_path(r.snapshot->path);p.truncated=r.snapshot->truncated;++p.revision;p.entries.clear();for(auto&e:r.snapshot->entries)p.entries.push_back({e.name,e.size,e.mtime,e.permissions,e.is_directory,e.is_symlink,e.is_regular});}}else if(!j.listing){operation_result(j.session,false,j.path,r.error);}dirty=true;}
  view.trust.reset();view.sessions.clear();view.metrics.clear();auto now=metrics::Clock::now();
  // Closing sessions are hidden immediately; reap them only after the SSH
  // worker has stopped, avoiding a blocking join in the UI callback.
  std::erase_if(sessions,[&](const auto& s){if(!s->closing)return false;if(s->serial){auto snapshot=s->serial->snapshot();auto state=snapshot.state;if(state!=net::SerialState::Disconnected&&state!=net::SerialState::Failed)return false;if(snapshot.discarded_input)view.notice="串口关闭，部分排队数据未发送";}else{auto state=s->ssh->snapshot().state;if(state!=net::SessionState::Disconnected&&state!=net::SessionState::Failed)return false;}dirty=true;return true;});
  for(auto&s:sessions){if(s->closing)continue;
   if(s->serial){
    auto state=s->serial->snapshot();
    if(state.state!=s->serial_previous){dirty=true;s->serial_previous=state.state;if((state.state==net::SerialState::Failed||state.state==net::SerialState::Disconnected)&&!s->pending_input.empty()){view.notice="串口已断开，未发送输入已清除";wipe(s->pending_input);}if(state.discarded_input)view.notice="串口关闭，部分排队数据未发送";}
    if(state.state==net::SerialState::Connected&&!s->pending_input.empty()){auto n=s->serial->write_input(s->pending_input);s->pending_input.erase(0,n);}
    auto data=s->serial->read_output(128*1024);if(!data.empty()){s->terminal->feed(data);dirty=true;}
    SessionView v;v.id=s->id;v.title="串口 · "+s->serial_port;v.type=SessionType::Serial;v.capabilities={false,false,false};v.terminal=s->terminal;v.error=state.error;v.connected=state.state==net::SerialState::Connected;
    switch(state.state){case net::SerialState::Disconnected:v.status="已断开";break;case net::SerialState::Connecting:v.status="正在连接";break;case net::SerialState::Connected:v.status="已连接";break;case net::SerialState::Disconnecting:v.status="正在断开";break;case net::SerialState::Failed:v.status="连接失败";break;}
    view.sessions.push_back(std::move(v));continue;
   }
   if(s->waiting&&!s->credential_wait&&!s->lookup&&!shutting_down){s->lookup=hosts.get_known_host(s->host.hostname,s->host.port);if(s->lookup)lookups[s->lookup]=s->id;}
   for(auto&t:s->ssh->poll_tasks()){task_result(*s,t,true);dirty=true;}
   auto state=s->ssh->snapshot();if(state.state!=s->previous){dirty=true;s->previous=state.state;if(state.state==net::SessionState::Connected){s->monitor=true;s->next_sample=metrics::Clock::now();s->ssh->resize(s->terminal->cols(),s->terminal->rows());if(s->save_after_auth){auto*h=host(s->host.id);if(h&&h->connection_type=="ssh"&&binding(*h)==binding(s->host)){auto id=credentials.store(s->remember_ref,policy(s->remember_policy),std::move(s->remember_secret));track(id,CredentialJobKind::Store,s->id);if(!id)view.notice="认证成功，但口令存储繁忙，凭据未保存";}s->remember_secret.clear();s->save_after_auth=false;}}
    if(state.state==net::SessionState::Failed||state.state==net::SessionState::Disconnected){if(!s->pending_input.empty())view.notice="连接已结束，未发送输入已清除";wipe(s->pending_input);clear_session_secrets(*s);}}
   if(state.state==net::SessionState::Connected&&!s->pending_input.empty()){auto n=s->ssh->write_input(s->pending_input);s->pending_input.erase(0,n);}
   auto out=s->ssh->read_output(128*1024);if(!out.empty()){s->terminal->feed(out);dirty=true;}if(!s->ssh->poll_events().empty())dirty=true;
    for(auto&t:state.tasks){task_result(*s,t,false);dirty=true;}
    if(s->monitor&&!s->monitor_task&&state.state==net::SessionState::Connected&&now>=s->next_sample&&!shutting_down){if(s->collector.begin_sample()){net::TaskRequest r;r.kind=net::TaskKind::Exec;r.command=metrics::Collector::script();r.output_limit=metrics::Collector::output_limit;r.timeout=std::chrono::seconds(10);auto id=s->ssh->submit_task(std::move(r));if(id){s->monitor_task=id;jobs[{s->id,id}]={JobKind::Monitor,0,0,{}};}else{s->collector.fail("采集任务队列繁忙",now);s->next_sample=now+std::chrono::seconds(4);}}}
   metrics_view(*s,now);SessionView v;v.id=s->id;v.title=s->host.name.empty()?s->host.hostname:s->host.name;v.status=s->credential_wait?"读取已保存凭据":s->waiting?"读取主机信任记录":state_name(state.state);v.terminal=s->terminal;v.error=s->local_error.empty()?state.error.message:s->local_error;v.connected=state.state==net::SessionState::Connected;view.sessions.push_back(std::move(v));
   if(!view.trust&&state.host_key_request){auto&k=*state.host_key_request;TrustRequest t;t.session_id=s->id;t.request_id=k.id;t.host=k.host;t.port=k.port;t.algorithm=k.algorithm;t.fingerprint=k.sha256_fingerprint;view.trust=std::move(t);}
   }
   for(auto it=pending_refresh.begin();it!=pending_refresh.end();){
    const auto& [id,remote,path]=*it;
    const auto p=std::find_if(view.file_panes.begin(),view.file_panes.end(),[&](const auto& p){return p.session_id==id&&p.remote==remote;});
    if(p!=view.file_panes.end()&&p->loading&&p->requested_path==path){++it;continue;}
    if(p!=view.file_panes.end()&&!p->loading&&p->path==path){list(id,remote,path);dirty=true;}
    it=pending_refresh.erase(it);
   }
   for(auto& s:sessions)if(s->ssh&&!s->closing)dirty=dispatch_transfers(*s,s->previous)||dirty;
  if(shutting_down&&!host_shutdown&&mutations.empty()&&credentials.stopped()){hosts.shutdown();host_shutdown=true;}
  return dirty;
 }
 void shutdown(){if(shutting_down)return;shutting_down=true;++epoch;pending_connections.clear();for(auto&s:sessions){s->waiting=false;s->credential_wait=false;s->monitor=false;if(s->serial)s->serial->disconnect();else s->ssh->disconnect();clear_session_secrets(*s);wipe(s->pending_input);}local.shutdown();credentials.shutdown();if(mutations.empty()){hosts.shutdown();host_shutdown=true;}}
};
WorkspaceController::WorkspaceController(std::filesystem::path p):impl_(std::make_unique<Impl>(std::move(p))){}
WorkspaceController::~WorkspaceController(){impl_->shutdown();}
const WorkspaceModel& WorkspaceController::model()const{return impl_->view;}
const WorkspaceCallbacks& WorkspaceController::callbacks()const{return impl_->cb;}
bool WorkspaceController::tick(){return impl_->tick();}
bool WorkspaceController::busy()const{if(!impl_->pending_connections.empty()||!impl_->credential_jobs.empty()||!impl_->local_jobs.empty()||!impl_->jobs.empty())return true;if(impl_->serial_discovery)return true;for(auto&s:impl_->sessions){if(s->serial){auto t=s->serial->snapshot();if(t.state==net::SerialState::Connecting||t.state==net::SerialState::Disconnecting)return true;continue;}auto t=s->ssh->snapshot();if(s->waiting||s->credential_wait||t.state==net::SessionState::Connecting||t.state==net::SessionState::Authenticating||t.state==net::SessionState::AwaitingHostKey||t.state==net::SessionState::Disconnecting)return true;}return false;}
bool WorkspaceController::has_pending_output()const{for(auto&s:impl_->sessions)if((s->serial?s->serial->snapshot().available_output:s->ssh->snapshot().available_output)||!s->pending_input.empty())return true;return false;}
void WorkspaceController::begin_shutdown(){impl_->shutdown();}
bool WorkspaceController::shutdown_complete()const{if(!impl_->hosts.stopped()||!impl_->local.stopped()||!impl_->credentials.stopped())return false;for(auto&s:impl_->sessions){if(s->serial){auto state=s->serial->snapshot().state;if(state!=net::SerialState::Disconnected&&state!=net::SerialState::Failed)return false;}else{auto state=s->ssh->snapshot().state;if(state!=net::SessionState::Disconnected&&state!=net::SessionState::Failed)return false;}}return true;}
}
