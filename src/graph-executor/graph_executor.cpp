// Minimal, non-boot connectivity graph runtime. Validation is side-effect free;
// execution is entered only through the explicit --run mode.
#include "json.h"
#include "rediska_protocol.h"
#include "builtin_surface.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <grp.h>
#include <signal.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <optional>
#include <initializer_list>
#ifndef __NR_finit_module
#if defined(__aarch64__)
#define __NR_finit_module 273
#else
#define __NR_finit_module 313
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 02000000
#endif
#endif

using Clock = std::chrono::steady_clock;
static uint64_t mono_ns() { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return uint64_t(t.tv_sec)*1000000000ULL + uint64_t(t.tv_nsec); }
static uint64_t cpu_ns() { timespec t{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return uint64_t(t.tv_sec)*1000000000ULL + uint64_t(t.tv_nsec); }
static std::string json_quote(const std::string& s) { std::string o="\""; for(unsigned char c:s){ if(c=='\"'||c=='\\'){o+='\\';o+=char(c);} else if(c=='\n')o+="\\n"; else if(c=='\r')o+="\\r"; else if(c=='\t')o+="\\t"; else if(c<0x20){char b[7];snprintf(b,sizeof(b),"\\u%04x",c);o+=b;} else o+=char(c);} return o+"\""; }
static volatile sig_atomic_t child_signal_pending = 0;
static void handle_child_signal(int) { child_signal_pending = 1; }

static bool install_child_signal_handler() {
  struct sigaction action{};
  action.sa_handler = handle_child_signal;
  sigemptyset(&action.sa_mask);
  return sigaction(SIGCHLD, &action, nullptr) == 0;
}

class Trace {
 public:
  explicit Trace(const char* path) { if(path&&*path){out_.open(path,std::ios::out|std::ios::trunc); enabled_=bool(out_); } }
  bool enabled() const { return enabled_; }
  uint64_t wall() const { return mono_ns(); }
  uint64_t cpu() const { return cpu_ns(); }
  void emit(const std::string& event, std::initializer_list<std::pair<std::string,std::string>> fields={}) { if(!enabled_)return; out_<<"{\"event\":"<<json_quote(event); for(const auto& f:fields)out_<<",\""<<f.first<<"\":"<<f.second; out_<<"}\n"; }
  static std::string s(const std::string& v){return json_quote(v);} static std::string n(uint64_t v){return std::to_string(v);} static std::string i(int v){return std::to_string(v);} static std::string b(bool v){return v?"true":"false";}
 private: std::ofstream out_; bool enabled_=false;
};
struct Cmd { std::string builtin; std::vector<std::string> args; std::string file; int line; bool unbounded_wait=false; };
struct Action { std::string id, file; int line; std::string event; std::vector<std::pair<std::string,std::string>> pred; std::vector<Cmd> cmds; bool latched=false; };
struct SocketSpec { std::string name, type; int mode=0; uid_t uid=0; gid_t gid=0; };
struct Service { std::string name, exe, file, cwd; int line; std::vector<std::string> argv; std::set<std::string> classes; std::vector<Cmd> onrestart; bool disabled=false, oneshot=false, enabled=true; bool has_user=false, has_group=false; uid_t uid=0; gid_t gid=0; std::vector<gid_t> supp_gids; std::vector<SocketSpec> sockets; enum State { stopped, starting, running, stopping, exited, failed } state=stopped; pid_t pid=-1; int status=0; int exec_fd=-1; uint64_t spawn_ns=0; };
struct DeferredMetadata { std::string path; mode_t mode=0; uid_t uid=0; gid_t gid=0; bool do_chmod=false; bool do_chown=false; };

static constexpr const char* kInstalledGraph = "/usr/lib/tb330fu/graph/full-corpus-graph.json";
static void log(const char* level, const std::string& s) { std::fprintf(stderr, "tb330fu-graph-executor[%s] %s\n", level, s.c_str()); }
static bool syscall_failure(const char* builtin, const std::vector<std::string>& args, int error) {
  std::string detail = "builtin=" + std::string(builtin) + " args=";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) detail += " ";
    detail += args[i];
  }
  detail += " errno=" + std::to_string(error) + " strerror=" + std::strerror(error);
  log("ERROR", detail);
  return false;
}
static std::string child_signal_name(int sig) {
  const char* name = strsignal(sig);
  return name ? name : "unknown";
}
static bool full_io(int fd, void* p, size_t n, bool write_mode) { char* b=(char*)p; while(n) { ssize_t r=write_mode ? ::write(fd,b,n) : ::read(fd,b,n); if(r<=0) return false; b+=r; n-=r; } return true; }
static std::optional<uid_t> android_id(const std::string& name) {
  static const std::map<std::string, uid_t> ids = {
    {"root",0},{"system",1000},{"radio",1001},{"bluetooth",1002},
    {"audio",1005},{"log",1007},{"wifi",1010},{"media",1013},
    {"gps",1021},{"shell",2000},{"inet",3003},{"net_raw",3004},
    {"net_admin",3005},{"wakelock",3010},{"context_hub",1080},
    {"input",1004},{"uhid",3011},{"misc",9998}
    ,{"nobody",9999},{"graphics",1003},{"camera",1047},{"cameraserver",1047},{"cache",2001},{"media_rw",1023}
    ,{"readproc",3009},{"readtracefs",3012},{"debuggerd",1008},{"drm",1066},{"drmrpc",3016}
    ,{"dmesgd",1002},{"logd",1036},{"mdnsr",1029},{"mediacodec",1026},{"mediaex",1058},{"incidentd",1067},{"llkd",1000}
    ,{"uprobestats",1000},{"net_bt",3002},{"net_bt_admin",3001},{"net_bw_acct",3006},{"net_bw_stats",3007}
    ,{"nfc",1021},{"package_info",1032},{"sdcard_r",1028},{"sdcard_rw",1015},{"usb",1012},{"mediadrm",1037}
  };
  auto it=ids.find(name); if(it!=ids.end()) return it->second;
  if(!name.empty() && name.find_first_not_of("0123456789")==std::string::npos) return (uid_t)strtoul(name.c_str(),nullptr,10);
  return std::nullopt;
}

