#include "jianssh/app/credential_service.hpp"
#include "jianssh/util/crypto_bridge.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace jianssh::app {
CredentialSecret::CredentialSecret(std::string_view s): bytes_(s.begin(),s.end()) {}
CredentialSecret::~CredentialSecret(){ clear(); }
CredentialSecret::CredentialSecret(CredentialSecret&& s) noexcept: bytes_(std::move(s.bytes_)) {}
CredentialSecret& CredentialSecret::operator=(CredentialSecret&& s) noexcept { if(this!=&s){clear();bytes_=std::move(s.bytes_);}return *this; }
void CredentialSecret::clear() noexcept { if(!bytes_.empty())js_crypto_wipe(bytes_.data(),bytes_.size());bytes_.clear(); }
namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr std::size_t MaxFile=16*1024*1024, MaxSecret=64*1024, MaxRecords=4096, Capacity=64;
struct Failure:std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool v,const char* error="凭据库数据无效"){if(!v)throw Failure(error);}
struct Key { std::array<std::uint8_t,32> b{}; ~Key(){js_crypto_wipe(b.data(),b.size());} };
void random(std::uint8_t* p,std::size_t n){require(js_crypto_random(p,n)==0,"安全随机数不可用");}
void put32(Bytes& b,std::uint32_t n){for(int i=0;i<4;++i)b.push_back((n>>(8*i))&255);}
void put64(Bytes& b,std::uint64_t n){for(int i=0;i<8;++i)b.push_back((n>>(8*i))&255);}
void append(Bytes& b,const void* p,std::size_t n){auto s=static_cast<const std::uint8_t*>(p);b.insert(b.end(),s,s+n);}
struct Reader {
    const Bytes& b; std::size_t p=0;
    const std::uint8_t* take(std::size_t n){require(n<=b.size()-p);auto r=b.data()+p;p+=n;return r;}
    std::uint32_t u32(){auto q=take(4);std::uint32_t n=0;for(int i=0;i<4;++i)n|=std::uint32_t(q[i])<<(8*i);return n;}
    std::uint64_t u64(){auto q=take(8);std::uint64_t n=0;for(int i=0;i<8;++i)n|=std::uint64_t(q[i])<<(8*i);return n;}
};
void valid(const CredentialRef& r){require(!r.credential_id.empty()&&r.credential_id.size()<=128&&r.host_id>0&&(r.kind==CredentialKind::Password||r.kind==CredentialKind::KeyPassphrase),"凭据引用无效");}
Bytes aad(const CredentialRef& r){Bytes b;const char domain[]="JianSSH credential v1";append(b,domain,sizeof domain);put32(b,r.credential_id.size());append(b,r.credential_id.data(),r.credential_id.size());put64(b,r.host_id);put32(b,static_cast<std::uint32_t>(r.kind));return b;}
std::string index(const CredentialRef& r){auto b=aad(r);return std::string(reinterpret_cast<const char*>(b.data()),b.size());}
struct Sealed {std::array<std::uint8_t,24> nonce{};std::array<std::uint8_t,16>tag{};Bytes cipher;};
Sealed seal(const std::uint8_t* p,std::size_t n,const Bytes& ad,const Key& key){Sealed s;random(s.nonce.data(),s.nonce.size());s.cipher.resize(n);require(js_crypto_encrypt(s.cipher.data(),s.tag.data(),p,n,ad.data(),ad.size(),s.nonce.data(),key.b.data())==0,"加密失败");return s;}
Bytes unseal(const Sealed& s,const Bytes& ad,const Key& key){Bytes b(s.cipher.size());if(js_crypto_decrypt(b.data(),s.cipher.data(),b.size(),s.tag.data(),ad.data(),ad.size(),s.nonce.data(),key.b.data())!=0){if(!b.empty())js_crypto_wipe(b.data(),b.size());throw Failure("凭据校验失败");}return b;}
void pack(Bytes& b,const Sealed& s){append(b,s.nonce.data(),24);put32(b,s.cipher.size());append(b,s.cipher.data(),s.cipher.size());append(b,s.tag.data(),16);}
Sealed unpack(Reader& r,std::size_t max){Sealed s;std::memcpy(s.nonce.data(),r.take(24),24);auto n=r.u32();require(n<=max);auto p=r.take(n);s.cipher.assign(p,p+n);std::memcpy(s.tag.data(),r.take(16),16);return s;}
struct Record {CredentialRef ref;Sealed secret;};
Bytes prefix(){Bytes b;append(b,"JSSHLC01",8);put32(b,1);return b;}
struct WipeBytes {Bytes& b;~WipeBytes(){if(!b.empty())js_crypto_wipe(b.data(),b.size());}};
}
struct CredentialService::State {
    enum class Op {Store,Load,Erase};
    struct Command {CredentialCommandId id=0;Op op;CredentialRef ref;CredentialPolicy policy=CredentialPolicy::Never;CredentialSecret secret;};
    std::filesystem::path path;
    std::mutex mutex;std::condition_variable cv;std::deque<Command> commands;std::vector<CredentialResult> results;
    std::shared_ptr<const CredentialSnapshot> published;
    std::atomic<bool> done=false;bool stopping=false;std::uint64_t next=1;std::size_t outstanding=0;
    CredentialSnapshot view;Key key;std::map<std::string,Record> records;struct Session {CredentialRef ref;CredentialSecret secret;};std::map<std::string,Session> sessions;
    int directory=-1, lockfd=-1;std::string filename;bool usable=false;
    State(std::filesystem::path p):path(std::move(p)){auto initial=std::make_shared<CredentialSnapshot>();initial->platform_supported=credential_vault_supported();std::atomic_store(&published,std::shared_ptr<const CredentialSnapshot>(std::move(initial)));}
    ~State(){close_files();}
    void close_files() noexcept {
#ifndef _WIN32
        if(lockfd>=0){::close(lockfd);lockfd=-1;}if(directory>=0){::close(directory);directory=-1;}
#endif
    }
    void publish(){view.entries.clear();if(view.unlocked){for(const auto& [idx,r]:records){(void)idx;view.entries.push_back({r.ref,CredentialPolicy::Encrypted});}}for(const auto& [idx,s]:sessions){(void)idx;view.entries.push_back({s.ref,CredentialPolicy::Session});}++view.revision;std::atomic_store(&published,std::shared_ptr<const CredentialSnapshot>(std::make_shared<CredentialSnapshot>(view)));}
    void wipe_state(){js_crypto_wipe(key.b.data(),32);sessions.clear();records.clear();view.unlocked=false;}
    void initialize(){
#ifdef _WIN32
        view.platform_supported=false;
        throw Failure("Windows 下尚未实现系统凭据加密，凭据库不可用");
#else
        require(!path.filename().empty(),"凭据库路径无效");filename=path.filename().string();require(filename!="."&&filename!="..","凭据库路径无效");
        auto parent=path.parent_path();require(!parent.empty(),"凭据库需要专用私有目录");
        std::filesystem::create_directories(parent);
        directory=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);require(directory>=0,"无法打开私有凭据库目录");
        struct stat st{};require(::fstat(directory,&st)==0&&st.st_uid==::geteuid(),"凭据库目录属主不匹配");
        require(::fchmod(directory,0700)==0,"无法设置凭据库目录权限");
        auto lockname=filename+".lock";lockfd=::openat(directory,lockname.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        require(lockfd>=0,"无法打开凭据库锁");require(::fstat(lockfd,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==::geteuid()&&st.st_nlink==1,"凭据库锁不安全");require(::fchmod(lockfd,0600)==0&&::flock(lockfd,LOCK_EX|LOCK_NB)==0,"凭据库已被占用");
        int r=::fstatat(directory,filename.c_str(),&st,AT_SYMLINK_NOFOLLOW);require(r==0||errno==ENOENT,"无法检查凭据库");
        view.exists=r==0;if(view.exists)require(S_ISREG(st.st_mode)&&st.st_uid==::geteuid()&&(st.st_mode&0777)==0600&&st.st_nlink==1,"凭据库权限不安全");
        const auto keyname=filename+".key";
        int kr=::fstatat(directory,keyname.c_str(),&st,AT_SYMLINK_NOFOLLOW);
        require(kr==0||errno==ENOENT,"无法检查本机密钥");
        if(kr!=0){
            require(!view.exists,"本机密钥缺失，无法恢复凭据");
            random(key.b.data(),key.b.size());
            Bytes raw(key.b.begin(),key.b.end());WipeBytes wipe{raw};
            replace_file(raw,keyname);
        }else{
            auto raw=read_file(keyname,32);WipeBytes wipe{raw};
            require(raw.size()==32,"本机密钥长度无效");
            std::memcpy(key.b.data(),raw.data(),32);
        }
        if(view.exists)open();
        usable=true;view.ready=true;view.unlocked=true;
#endif
    }
    Bytes read_file(const std::string& name,std::size_t maximum=MaxFile){
#ifndef _WIN32
        int fd=::openat(directory,name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);require(fd>=0,"无法读取凭据库");
        struct Guard{int fd;~Guard(){::close(fd);}}guard{fd};struct stat st{};
        require(::fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==::geteuid()&&(st.st_mode&0777)==0600&&st.st_nlink==1&&st.st_size>=0&&static_cast<std::uint64_t>(st.st_size)<=maximum,"凭据库文件不安全");
        Bytes b(static_cast<std::size_t>(st.st_size));std::size_t p=0;while(p<b.size()){auto n=::read(fd,b.data()+p,b.size()-p);if(n<0&&errno==EINTR)continue;require(n>0,"无法读取凭据库");p+=n;}return b;
#else
        (void)name;(void)maximum;throw Failure("Windows 下凭据库不可用");
#endif
    }
    void replace_file(const Bytes& bytes,const std::string& name){
#ifndef _WIN32
        require(bytes.size()<=MaxFile,"凭据库大小超过上限");std::array<std::uint8_t,12> rnd;random(rnd.data(),rnd.size());const char hex[]="0123456789abcdef";std::string temp=name+".tmp.";for(auto v:rnd){temp+=hex[v>>4];temp+=hex[v&15];}
        int fd=::openat(directory,temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);require(fd>=0,"无法写入凭据库");
        bool renamed=false;try {require(::fchmod(fd,0600)==0,"无法设置凭据库文件权限");std::size_t p=0;while(p<bytes.size()){auto n=::write(fd,bytes.data()+p,bytes.size()-p);if(n<0&&errno==EINTR)continue;require(n>0,"无法写入凭据库");p+=n;}require(::fsync(fd)==0,"无法同步凭据库");int closed=::close(fd);fd=-1;require(closed==0,"无法关闭凭据库");require(::renameat(directory,temp.c_str(),directory,name.c_str())==0,"无法替换凭据库");renamed=true;require(::fsync(directory)==0,"无法同步凭据库目录");}
        catch(...){if(fd>=0)::close(fd);if(!renamed)::unlinkat(directory,temp.c_str(),0);else {wipe_state();usable=false;view.ready=false;view.failed=true;}throw;}
#else
        (void)bytes;(void)name;throw Failure("Windows 下凭据库不可用");
#endif
    }
    Bytes encode(const std::map<std::string,Record>& rs){
        Bytes plain;WipeBytes wipe{plain};put32(plain,rs.size());for(const auto& [idx,r]:rs){(void)idx;put32(plain,r.ref.credential_id.size());append(plain,r.ref.credential_id.data(),r.ref.credential_id.size());put64(plain,r.ref.host_id);put32(plain,static_cast<std::uint32_t>(r.ref.kind));pack(plain,r.secret);}
        auto b=prefix();auto body=seal(plain.data(),plain.size(),b,key);pack(b,body);return b;
    }
    void persist(const std::map<std::string,Record>& rs){replace_file(encode(rs),filename);view.exists=true;}
    void open(){
        auto file=read_file(filename);Reader r{file};require(std::memcmp(r.take(8),"JSSHLC01",8)==0&&r.u32()==1,"不支持的凭据文件版本");
        auto body=unpack(r,MaxFile);require(r.p==file.size());
        auto plain=unseal(body,prefix(),key);WipeBytes plainwipe{plain};Reader pr{plain};auto count=pr.u32();require(count<=MaxRecords);std::map<std::string,Record> loaded;
        for(std::uint32_t i=0;i<count;++i){Record record;auto n=pr.u32();require(n>0&&n<=128);auto s=pr.take(n);record.ref.credential_id.assign(reinterpret_cast<const char*>(s),n);auto host=pr.u64();require(host<=INT64_MAX);record.ref.host_id=host;record.ref.kind=static_cast<CredentialKind>(pr.u32());valid(record.ref);record.secret=unpack(pr,MaxSecret);auto idx=index(record.ref);require(!loaded.contains(idx));loaded.emplace(std::move(idx),std::move(record));}
        require(pr.p==plain.size());records=std::move(loaded);
    }
    void execute(Command& c,CredentialResult& result){
        require(usable,"凭据库不可用");
        switch(c.op){
        case Op::Store:{valid(c.ref);require(c.secret.size()<=MaxSecret,"凭据内容过大");auto idx=index(c.ref);
            if(c.policy==CredentialPolicy::Never){require(c.secret.size()<=MaxSecret);auto nextrecords=records;nextrecords.erase(idx);if(nextrecords.size()!=records.size())persist(nextrecords);records=std::move(nextrecords);sessions.erase(idx);break;}
            require(c.policy==CredentialPolicy::Session||c.policy==CredentialPolicy::Encrypted,"凭据保存策略无效");require(c.secret.size()>0,"凭据内容为空");if(c.policy==CredentialPolicy::Session)require(sessions.contains(idx)||sessions.size()<MaxRecords,"凭据数量超过上限");
            auto nextrecords=records;nextrecords.erase(idx);
            if(c.policy==CredentialPolicy::Encrypted){require(nextrecords.size()<MaxRecords,"凭据数量超过上限");Record rec{c.ref,seal(c.secret.data(),c.secret.size(),aad(c.ref),key)};nextrecords.emplace(idx,std::move(rec));}
            if(c.policy==CredentialPolicy::Encrypted||nextrecords.size()!=records.size())persist(nextrecords);records=std::move(nextrecords);sessions.erase(idx);if(c.policy==CredentialPolicy::Session)sessions.emplace(idx,Session{c.ref,std::move(c.secret)});break;}
        case Op::Load:{valid(c.ref);require(c.policy!=CredentialPolicy::Never,"凭据未保留");auto idx=index(c.ref);if(c.policy==CredentialPolicy::Session){auto it=sessions.find(idx);require(it!=sessions.end(),"凭据不存在");result.secret=CredentialSecret(std::string_view(reinterpret_cast<const char*>(it->second.secret.data()),it->second.secret.size()));}
            else {require(c.policy==CredentialPolicy::Encrypted,"凭据保存策略无效");auto it=records.find(idx);require(it!=records.end(),"凭据不存在");auto p=unseal(it->second.secret,aad(c.ref),key);WipeBytes wipe{p};result.secret=CredentialSecret(std::string_view(reinterpret_cast<const char*>(p.data()),p.size()));}break;}
        case Op::Erase:{valid(c.ref);auto idx=index(c.ref);auto nextrecords=records;nextrecords.erase(idx);if(nextrecords.size()!=records.size())persist(nextrecords);records=std::move(nextrecords);sessions.erase(idx);break;}
        }
        result.success=true;
    }
    void wipe_pending_results(){for(auto& r:results)if(r.secret.size()){r.secret.clear();r.success=false;r.error="凭据服务已停止";}}
    void run() noexcept {
        try{initialize();}catch(...){wipe_state();usable=false;view.ready=false;view.failed=true;view.error=view.platform_supported?"凭据初始化失败（文件、密钥、校验、私有目录或锁）":"Windows 下尚未实现系统凭据加密，凭据不可用";}try{publish();}catch(...){}
        for(;;){Command c;
            {std::unique_lock guard(mutex);cv.wait(guard,[&]{return stopping||!commands.empty();});
                if(stopping&&commands.empty())break;
                c=std::move(commands.front());commands.pop_front();}
            CredentialResult result;result.command=c.id;
            try{execute(c,result);view.error.clear();}catch(const Failure& e){result.error=e.what();view.error=result.error;}catch(...){result.error="凭据操作失败";view.error=result.error;}
            c.secret.clear();
            {std::lock_guard guard(mutex);if(stopping){result.secret.clear();if(c.op==Op::Load){result.success=false;result.error="凭据服务已停止";}}try{results.push_back(std::move(result));publish();}catch(...){stopping=true;}}
        }
        wipe_state();{std::lock_guard guard(mutex);wipe_pending_results();view.ready=false;view.stopped=true;try{publish();}catch(...){}}close_files();done=true;
    }
    CredentialCommandId submit(Op op,CredentialRef ref={},CredentialPolicy policy=CredentialPolicy::Never,CredentialSecret secret={}){
        if(secret.size()>MaxSecret||ref.credential_id.size()>128)return 0;
        std::unique_lock guard(mutex,std::try_to_lock);if(!guard.owns_lock()||stopping||outstanding>=Capacity)return 0;
        Command c;c.id=next++;c.op=op;c.ref=std::move(ref);c.policy=policy;c.secret=std::move(secret);auto id=c.id;commands.push_back(std::move(c));++outstanding;cv.notify_one();return id;
    }
};
CredentialService::CredentialService(std::filesystem::path p):state_(std::make_shared<State>(std::move(p))){auto s=state_;std::thread([s]{s->run();}).detach();}
CredentialService::~CredentialService(){shutdown();}
std::shared_ptr<const CredentialSnapshot> CredentialService::snapshot() const noexcept{return std::atomic_load(&state_->published);}
CredentialCommandId CredentialService::store(CredentialRef r,CredentialPolicy p,CredentialSecret s){return state_->submit(State::Op::Store,std::move(r),p,std::move(s));}
CredentialCommandId CredentialService::load(CredentialRef r,CredentialPolicy p){return state_->submit(State::Op::Load,std::move(r),p);}
CredentialCommandId CredentialService::erase(CredentialRef r){return state_->submit(State::Op::Erase,std::move(r));}
std::vector<CredentialResult> CredentialService::drain_results(){std::unique_lock guard(state_->mutex,std::try_to_lock);if(!guard.owns_lock())return {};std::vector<CredentialResult> out;out.swap(state_->results);state_->outstanding-=out.size();return out;}
void CredentialService::shutdown() noexcept {std::lock_guard guard(state_->mutex);state_->stopping=true;state_->cv.notify_one();}
bool CredentialService::stopped() const noexcept{return state_->done.load();}
} // namespace jianssh::app
