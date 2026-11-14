#include "jianssh/metrics/collector.hpp"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <type_traits>
#include <limits>
#include <locale>
#include <sstream>

namespace jianssh::metrics {
std::string_view Collector::script() {
    return R"JSH(LC_ALL=C
LANG=C
export LC_ALL LANG
exec 2>/dev/null
if [ "$(uname -s)" != Linux ]; then printf 'JIANSSH_METRICS 1 Unsupported\nEND\n'; exit 0; fi
if ! command -v awk >/dev/null 2>&1; then printf 'JIANSSH_METRICS 1 Linux\nE 无法执行 awk\nEND\n'; exit 0; fi
{
printf 'JIANSSH_METRICS 1 Linux\n'
awk '/^cpu/ {printf "C %s",$1; for(i=2;i<=9;i++) printf " %.0f",$i+0; print ""; n++} END {if(!n) print "E 无法读取 /proc/stat"}' /proc/stat
awk '{print "M",$1,$2}' /proc/meminfo
awk '{print "L",$1,$2,$3}' /proc/loadavg
awk 'NR>2 {gsub(":"," "); print "N",$1,$2,$10}' /proc/net/dev
if command -v df >/dev/null 2>&1; then
 df -Pk 2>/dev/null | awk 'NR>1 && NF>=6 {printf "D %s %s %s %s %s",$1,$2,$3,$4,$5; for(i=6;i<=NF;i++) printf " %s",$i; print ""}'
else printf 'E 无法执行 df\n'; fi
hz=$(getconf CLK_TCK 2>/dev/null); pages=$(getconf PAGESIZE 2>/dev/null)
case "$hz:$pages" in *[!0-9:]*|:*|*:) printf 'E 无法获取进程单位\n';;
*) printf 'U %s %s\n' "$hz" "$pages"
awk 'FNR==1 {s=$0; p=index(s,"("); q=0; for(i=length(s);i>p;i--) if(substr(s,i,1)==")") {q=i;break}; if(q) {pid=substr(s,1,p-1)+0; cmd=substr(s,p+1,q-p-1); rest=substr(s,q+2); split(rest,a," "); gsub(/[\t\r\n]/," ",cmd); printf "P %.0f %.0f %s %s %s\n",pid,a[12]+a[13],a[20],a[22],cmd}; if(++n>=4096) exit}' /proc/[0-9]*/stat
;; esac
printf 'END\n'
} | head -c 1048576
)JSH";
}
namespace {
template<class T> bool field(std::istringstream& in, T& value) {
    if constexpr (std::is_same_v<T,std::string>) return bool(in >> value);
    else {
        std::string token; if(!(in>>token)) return false;
        auto [end,ec]=std::from_chars(token.data(),token.data()+token.size(),value);
        return ec==std::errc{} && end==token.data()+token.size();
    }
}
template<class... T> bool fields(std::istringstream& in, T&... values) { return (field(in,values) && ...); }
std::optional<double> percent(std::uint64_t used, std::uint64_t total) {
    if (!total || used > total) return {};
    return 100.0 * static_cast<double>(used) / static_cast<double>(total);
}
bool add(std::uint64_t& a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) return false;
    a += b; return true;
}
}
bool Collector::begin_sample() { std::lock_guard lock(mutex_); if(value_.sampling) return false; value_.sampling = true; return true; }
Snapshot Collector::snapshot() const { std::lock_guard lock(mutex_); return value_; }
void Collector::fail_locked(std::string error, Clock::time_point now) {
    value_.sampling = false; value_.stale = true; value_.status = Status::Error; value_.error = std::move(error);
    while(!value_.history.empty() && now - value_.history.front().time > std::chrono::seconds(300)) value_.history.pop_front();
    // Errors break the baseline: never smear a missing interval into a later sample.
    baseline_ = false;
}
void Collector::fail(std::string error, Clock::time_point now) { std::lock_guard lock(mutex_); fail_locked(std::move(error), now); }
bool Collector::consume(std::string_view output, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    auto bad = [&](std::string e) { fail_locked(std::move(e), now); return false; };
    if(output.size() > output_limit || output.find('\0') != std::string_view::npos) return bad("监控输出无效或过长");
    std::istringstream input{std::string(output)}; input.imbue(std::locale::classic());
    std::string line;
    if(!std::getline(input,line)) return bad("监控输出为空");
    if(line == "JIANSSH_METRICS 1 Unsupported") {
        if(!std::getline(input,line) || line!="END") return bad("不支持的系统监控输出被截断");
        while(std::getline(input,line)) if(!line.empty()) return bad("监控输出包含多余内容");
        value_ = Snapshot{}; value_.status = Status::Unsupported; value_.error = "远端操作系统不受支持"; baseline_ = false; return true;
    }
    if(line != "JIANSSH_METRICS 1 Linux") return bad("监控协议或版本不受支持");
    Snapshot next; next.status = Status::Ready; next.stale = false; next.sampled_at = now;
    std::map<std::string,Counters> cpus;
    std::map<std::string,std::pair<std::uint64_t,std::uint64_t>> nets;
    std::map<std::uint64_t,ProcCounters> procs;
    std::map<std::string,std::uint64_t> mem;
    std::uint64_t hz=0, pages=0; bool ended=false; bool loaded=false;
    while(std::getline(input,line)) {
        if(line == "END") { ended=true; break; }
        if(line.empty() || line.size()>16384) return bad("监控记录无效");
        std::istringstream row(line); row.imbue(std::locale::classic()); std::string tag; row>>tag;
        // Unsigned extraction accepts negative numbers; reject negative numeric tokens explicitly.
        if(tag!="E" && tag!="P" && tag!="D" && line.find(" -")!=std::string::npos) return bad("监控计数器为负数");
        if(tag=="C") {
            std::string name; std::uint64_t v[8]{}; if(!(row>>name)) return bad("处理器记录无效");
            Counters c; for(auto& n:v) { if(!field(row,n) || !add(c.total,n)) return bad("处理器计数器无效"); }
            std::copy(std::begin(v),std::end(v),c.ticks.begin());
            c.idle=v[3]; if(!add(c.idle,v[4]) || !cpus.emplace(name,c).second) return bad("处理器记录重复");
        } else if(tag=="M") {
            std::string name; std::uint64_t n; if(!fields(row,name,n) || n>std::numeric_limits<std::uint64_t>::max()/1024) return bad("内存记录无效"); mem[name]=n*1024;
        } else if(tag=="L") {
            double a,b,c; if(!fields(row,a,b,c) || !std::isfinite(a+b+c) || a<0||b<0||c<0 || loaded) return bad("负载记录无效"); next.load1=a; next.load5=b; next.load15=c; loaded=true;
        } else if(tag=="N") {
            std::string name; std::uint64_t rx,tx; if(!fields(row,name,rx,tx) || !nets.emplace(name,std::pair{rx,tx}).second) return bad("网络记录无效");
        } else if(tag=="D") {
            Disk d; std::string pc; std::uint64_t a,b,c;
            if(!fields(row,d.filesystem,a,b,c,pc) || a>UINT64_MAX/1024 || b> a || c>a) return bad("磁盘记录无效");
            std::getline(row>>std::ws,d.mount); if(d.mount.empty()) return bad("缺少挂载点");
            d.total=a*1024; d.used=b*1024; d.available=c*1024; d.percent=percent(d.used,d.total); next.disks.push_back(std::move(d));
        } else if(tag=="U") {
            if(!fields(row,hz,pages) || !hz || !pages || pages>1024*1024) return bad("进程单位无效");
        } else if(tag=="P") {
            Process p; ProcCounters c; std::int64_t rss;
            if(!fields(row,p.pid,c.ticks,c.start,rss) || rss<0 || !pages || static_cast<std::uint64_t>(rss)>UINT64_MAX/pages) return bad("进程记录无效");
            p.resident_bytes=static_cast<std::uint64_t>(rss)*pages; std::getline(row>>std::ws,p.command);
            if(!procs.emplace(p.pid,c).second) return bad("进程记录重复"); next.top.push_back(std::move(p));
        } else if(tag=="E") {
            std::string e; std::getline(row>>std::ws,e); if(!next.error.empty()) next.error += "; "; next.error += e;
        } else return bad("未知监控记录");
        if(cpus.size()>8192 || nets.size()>8192 || next.disks.size()>8192 || next.top.size()>4096) return bad("监控记录数量过多");
    }
    if(!ended) return bad("监控输出被截断（缺少结束标记）");
    while(std::getline(input,line)) if(!line.empty()) return bad("监控输出包含多余内容");
    if(cpus.empty() && mem.empty() && !loaded && nets.empty()) return bad(next.error.empty()?"没有可用的监控数据":next.error);
    double dt=std::chrono::duration<double>(now-previous_).count();
    if(baseline_ && dt<=0) return bad("采样时间没有递增");
    if(baseline_) next.sample_interval_seconds=dt;
    for(const auto& [name,c]:cpus) {
        std::optional<double> p;
        auto it=cpus_.find(name);
        if(baseline_ && it!=cpus_.end() && c.total>=it->second.total && c.idle>=it->second.idle &&
           std::equal(c.ticks.begin(),c.ticks.end(),it->second.ticks.begin(),[](auto a,auto b){return a>=b;})) {
            auto total=c.total-it->second.total, idle=c.idle-it->second.idle;
            if(idle<=total) p=percent(total-idle,total);
        }
        if(name=="cpu") next.cpu_percent=p; else next.cores.push_back({name,p});
    }
    if(mem.contains("MemTotal:") && mem.contains("MemAvailable:")) {
        Memory m; m.total=mem["MemTotal:"]; m.available=mem["MemAvailable:"];
        if(m.available>m.total) return bad("可用内存超过总内存"); m.used=m.total-m.available;
        m.cache=mem["Cached:"]; if(!add(m.cache,mem["SReclaimable:"])) return bad("内存缓存计数溢出");
        auto shmem=mem["Shmem:"]; m.cache=m.cache>shmem?m.cache-shmem:0;
        m.swap_total=mem["SwapTotal:"]; if(mem["SwapFree:"]>m.swap_total) return bad("交换分区计数无效"); m.swap_used=m.swap_total-mem["SwapFree:"]; next.memory=m;
    } else { if(!next.error.empty()) next.error+="; "; next.error+="内存计数不可用"; }
    std::optional<double> rx,tx;
    for(const auto& [name,c]:nets) {
        Network n{name,c.first,c.second,{},{}}; auto it=nets_.find(name);
        if(baseline_ && it!=nets_.end() && c.first>=it->second.first && c.second>=it->second.second) {
            n.rx_bytes_per_second=static_cast<double>(c.first-it->second.first)/dt; n.tx_bytes_per_second=static_cast<double>(c.second-it->second.second)/dt;
            if(name!="lo") { rx=rx.value_or(0)+*n.rx_bytes_per_second; tx=tx.value_or(0)+*n.tx_bytes_per_second; }
        }
        next.networks.push_back(std::move(n));
    }
    for(auto& p:next.top) {
        auto it=procs_.find(p.pid); auto c=procs.at(p.pid);
        if(baseline_ && hz && it!=procs_.end() && c.start==it->second.start && c.ticks>=it->second.ticks)
            p.cpu_percent=100.0*static_cast<double>(c.ticks-it->second.ticks)/static_cast<double>(hz)/dt;
    }
    std::sort(next.top.begin(),next.top.end(),[](const Process& a,const Process& b) { if(a.cpu_percent.value_or(-1)!=b.cpu_percent.value_or(-1)) return a.cpu_percent.value_or(-1)>b.cpu_percent.value_or(-1); return a.resident_bytes>b.resident_bytes; });
    if(next.top.size()>20) next.top.resize(20);
    next.history=std::move(value_.history);
    next.history.push_back({now,next.cpu_percent,next.memory?percent(next.memory->used,next.memory->total):std::nullopt,rx,tx});
    while(!next.history.empty() && now-next.history.front().time>std::chrono::seconds(300)) next.history.pop_front();
    value_=std::move(next); cpus_=std::move(cpus); nets_=std::move(nets); procs_=std::move(procs); previous_=now; baseline_=true; return true;
}
} // namespace jianssh::metrics