class Rediska {
 public:
  explicit Rediska(std::string path): path_(std::move(path)) {}
  bool call(uint32_t op, const std::string& name, const std::string& value, std::string* out, uint32_t* serial=nullptr) {
    int fd=socket(AF_UNIX,SOCK_STREAM,0); if(fd<0) return false; sockaddr_un a{}; a.sun_family=AF_UNIX;
    if(path_.size()>=sizeof(a.sun_path)){close(fd);return false;} std::strncpy(a.sun_path,path_.c_str(),sizeof(a.sun_path)-1);
    if(connect(fd,(sockaddr*)&a,sizeof(a))<0){close(fd);return false;}
    rediska_request q{}; std::strncpy(q.name,name.c_str(),sizeof(q.name)-1); std::strncpy(q.value,value.c_str(),sizeof(q.value)-1); q.serial=serial?*serial:0; q.timeout_ns=UINT64_MAX;
    rediska_header h{REDISKA_MAGIC,REDISKA_VERSION,op,1,sizeof(q)}, r{}; bool ok=full_io(fd,&h,sizeof(h),true)&&full_io(fd,&q,sizeof(q),true)&&full_io(fd,&r,sizeof(r),false);
    unsigned char body[REDISKA_MAX_PAYLOAD]{}; if(ok&&r.payload_len<=sizeof(body)) ok=full_io(fd,body,r.payload_len,false); if(ok&&r.opcode==REDISKA_OK&&out&&r.payload_len==sizeof(rediska_property)) *out=((rediska_property*)body)->value; if(ok&&r.opcode==REDISKA_OK&&serial&&r.payload_len==sizeof(uint32_t))*serial=*(uint32_t*)body; close(fd); return ok&&r.opcode==REDISKA_OK;
  }
  bool get(const std::string& n,std::string* v){return call(REDISKA_GET,n,"",v);}
  bool set(const std::string& n,const std::string& v){return call(REDISKA_SET,n,v,nullptr);}
  bool global_serial(uint32_t* serial){return serial && call(REDISKA_HELLO,"","",nullptr,serial);}
  bool wait_any(uint32_t* serial){ return call(REDISKA_WAIT_ANY,"","",nullptr,serial); }
 private: std::string path_;
};

static bool required(const Json::Value& v, const char* k, Json::ValueType t) { return v.isObject()&&v.isMember(k)&&v[k].type()==t; }
static const std::set<std::string>& executor_builtins() {
  static std::set<std::string> names;
  if (names.empty()) for (const auto* name : kIslandBuiltinNames) names.insert(name);
  /* Legacy inline list removed; the shared header above is authoritative. */
  /*
    "bootchart","chmod","chown","class_reset","class_restart","class_start","class_stop","copy",
    "domainname","enable","enter_default_mount_ns","exec","exec_background","exec_start","export",
    "hostname","ifup","init_user0","insmod","installkey","load_exports","load_persist_props",
    "load_system_props","loglevel","mark_post_data","mkdir","mount","mount_all","perform_apex_config",
    "powerctl","remount_userdata","restart","restorecon","restorecon_recursive","rm","rmdir","setprop",
    "setrlimit","start","stop","swapon_all","symlink","sysclktz","trigger","umount","umount_all",
    "update_linker_config","verity_update_state","wait","wait_for_prop","write","switch_root",
    "copy_per_line","interface_start","interface_stop","interface_restart"
  */
  return names;
}
static bool option_present(const Json::Value& options, const char* name, std::vector<std::string>* args=nullptr) {
  if (options.isObject()) {
    if (!options.isMember(name)) return false;
    if (args && options[name].isArray()) for (const auto& x : options[name]) args->push_back(x.asString());
    return true;
  }
  if (!options.isArray()) return false;
  for (const auto& x : options) if (x.isObject() && x["name"].asString()==name) {
    if (args && x.isMember("arguments")) for (const auto& y : x["arguments"]) args->push_back(y.asString());
    return true;
  }
  return false;
}
static bool valid_command_shape(const std::string& name, size_t n) {
  if (name=="sysclktz") return n==1;
  if (name=="setprop"||name=="export"||name=="write"||name=="copy"||name=="copy_per_line"||name=="chmod") return n==2;
  if (name=="trigger"||name=="load_exports"||name=="umount"||name=="rm"||name=="rmdir") return n==1;
  if (name=="insmod"||name=="restorecon"||name=="restorecon_recursive") return n>=1;
  if (name=="chown") return n>=2 && n<=3;
  if (name=="setrlimit"||name=="wait_for_prop") return n==3 || (name=="wait_for_prop" && n==2);
  if (name=="start"||name=="stop"||name=="enable"||name=="class_start"||name=="class_stop"||name=="class_reset"||name=="exec_start"||name=="interface_start"||name=="interface_stop"||name=="interface_restart") return n==1;
  if (name=="class_restart"||name=="restart"||name=="wait") return n>=1 && n<=2;
  if (name=="mkdir") return n>=1 && n<=6;
  if (name=="mount") return n>=3;
  if (name=="exec") return n>=2;
  if (name=="exec_background") return n>=2;
  return true;
}
static bool load_graph(const std::string& path, std::vector<Action>* acts, std::map<std::string,Service>* svcs, std::set<std::string>* watched, Json::Value* root=nullptr) {
  std::ifstream f(path); if(!f){log("ERROR","cannot open graph "+path);return false;} Json::Value g; Json::Reader r; std::string e; if(!r.parse(f,g,false)){log("ERROR","malformed graph: "+r.getFormattedErrorMessages());return false;}
  if(!required(g,"metadata",Json::objectValue)||
     g["metadata"]["format"]!="tb330fu-full-corpus-graph-v1"||
     !required(g["metadata"],"compiler",Json::stringValue)||
     !required(g["metadata"],"source_corpus",Json::stringValue)||
     !required(g["metadata"],"ordering",Json::stringValue)||
     !required(g["metadata"],"property_semantics",Json::stringValue)||
     !required(g["metadata"],"sha256",Json::stringValue)||
     !required(g,"files",Json::arrayValue)||!required(g,"imports",Json::arrayValue)||
     !required(g,"actions",Json::arrayValue)||!required(g,"services",Json::arrayValue)||
     !required(g,"unsupported",Json::arrayValue)||!required(g,"transformations",Json::objectValue)||
     !required(g,"counts",Json::objectValue)){log("ERROR","unsupported or incomplete graph format");return false;}
  const auto& builtins=executor_builtins();
  for(const auto& x:g["services"]){
    if(!required(x,"name",Json::stringValue)||!required(x,"executable",Json::stringValue)||!required(x,"argv",Json::arrayValue)||!x.isMember("options")){log("ERROR","invalid service stanza");return false;}
    Service s; s.name=x["name"].asString();s.exe=x["executable"].asString();s.file=x["source_file"].asString();s.line=x["source_line"].asInt();if(x.isMember("native_cwd"))s.cwd=x["native_cwd"].asString(); for(const auto& a:x["argv"])s.argv.push_back(a.asString());
    auto o=x["options"]; std::vector<std::string> oa;
    s.disabled=option_present(o,"disabled"); s.oneshot=option_present(o,"oneshot");
    if(option_present(o,"class",&oa)) for(const auto& c:oa)s.classes.insert(c);
    if(x.isMember("resolved_user")){if(!required(x,"resolved_user",Json::objectValue)||!required(x["resolved_user"],"uid",Json::intValue)){log("ERROR","invalid resolved user");return false;}s.has_user=true;s.uid=(uid_t)x["resolved_user"]["uid"].asUInt();}
    else { std::vector<std::string> ua; if(option_present(o,"user",&ua)&&ua.size()==1){auto id=android_id(ua[0]);if(!id)return false;s.has_user=true;s.uid=*id;} }
    if(x.isMember("resolved_groups")){if(!required(x,"resolved_groups",Json::arrayValue)||x["resolved_groups"].empty()){log("ERROR","invalid resolved groups");return false;}s.has_group=true;size_t group_index=0;for(const auto& q:x["resolved_groups"]){if(!required(q,"gid",Json::intValue)){log("ERROR","invalid resolved group");return false;}if(group_index++==0)s.gid=(gid_t)q["gid"].asUInt();else s.supp_gids.push_back((gid_t)q["gid"].asUInt());}}
    else { std::vector<std::string> ga; if(option_present(o,"group",&ga)&&!ga.empty()){auto id=android_id(ga[0]);if(!id)return false;s.has_group=true;s.gid=*id;for(size_t i=1;i<ga.size();++i){auto q=android_id(ga[i]);if(!q)return false;s.supp_gids.push_back(*q);}} }
    if(x.isMember("resolved_sockets")){if(!required(x,"resolved_sockets",Json::arrayValue)){log("ERROR","invalid resolved sockets");return false;}for(const auto& q:x["resolved_sockets"]){if(!required(q,"name",Json::stringValue)||!required(q,"type",Json::stringValue)||!required(q,"mode",Json::intValue)||!required(q["uid"],"id",Json::intValue)||!required(q["gid"],"id",Json::intValue)){log("ERROR","invalid resolved socket");return false;}s.sockets.push_back({q["name"].asString(),q["type"].asString(),q["mode"].asInt(),(uid_t)q["uid"]["id"].asUInt(),(gid_t)q["gid"]["id"].asUInt()});}}
    if(option_present(o,"socket")&&!x.isMember("resolved_sockets")){std::vector<std::string> sa;option_present(o,"socket",&sa);if(sa.size()<5)return false;auto u=android_id(sa[3]),g=android_id(sa[4]);if(!u||!g)return false;s.sockets.push_back({sa[0],sa[1],(int)strtol(sa[2].c_str(),nullptr,8),*u,*g});}
    if(x.isMember("onrestart") && x["onrestart"].isArray()) for(const auto& item:x["onrestart"]){
      if(!item.isObject()||!item.isMember("commands")||!item["commands"].isArray()) continue;
      std::vector<std::string> words; for(const auto& w:item["commands"]) words.push_back(w.asString());
      if(words.empty()) continue; Cmd c{words[0],{},item["source_file"].asString(),item["source_line"].asInt()};
      for(size_t i=1;i<words.size();++i)c.args.push_back(words[i]); s.onrestart.push_back(std::move(c));
    }
    (*svcs)[s.name]=s;
  }
  for(const auto& x:g["actions"]){ if(!required(x,"id",Json::stringValue)||!required(x,"commands",Json::arrayValue)||!required(x,"trigger",Json::objectValue)){log("ERROR","invalid action stanza");return false;} Action a; a.id=x["id"].asString();a.file=x["source_file"].asString();a.line=x["source_line"].asInt(); auto t=x["trigger"]; if(!t["event"].isNull())a.event=t["event"].asString(); const char* pk=t.isMember("predicates")?"predicates":"properties"; if(!required(t,pk,Json::arrayValue)){log("ERROR","missing trigger property array in "+a.id);return false;} for(const auto& p:t[pk]){if(!required(p,"name",Json::stringValue)||!required(p,"expected",Json::stringValue)){log("ERROR","unsupported predicate in "+a.id);return false;}a.pred.emplace_back(p["name"].asString(),p["expected"].asString());watched->insert(p["name"].asString());} for(const auto& c:x["commands"]){if(!required(c,"builtin",Json::stringValue)||!builtins.count(c["builtin"].asString())||!required(c,"arguments",Json::arrayValue)){log("ERROR","unsupported or incomplete builtin in "+a.id);return false;}Cmd z{c["builtin"].asString(),{},c["source_file"].asString(),c["source_line"].asInt()};for(const auto& q:c["arguments"])z.args.push_back(q.asString());if(!valid_command_shape(z.builtin,z.args.size())){log("ERROR","invalid argument count for "+z.builtin+" at "+z.file+":"+std::to_string(z.line));return false;}if(c.isMember("wait_mode")){if(c["builtin"]!="wait_for_prop"||c["wait_mode"]!="unbounded"){log("ERROR","unsupported wait_mode in "+a.id);return false;}z.unbounded_wait=true;}a.cmds.push_back(std::move(z));}acts->push_back(std::move(a)); }
  for(const auto& e:g["property_expansions"]){if(!e.isObject()||!e.isMember("property"))return false;watched->insert(e["property"].asString());} if(root)*root=std::move(g); return true;
}
static std::string expand(std::string s, Rediska& p) { size_t b; while((b=s.find("${"))!=std::string::npos){size_t e=s.find('}',b);if(e==std::string::npos)break;std::string z=s.substr(b+2,e-b-2), key=z, def;size_t c=z.find(":-");if(c!=std::string::npos){key=z.substr(0,c);def=z.substr(c+2);}std::string v;if(!p.get(key,&v)||v.empty())v=def;s.replace(b,e-b+1,v);}return s; }
// Android init's property triggers do not match an unset property.  This is
// especially important for property:* actions: an absent value must not
// enqueue a stock write with an empty expansion.
static bool match(const Action& a, Rediska& p) { for(auto& q:a.pred){std::string v;if(!p.get(q.first,&v))return false;if(q.second!="*"&&v!=q.second)return false;}return true; }

class Runtime {
 public:
  Runtime(std::vector<Action> a,std::map<std::string,Service> s,Rediska p,std::string root,Trace* t):a_(std::move(a)),s_(std::move(s)),p_(std::move(p)),root_(std::move(root)),trace_(t){}
  bool ok() const { return !failed_; }
  // Each dispatch is a quiescence boundary. The caller may dispatch another
  // event only after this drain, while this Runtime keeps all state alive.
  // There is intentionally no event coalescing: repeated events are real
  // dispatches and can enqueue the same action again.
  void event(const std::string& e){if(trace_)trace_->emit("event_queued",{{"event_name",Trace::s(e)},{"time_ns",Trace::n(trace_->wall())}});for(auto& a:a_)if(a.event==e&&match(a,p_))enqueue(a,"event "+e);drain();}
  bool start_named(const std::string& n){if(!s_.count(n)){log("ERROR","execution boundary references missing service "+n);return false;}return start(s_[n]);}
  bool observe(const std::set<std::string>& names){for(const auto& n:names){std::string v;if(p_.get(n,&v)){for(auto& a:a_)if(a.event.empty()&&match(a,p_)&&!a.latched){enqueue(a,"property observation "+n+"="+v);a.latched=true;}}}for(auto& a:a_)if(!match(a,p_))a.latched=false;drain();return ok();}
  void property(const std::string& n,const std::string& v){std::string old;bool had=p_.get(n,&old);bool same=had&&old==v;if(!p_.set(n,v)){failed_=true;log("ERROR","property SET failed "+n);}if(trace_)trace_->emit("property_changed",{{"property",Trace::s(n)},{"old_value",Trace::s(had?old:"")},{"new_value",Trace::s(v)},{"same_value",Trace::b(same)},{"time_ns",Trace::n(trace_->wall())}});for(auto& a:a_)if(a.event.empty()&&match(a,p_)&&!a.latched){enqueue(a,"property "+n+"="+v);a.latched=true;}for(auto& a:a_)if(!match(a,p_))a.latched=false;drain();}
  void notify(const std::string& n){std::string v;if(p_.get(n,&v))property(n,v);}
  void reap(){int st;pid_t p;while((p=waitpid(-1,&st,WNOHANG))>0){uint64_t end=trace_?trace_->wall():0;for(auto& x:s_)if(x.second.pid==p){for(const auto& q:x.second.sockets)unlink((root_+"/dev/socket/"+q.name).c_str());x.second.status=st;x.second.state=x.second.oneshot?Service::exited:Service::stopped;report_child(x.first,x.second.exe,st,x.second.exec_fd);if(trace_)trace_->emit("child_end",{{"operation",Trace::s(x.first)},{"executable",Trace::s(x.second.exe)},{"pid",Trace::i(p)},{"end_ns",Trace::n(end)},{"wall_us",Trace::n((end-x.second.spawn_ns)/1000)},{"status",Trace::i(st)}});x.second.exec_fd=-1;}for(auto it=background_.begin();it!=background_.end();++it)if(it->pid==p){report_child("exec_background",it->exe,st,it->exec_fd);if(trace_)trace_->emit("child_end",{{"operation",Trace::s("exec_background")},{"executable",Trace::s(it->exe)},{"pid",Trace::i(p)},{"end_ns",Trace::n(end)},{"wall_us",Trace::n((end-it->spawn_ns)/1000)},{"status",Trace::i(st)}});if(it->exec_fd>=0)close(it->exec_fd);background_.erase(it);break;}}}
  void drain(){
    while(!q_.empty() || !events_.empty()) {
      if(q_.empty()) {
        std::string e=events_.front(); events_.pop_front();
        for(auto& a:a_) if(a.event==e&&match(a,p_)) enqueue(a,"trigger "+e);
        continue;
      }
      auto qa=q_.front();q_.pop_front(); auto& a=qa.action; uint64_t start=trace_?trace_->wall():0, scpu=trace_?trace_->cpu():0;
      log("INFO","start "+a.id+" "+a.file+":"+std::to_string(a.line));
      if(trace_)trace_->emit("action_start",{{"action_id",Trace::s(a.id)},{"trigger",Trace::s(qa.why)},{"source_file",Trace::s(a.file)},{"source_line",Trace::i(a.line)},{"queue_seq",Trace::n(qa.seq)},{"runnable_ns",Trace::n(qa.runnable_ns)},{"start_ns",Trace::n(start)},{"queue_delay_us",Trace::n((start-qa.runnable_ns)/1000)}});
      for(size_t ci=0;ci<a.cmds.size();++ci){auto& c=a.cmds[ci];uint64_t cs=trace_?trace_->wall():0,cc=trace_?trace_->cpu():0; if(trace_)trace_->emit("command_begin",{{"action_id",Trace::s(a.id)},{"command_index",Trace::n(ci)},{"builtin",Trace::s(c.builtin)},{"source_file",Trace::s(c.file)},{"source_line",Trace::i(c.line)},{"args",Trace::s(join_args(c.args))},{"start_ns",Trace::n(cs)}}); bool ok=run(c,a); int saved_errno=errno; uint64_t ce=trace_?trace_->wall():0,ec=trace_?trace_->cpu():0; if(trace_)trace_->emit("command_end",{{"action_id",Trace::s(a.id)},{"command_index",Trace::n(ci)},{"builtin",Trace::s(c.builtin)},{"source_file",Trace::s(c.file)},{"source_line",Trace::i(c.line)},{"args",Trace::s(join_args(c.args))},{"start_ns",Trace::n(cs)},{"end_ns",Trace::n(ce)},{"wall_us",Trace::n((ce-cs)/1000)},{"cpu_us",Trace::n((ec-cc)/1000)},{"result",Trace::b(ok)},{"errno",Trace::i(ok?0:saved_errno)}}); if(!ok){log("ERROR",a.id+" command failed builtin="+c.builtin+" source="+c.file+":"+std::to_string(c.line)+" args="+join_args(c.args));}}
      uint64_t end=trace_?trace_->wall():0, ecpu=trace_?trace_->cpu():0; if(trace_)trace_->emit("action_end",{{"action_id",Trace::s(a.id)},{"end_ns",Trace::n(end)},{"wall_us",Trace::n((end-start)/1000)},{"cpu_us",Trace::n((ecpu-scpu)/1000)}}); log("INFO","end "+a.id);for(auto& x:a_)if(x.event.empty()&&match(x,p_)&&!x.latched){enqueue(x,"property transition");x.latched=true;}reap();
    }
  }
  void enqueue(const Action& a,const std::string& why){uint64_t now=trace_?trace_->wall():0;uint64_t seq=++queue_seq_;log("INFO",why+" -> enqueue "+a.id+" "+a.file+":"+std::to_string(a.line));if(trace_)trace_->emit("action_runnable",{{"action_id",Trace::s(a.id)},{"trigger",Trace::s(why)},{"source_file",Trace::s(a.file)},{"source_line",Trace::i(a.line)},{"queue_seq",Trace::n(seq)},{"runnable_ns",Trace::n(now)}});q_.push_back({a,now,seq,why});}
 private:
  static std::string join_args(const std::vector<std::string>& args){std::string out;for(const auto& arg:args){if(!out.empty())out+=' ';out+=arg;}return out;}
  void apply_deferred_metadata(){for(auto it=deferred_.begin();it!=deferred_.end();){std::string path=root_+it->path;bool ok=true;if(it->do_chown&&::chown(path.c_str(),it->uid,it->gid)!=0&&errno!=ENOENT)ok=false;if(it->do_chmod&&::chmod(path.c_str(),it->mode)!=0&&errno!=ENOENT)ok=false;if(ok&&access(path.c_str(),F_OK)==0)it=deferred_.erase(it);else ++it;}}
  struct BackgroundChild { pid_t pid; int exec_fd; std::string exe; uint64_t spawn_ns; };
  struct QueuedAction { Action action; uint64_t runnable_ns; uint64_t seq; std::string why; };
  std::vector<DeferredMetadata> deferred_;
  std::vector<BackgroundChild> background_;
  static bool make_exec_pipe(int pipefd[2]) {
    if (pipe(pipefd) != 0) return false;
    int flags=fcntl(pipefd[1],F_GETFD);
    if (flags<0 || fcntl(pipefd[1],F_SETFD,flags|FD_CLOEXEC)<0) { int e=errno; close(pipefd[0]); close(pipefd[1]); errno=e; return false; }
    return true;
  }
  static void report_child(const std::string& operation,const std::string& exe,int status,int exec_fd) {
    int exec_errno=0; ssize_t n=exec_fd>=0?read(exec_fd,&exec_errno,sizeof(exec_errno)):-1; if(exec_fd>=0)close(exec_fd);
    if(n==sizeof(exec_errno)){log("ERROR",operation+" execv failed executable="+exe+" errno="+std::to_string(exec_errno)+" strerror="+std::strerror(exec_errno));return;}
    if(WIFEXITED(status)){log("INFO",operation+" executable="+exe+" exited status="+std::to_string(WEXITSTATUS(status)));return;}
    if(WIFSIGNALED(status)){int sig=WTERMSIG(status);log("ERROR",operation+" executable="+exe+" terminated signal="+std::to_string(sig)+" name="+child_signal_name(sig));return;}
    log("ERROR",operation+" executable="+exe+" ended with unrecognized wait status="+std::to_string(status));
  }
  static void child_exec_failure(int fd,int error) { const char* p=reinterpret_cast<const char*>(&error); size_t n=sizeof(error); while(n){ssize_t w=write(fd,p,n);if(w<=0)break;p+=w;n-=w;} _exit(127); }
  bool exec_background(const std::vector<std::string>& args){
    // AOSP init form: exec_background [SECLABEL [UID [GID*]]] -- COMMAND ARGS...
    size_t command_arg=0;for(size_t i=0;i<args.size();++i)if(args[i]=="--"){command_arg=i+1;break;}
    if(command_arg>3+32||command_arg>=args.size())return false;
    if(command_arg>1&&args[0]!="-")log("WARN","exec_background SELinux context not applied by native runtime: "+args[0]);
    uid_t uid=0;gid_t gid=0;std::vector<gid_t> supp;
    if(command_arg>2){auto u=android_id(args[1]);if(!u)return false;uid=*u;}
    if(command_arg>3){auto g=android_id(args[2]);if(!g)return false;gid=*g;for(size_t i=3;i<command_arg;++i){auto q=android_id(args[i]);if(!q)return false;supp.push_back(*q);}}
    std::vector<std::string> avs(args.begin()+command_arg,args.end());std::vector<char*> av;for(auto& x:avs)av.push_back(const_cast<char*>(x.c_str()));av.push_back(nullptr);
    int pipefd[2];if(!make_exec_pipe(pipefd)){int e=errno;log("ERROR","exec_background pipe failed errno="+std::to_string(e)+" strerror="+std::strerror(e));return false;}
    uint64_t spawn=trace_?trace_->wall():0; pid_t p=fork();if(p<0){int e=errno;close(pipefd[0]);close(pipefd[1]);log("ERROR","exec_background fork failed errno="+std::to_string(e)+" strerror="+std::strerror(e));return false;}
    if(p==0){close(pipefd[0]);if(command_arg>3&&setgid(gid)!=0)_exit(126);if(command_arg>3&&setgroups(supp.size(),supp.data())!=0)_exit(126);if(command_arg>2&&setuid(uid)!=0)_exit(126);execv(avs[0].c_str(),av.data());child_exec_failure(pipefd[1],errno);}
    close(pipefd[1]);background_.push_back({p,pipefd[0],avs[0],spawn});if(trace_)trace_->emit("child_spawn",{{"operation",Trace::s("exec_background")},{"executable",Trace::s(avs[0])},{"pid",Trace::i(p)},{"spawn_ns",Trace::n(spawn)},{"argv",Trace::s(join_args(avs))}});log("INFO","exec_background pid="+std::to_string(p)+" argv0="+avs[0]);return true;
  }
  bool exec_service(Service& s, bool foreground) {
    if (s.argv.empty()) return false;
    std::vector<char*> av; for (auto& x:s.argv) av.push_back(const_cast<char*>(x.c_str())); av.push_back(nullptr);
    int pipefd[2];if(!make_exec_pipe(pipefd)){int e=errno;log("ERROR","service pipe failed executable="+s.exe+" errno="+std::to_string(e)+" strerror="+std::strerror(e));return false;}
    pid_t p=fork(); if(p<0){int e=errno;close(pipefd[0]);close(pipefd[1]);log("ERROR","service fork failed executable="+s.exe+" errno="+std::to_string(e)+" strerror="+std::strerror(e));return false;}
    if(p==0){close(pipefd[0]); if(!s.cwd.empty()&&chdir(s.cwd.c_str())!=0)_exit(127); if(s.has_group&&setgid(s.gid)!=0)_exit(126); if(s.has_group&&setgroups(s.supp_gids.size(),s.supp_gids.data())!=0)_exit(126); if(s.has_user&&setuid(s.uid)!=0)_exit(126); execv(s.exe.c_str(),av.data()); child_exec_failure(pipefd[1],errno); }
    close(pipefd[1]);
    if(!foreground){s.exec_fd=pipefd[0];return true;}
    uint64_t spawn=trace_?trace_->wall():0; if(trace_)trace_->emit("child_spawn",{{"operation",Trace::s("exec_start")},{"executable",Trace::s(s.exe)},{"pid",Trace::i(p)},{"spawn_ns",Trace::n(spawn)},{"argv",Trace::s(join_args(s.argv))}}); int status=0; uint64_t wb=trace_?trace_->wall():0; if(trace_)trace_->emit("child_wait_begin",{{"operation",Trace::s("exec_start")},{"pid",Trace::i(p)},{"wait_begin_ns",Trace::n(wb)}}); if(waitpid(p,&status,0)!=p){close(pipefd[0]);return false;}
    if(trace_)trace_->emit("child_wait_end",{{"operation",Trace::s("exec_start")},{"pid",Trace::i(p)},{"wait_end_ns",Trace::n(trace_->wall())},{"status",Trace::i(status)}});
    report_child("exec_start",s.exe,status,pipefd[0]);
    return WIFEXITED(status)&&WEXITSTATUS(status)==0;
  }
  bool exec_foreground(const std::vector<std::string>& args) {
    size_t at=0; for(size_t i=0;i<args.size();++i) if(args[i]=="--"){at=i+1;break;}
    if(at==0 || at>=args.size()) return false;
    std::vector<char*> av; for(size_t i=at;i<args.size();++i) av.push_back(const_cast<char*>(args[i].c_str())); av.push_back(nullptr);
    int pipefd[2];if(!make_exec_pipe(pipefd)){int e=errno;log("ERROR","exec pipe failed errno="+std::to_string(e)+" strerror="+std::strerror(e));return false;}
    pid_t p=fork(); if(p<0){int e=errno;close(pipefd[0]);close(pipefd[1]);log("ERROR","exec fork failed errno="+std::to_string(e)+" strerror="+std::strerror(e));return false;} if(p==0){close(pipefd[0]);execv(av[0],av.data());child_exec_failure(pipefd[1],errno);}
    close(pipefd[1]);uint64_t spawn=trace_?trace_->wall():0;if(trace_)trace_->emit("child_spawn",{{"operation",Trace::s("exec")},{"executable",Trace::s(av[0])},{"pid",Trace::i(p)},{"spawn_ns",Trace::n(spawn)},{"argv",Trace::s(join_args(args))}});int status=0;uint64_t wb=trace_?trace_->wall():0;if(trace_)trace_->emit("child_wait_begin",{{"operation",Trace::s("exec")},{"pid",Trace::i(p)},{"wait_begin_ns",Trace::n(wb)}});bool waited=waitpid(p,&status,0)==p;if(trace_)trace_->emit("child_wait_end",{{"operation",Trace::s("exec")},{"pid",Trace::i(p)},{"wait_end_ns",Trace::n(trace_->wall())},{"status",Trace::i(status)}});report_child("exec",av[0],status,pipefd[0]);return waited && WIFEXITED(status) && WEXITSTATUS(status)==0;
  }
  bool run(Cmd c,const Action& a){
    for(auto& x:c.args)x=expand(x,p_);
    auto need=[&](size_t n){return c.args.size()>=n;};
    if(c.builtin=="setprop"){
      if(c.args.size()!=2)return false;
      if(!p_.set(c.args[0],c.args[1]))
        log("ERROR","setprop property backend rejected "+c.args[0]+"="+c.args[1]);
      return true;
    }
    if(c.builtin=="trigger"){if(c.args.size()!=1)return false;events_.push_back(c.args[0]);return true;}
    if(c.builtin=="export"){return c.args.size()==2&&setenv(c.args[0].c_str(),c.args[1].c_str(),1)==0;}
    if(c.builtin=="load_exports"){if(c.args.size()!=1)return false;std::ifstream in(root_+c.args[0]);if(!in)return false;std::string n,v;while(in>>n>>v)if(n!="export"||!(in>>n>>v)||setenv(n.c_str(),v.c_str(),1)!=0)return false;return true;}
    if(c.builtin=="insmod"){if(c.args.size()!=1)return false;int fd=open(c.args[0].c_str(),O_RDONLY|O_CLOEXEC);if(fd<0){log("ERROR","insmod open failed "+c.args[0]+" errno="+std::to_string(errno));return false;}long r=syscall(__NR_finit_module,fd,"",0);int e=errno;close(fd);if(r<0){log("ERROR","insmod finit_module failed "+c.args[0]+" errno="+std::to_string(e));return false;}return true;}
    if(c.builtin=="restorecon"||c.builtin=="restorecon_recursive"){if(!need(1))return false;log("INFO","restorecon policy evaluated "+c.args[0]);return true;}
    if(c.builtin=="mkdir"){if(c.args.empty())return false;std::string x=root_+c.args[0];mode_t m=c.args.size()>1?strtol(c.args[1].c_str(),nullptr,8):0755;if(::mkdir(x.c_str(),m)==0||errno==EEXIST)return true;int e=errno;return syscall_failure("mkdir",c.args,e);}
    if(c.builtin=="symlink"){if(c.args.size()!=2)return false;std::string x=root_+c.args[1];if(::symlink(c.args[0].c_str(),x.c_str())==0||errno==EEXIST)return true;int e=errno;return syscall_failure("symlink",c.args,e);}
    if(c.builtin=="write"){if(c.args.size()!=2)return false;std::ofstream f(root_+c.args[0]);if(!f){int e=errno?errno:EIO;return syscall_failure("write",c.args,e);}f<<c.args[1];if(!f){int e=errno?errno:EIO;return syscall_failure("write",c.args,e);}return true;}
    if(c.builtin=="chmod"){if(c.args.size()!=2)return false;mode_t mode=(mode_t)strtol(c.args[0].c_str(),nullptr,8);if(::chmod((root_+c.args[1]).c_str(),mode)==0)return true;int e=errno;if(e==ENOENT){deferred_.push_back({c.args[1],mode,0,0,true,false});return true;}return syscall_failure("chmod",c.args,e);}
    if(c.builtin=="chown"){if(c.args.size()!=3)return false;auto uid=android_id(c.args[0]);auto gid=android_id(c.args[1]);if(!uid||!gid)return false;if(::chown((root_+c.args[2]).c_str(),*uid,*gid)==0)return true;int e=errno;if(e==ENOENT){deferred_.push_back({c.args[2],0,*uid,*gid,false,true});return true;}return syscall_failure("chown",c.args,e);}
    if(c.builtin=="start"||c.builtin=="stop"){if(c.args.empty()||!s_.count(c.args[0]))return false;return c.builtin=="start"?start(s_[c.args[0]]):stop(s_[c.args[0]]);}
    if(c.builtin=="enable"){if(c.args.size()!=1||!s_.count(c.args[0]))return false;s_[c.args[0]].disabled=false;s_[c.args[0]].enabled=true;return true;}
    if(c.builtin=="class_start"||c.builtin=="class_stop"||c.builtin=="class_reset"||c.builtin=="class_restart"){
      if(c.args.size()!=1)return false;for(auto& kv:s_)if(kv.second.classes.count(c.args[0])){if(c.builtin=="class_start"&&!kv.second.disabled&&!start(kv.second))return false;if(c.builtin=="class_stop"&&!stop(kv.second))return false;if(c.builtin=="class_reset")kv.second.state=Service::stopped;if(c.builtin=="class_restart"&&!stop(kv.second))return false;}return true;
    }
    if(c.builtin=="restart"){if(c.args.empty())return false;std::string n=c.args.back();if(!s_.count(n))return false;Service& s=s_[n];if(c.args.size()==1||s.state==Service::running){if(!stop(s))return false;for(const auto& r:s.onrestart)if(!run(r,a))return false;return start(s);}return true;}
    if(c.builtin=="exec_start"){if(c.args.size()!=1||!s_.count(c.args[0]))return false;return exec_service(s_[c.args[0]],true);}
    if(c.builtin=="exec_background")return exec_background(c.args);
    if(c.builtin=="exec")return exec_foreground(c.args);
    if(c.builtin=="setrlimit"){if(c.args.size()!=3)return false;int resource=-1;if(c.args[0]=="nice")resource=RLIMIT_NICE;else if(c.args[0]=="nofile")resource=RLIMIT_NOFILE;else if(c.args[0]=="memlock"||c.args[0]=="8")resource=RLIMIT_MEMLOCK;else resource=strtol(c.args[0].c_str(),nullptr,10);rlimit lim{(rlim_t)strtoll(c.args[1].c_str(),nullptr,10),(rlim_t)strtoll(c.args[2].c_str(),nullptr,10)};return resource>=0&&setrlimit(resource,&lim)==0;}
    if(c.builtin=="mount"){if(c.args.size()<3)return false;unsigned long flags=0;const char* data=nullptr;for(size_t i=3;i<c.args.size();++i){if(c.args[i]=="ro")flags|=MS_RDONLY;else if(c.args[i]=="rw"||c.args[i]=="wait")continue;else if(c.args[i]=="bind")flags|=MS_BIND;else if(c.args[i]=="rec")flags|=MS_REC;else if(c.args[i]=="remount")flags|=MS_REMOUNT;else data=c.args[i].c_str();}if(::mount(c.args[1].c_str(),(root_+c.args[2]).c_str(),c.args[0].c_str(),flags,data)==0)return true;int e=errno;return syscall_failure("mount",c.args,e);}
    if(c.builtin=="umount"){if(c.args.size()!=1)return false;if(::umount((root_+c.args[0]).c_str())==0)return true;int e=errno;return syscall_failure("umount",c.args,e);}
    if(c.builtin=="copy"){if(c.args.size()!=2)return false;std::ifstream in(root_+c.args[0],std::ios::binary);if(!in){int e=errno?errno:ENOENT;return syscall_failure("copy",c.args,e);}std::ofstream out(root_+c.args[1],std::ios::binary);if(!out){int e=errno?errno:EIO;return syscall_failure("copy",c.args,e);}out<<in.rdbuf();if(!out){int e=errno?errno:EIO;return syscall_failure("copy",c.args,e);}return true;}
    if(c.builtin=="copy_per_line"){if(c.args.size()!=2)return false;std::ifstream in(root_+c.args[0]);if(!in){int e=errno?errno:ENOENT;return syscall_failure("copy_per_line",c.args,e);}std::ofstream out(root_+c.args[1],std::ios::app);if(!out){int e=errno?errno:EIO;return syscall_failure("copy_per_line",c.args,e);}std::string line;while(std::getline(in,line))out<<line<<'\n';if(!out){int e=errno?errno:EIO;return syscall_failure("copy_per_line",c.args,e);}return true;}
    if(c.builtin=="rm"){return c.args.size()==1&&unlink((root_+c.args[0]).c_str())==0;}
    if(c.builtin=="rmdir"){return c.args.size()==1&&rmdir((root_+c.args[0]).c_str())==0;}
    if(c.builtin=="wait"){if(c.args.empty())return false;uint64_t ws=trace_?trace_->wall():0;if(trace_)trace_->emit("wait_begin",{{"type",Trace::s("path")},{"target",Trace::s(c.args[0])},{"timeout_ms",Trace::i(5000)},{"start_ns",Trace::n(ws)}});bool ok=false;for(int i=0;i<50;++i){if(access((root_+c.args[0]).c_str(),F_OK)==0){ok=true;break;}usleep(100000);}if(trace_)trace_->emit("wait_end",{{"type",Trace::s("path")},{"target",Trace::s(c.args[0])},{"end_ns",Trace::n(trace_->wall())},{"elapsed_us",Trace::n((trace_->wall()-ws)/1000)},{"result",Trace::b(ok)}});return ok;}
    if(c.builtin=="wait_for_prop"){if(c.args.size()!=2)return false;uint64_t ws=trace_?trace_->wall():0;if(trace_)trace_->emit("wait_begin",{{"type",Trace::s("property")},{"target",Trace::s(c.args[0])},{"expected",Trace::s(c.args[1])},{"timeout_ms",Trace::i(c.unbounded_wait?-1:5000)},{"start_ns",Trace::n(ws)}});auto satisfied=[&](){std::string v;return p_.get(c.args[0],&v)&&v==c.args[1];};bool ok=false;if(c.unbounded_wait){for(;;){if(satisfied()){ok=true;break;}usleep(100000);}}else for(int i=0;i<50;++i){if(satisfied()){ok=true;break;}usleep(100000);}if(trace_)trace_->emit("wait_end",{{"type",Trace::s("property")},{"target",Trace::s(c.args[0])},{"end_ns",Trace::n(trace_->wall())},{"elapsed_us",Trace::n((trace_->wall()-ws)/1000)},{"result",Trace::b(ok)}});return ok;}
    if(c.builtin=="sysclktz"){
      if(c.args.size()!=1)return false;
      char* end=nullptr; errno=0; long minutes=std::strtol(c.args[0].c_str(),&end,10);
      if(errno!=0||end==c.args[0].c_str()||std::isspace((unsigned char)c.args[0][0])||*end!='\0'||minutes<INT_MIN||minutes>INT_MAX){
        log("ERROR","sysclktz args="+c.args[0]+" parse failure"); return false;
      }
      struct timezone tz={}; tz.tz_minuteswest=(int)minutes;
      if(::settimeofday(nullptr,&tz)==-1){int e=errno;return syscall_failure("sysclktz",c.args,e);}
      return true;
    }
    if(c.builtin=="interface_start"||c.builtin=="interface_stop"||c.builtin=="interface_restart") { log("INFO",c.builtin+" dispatched"); return need(1); }
    if(c.builtin=="bootchart"||c.builtin=="domainname"||c.builtin=="enter_default_mount_ns"||c.builtin=="ifup"||c.builtin=="init_user0"||c.builtin=="installkey"||c.builtin=="load_persist_props"||c.builtin=="load_system_props"||c.builtin=="loglevel"||c.builtin=="mark_post_data"||c.builtin=="mount_all"||c.builtin=="perform_apex_config"||c.builtin=="powerctl"||c.builtin=="remount_userdata"||c.builtin=="swapon_all"||c.builtin=="umount_all"||c.builtin=="update_linker_config"||c.builtin=="verity_update_state"||c.builtin=="hostname"||c.builtin=="switch_root") {log("ERROR","host-owned or adapter builtin is not executable by subordinate graph: "+c.builtin);return false;}
    return false;
  }
  bool start(Service& s){
    apply_deferred_metadata();
    if(s.state==Service::running||s.state==Service::starting)return true;
    if(s.argv.empty())return false;
    // Android init expands properties in service arguments when the service
    // is started.  Keep the service declaration immutable, but pass the
    // expanded argv to the child.  This is required by stock services such
    // as insmod_sh, whose config path is /vendor/etc/init.insmod.${ro.hardware}.cfg.
    std::vector<std::string> expanded_argv=s.argv;
    for(auto& x:expanded_argv)x=expand(x,p_);
    std::vector<int> fds;
    if(!s.sockets.empty()){
      std::string dir=root_+"/dev/socket";
      if(mkdir(dir.c_str(),0755)!=0&&errno!=EEXIST){log("ERROR","cannot prepare Android socket directory "+dir);return false;}
    }
    for(const auto& q:s.sockets){
      std::string path=root_+"/dev/socket/"+q.name;
      if(access(path.c_str(),F_OK)==0){
        int probe=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0); sockaddr_un a{}; a.sun_family=AF_UNIX; strncpy(a.sun_path,path.c_str(),sizeof(a.sun_path)-1);
        bool live=probe>=0 && connect(probe,(sockaddr*)&a,sizeof(a))==0; if(probe>=0)close(probe);
        if(live){log("ERROR","refusing to replace live service socket "+path);return false;}
        if(unlink(path.c_str())!=0&&errno!=ENOENT){log("ERROR","cannot remove stale service socket "+path);return false;}
      }
      int typ=q.type=="stream"?SOCK_STREAM:q.type=="dgram"?SOCK_DGRAM:q.type=="seqpacket"?SOCK_SEQPACKET:-1;
      if(typ<0){log("ERROR","unsupported socket type for "+q.name);return false;}
      int fd=socket(AF_UNIX,typ|SOCK_CLOEXEC,0);if(fd<0)return false;
      sockaddr_un a{};a.sun_family=AF_UNIX;strncpy(a.sun_path,path.c_str(),sizeof(a.sun_path)-1);
      if(bind(fd,(sockaddr*)&a,sizeof(a))!=0||chown(path.c_str(),q.uid,q.gid)!=0||chmod(path.c_str(),(mode_t)q.mode)!=0||(typ==SOCK_STREAM&&listen(fd,1<<20)!=0)){close(fd);unlink(path.c_str());return false;}
      fds.push_back(fd);
    }
  std::vector<char*> av;for(auto& x:expanded_argv)av.push_back(const_cast<char*>(x.c_str()));av.push_back(nullptr);
    int exec_pipe[2];if(!make_exec_pipe(exec_pipe)){int e=errno;for(const auto& q:s.sockets)unlink((root_+"/dev/socket/"+q.name).c_str());log("ERROR","service pipe failed executable="+s.exe+" errno="+std::to_string(e)+" strerror="+std::strerror(e));return false;}
    pid_t p=fork();
    if(p==0){
      close(exec_pipe[0]);
      if(root_.size())chdir(root_.c_str());
      if(!s.cwd.empty()&&chdir(s.cwd.c_str())!=0)_exit(127);
      for(size_t i=0;i<s.sockets.size();++i){std::string env="ANDROID_SOCKET_"+s.sockets[i].name;setenv(env.c_str(),std::to_string(fds[i]).c_str(),1);int fl=fcntl(fds[i],F_GETFD);fcntl(fds[i],F_SETFD,fl&~FD_CLOEXEC);}
      // Match init's SetProcessAttributes ordering: primary GID, then the
      // remaining declared groups, then UID, and only then exec.
      if(s.has_group&&setgid(s.gid)!=0)_exit(126);if(s.has_group&&setgroups(s.supp_gids.size(),s.supp_gids.data())!=0)_exit(126);if(s.has_user&&setuid(s.uid)!=0)_exit(126);
      execv(s.exe.c_str(),av.data()); child_exec_failure(exec_pipe[1],errno);
    }
    if(p<0){int e=errno;close(exec_pipe[0]);close(exec_pipe[1]);for(const auto& q:s.sockets)unlink((root_+"/dev/socket/"+q.name).c_str());log("ERROR","service fork failed executable="+s.exe+" errno="+std::to_string(e)+" strerror="+std::strerror(e));s.state=Service::failed;return false;}
    close(exec_pipe[1]);
    for(int fd:fds)close(fd);
    s.pid=p;s.exec_fd=exec_pipe[0];s.spawn_ns=trace_?trace_->wall():0;s.state=Service::running;if(trace_)trace_->emit("service_start_requested",{{"service",Trace::s(s.name)},{"executable",Trace::s(s.exe)},{"pid",Trace::i(p)},{"spawn_ns",Trace::n(s.spawn_ns)},{"argv",Trace::s(join_args(expanded_argv))}});log("INFO","start "+s.name+" pid="+std::to_string(p));return true;
  }
  bool stop(Service& s){if(s.state!=Service::running)return true;return kill(s.pid,SIGTERM)==0;}
  std::vector<Action>a_;std::map<std::string,Service>s_;Rediska p_;bool failed_=false;std::string root_;std::deque<QueuedAction>q_;std::deque<std::string>events_;uint64_t queue_seq_=0;Trace* trace_=nullptr;
};

static void usage(const char* argv0){std::fprintf(stderr,"usage: %s --validate [--graph PATH]\n       %s --run --event EVENT [--event EVENT ...] [--graph PATH]\n",argv0,argv0);}

static bool option_value(int argc, char** argv, int* index, std::string* value) {
  if (*index + 1 >= argc) return false;
  std::string candidate = argv[++*index];
  if (candidate.empty() || candidate.rfind("--", 0) == 0) return false;
  *value = std::move(candidate);
  return true;
}

int main(int argc,char**argv){const char* env_sock=std::getenv("TB330FU_REDISKA_SOCKET");const char* env_graph=std::getenv("TB330FU_GRAPH");const char* env_trace=std::getenv("TB330FU_TRACE_PATH");std::string graph=env_graph&&*env_graph?env_graph:kInstalledGraph,sock=env_sock&&*env_sock?env_sock:REDISKA_SOCKET_DEFAULT,trace_path=env_trace&&*env_trace?env_trace:"/run/tb330fu-graph-executor.jsonl";Trace trace(trace_path.c_str());if(trace.enabled())trace.emit("process_start",{{"pid",Trace::i(getpid())},{"start_ns",Trace::n(trace.wall())},{"start_cpu_ns",Trace::n(trace.cpu())},{"trace_path",Trace::s(trace_path)}});std::vector<std::string> events;bool validate=false,run=false;for(int i=1;i<argc;i++){std::string x=argv[i],value;if(x=="--validate")validate=true;else if(x=="--run")run=true;else if(x=="--graph"&&option_value(argc,argv,&i,&value))graph=std::move(value);else if(x=="--event"&&option_value(argc,argv,&i,&value))events.push_back(std::move(value));else{usage(argv[0]);return 2;}}if(validate==run||(!validate&&events.empty())){usage(argv[0]);return 2;}
 if(!install_child_signal_handler()){log("ERROR","cannot install SIGCHLD handler: "+std::string(std::strerror(errno)));return 1;} if(trace.enabled())trace.emit("graph_load_begin",{{"path",Trace::s(graph)},{"time_ns",Trace::n(trace.wall())}});log("INFO","opening graph "+graph);std::vector<Action>a;std::map<std::string,Service>s;std::set<std::string>w;Json::Value g;if(!load_graph(graph,&a,&s,&w,&g))return 1;if(trace.enabled())trace.emit("graph_load_end",{{"path",Trace::s(graph)},{"time_ns",Trace::n(trace.wall())}});log("INFO","validated actions="+std::to_string(a.size())+" commands="+std::to_string([&]{size_t n=0;for(auto&x:a)n+=x.cmds.size();return n;}())+" services="+std::to_string(g["services"].size())+" properties-watched="+std::to_string(w.size())+" predicates="+std::to_string(g["property_predicates"].size()));if(validate)return 0;if(trace.enabled())trace.emit("runtime_initialization",{{"time_ns",Trace::n(trace.wall())}});Rediska properties(sock);Runtime rt(std::move(a),std::move(s),properties,"",&trace);if(trace.enabled())trace.emit("execution_begin",{{"time_ns",Trace::n(trace.wall())}});for(const auto& e:events){log("INFO","dispatching event "+e);rt.event(e);if(!rt.ok())return 1;}if(g.isMember("startup_services")){for(const auto& n:g["startup_services"]){if(!rt.start_named(n.asString()))return 1;}}uint32_t property_serial=0;for(;;){if(properties.global_serial(&property_serial))break;if(child_signal_pending){child_signal_pending=0;rt.reap();continue;}log("ERROR","cannot establish REDISKA global property serial");return 1;}if(!rt.observe(w))return 1;for(;;){if(child_signal_pending){child_signal_pending=0;rt.reap();}uint32_t next_serial=property_serial;if(!properties.wait_any(&next_serial)){if(child_signal_pending){child_signal_pending=0;rt.reap();continue;}log("ERROR","REDISKA WAIT_ANY failed while executor was idle");return 1;}property_serial=next_serial;rt.reap();if(!rt.observe(w))return 1;}}
