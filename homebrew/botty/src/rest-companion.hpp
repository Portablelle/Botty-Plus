#pragma once
#include "core.hpp"
#include "rest-mode.hpp"
#include "rest-companion-protocol.h"
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#ifdef __PS5__
#include <ps5/kernel.h>
#include <sys/sysctl.h>
#endif
namespace botty {
inline int64_t restCompanionMonotonicSeconds() {
  timespec value{};if(clock_gettime(CLOCK_MONOTONIC,&value)||value.tv_sec<0)throw std::runtime_error("Cannot read companion clock");return value.tv_sec;
}
inline json restCompanionStatus(const fs::path& root,int64_t now,const json& boot) {
  try {
    auto state=json::parse(readText(root/"rest-mode-companion.json",4096));
    const auto at=state.at("monotonicSeconds").get<int64_t>();
    if(state.at("schema")!=1||state.at("firmware")!=BOTTY_REST_FW||state.at("boot")!=boot||at<0||now<at||now-at>20)return {{"status","stale"},{"live",false}};
    state["live"]=true;state["ageSeconds"]=now-at;return state;
  }catch(const std::exception&){return {{"status","unavailable"},{"live",false}};}
}
inline void restCompanionControl(const fs::path& root,const json& state) {
#ifdef __PS5__
  // This does not inject or launch anything. Activation is a separate experiment.
  if((kernel_get_fw_version()&0xffff0000U)!=BOTTY_REST_FW)return;
  bool enabled=false;
  // Shutdown revokes without consulting a marker that may be unavailable.
  if(state.at("status")!="stopped"){
    int marker=open((root/"rest-mode-companion.enabled").c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(marker>=0){
      struct stat info{};char bytes[18];
      if(!fstat(marker,&info)&&S_ISREG(info.st_mode)&&info.st_size==18){
        const auto count=read(marker,bytes,sizeof(bytes));
        enabled=count==18&&!std::memcmp(bytes,"experimental-fw13\n",18);
      }
      close(marker);
    }
  }
  timeval boot{};size_t length=sizeof(boot);
  if(sysctlbyname("kern.boottime",&boot,&length,nullptr,0)||length!=sizeof(boot)||boot.tv_sec<=0||boot.tv_usec<0||boot.tv_usec>999999)throw std::runtime_error("Cannot establish companion boot identity");
  botty_rest_control control{};control.magic=BOTTY_REST_MAGIC;control.version=1;
  control.firmware=BOTTY_REST_FW;control.pid=getpid();control.boot_seconds=boot.tv_sec;control.boot_microseconds=boot.tv_usec;
  control.sequence=state.at("attempts").get<uint64_t>();
  const auto now=restCompanionMonotonicSeconds();
  control.expires=enabled&&state.at("status")!="stopped"?uint64_t(now)+BOTTY_REST_TTL:0;
  const auto target=root/"rest-mode-control.bin";
  auto pattern=(root/"rest-mode-control.XXXXXX").string();
  std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
  int fd=mkstemp(temporary.data());
  const fs::path tmp=temporary.data();
  if(fd<0)throw std::runtime_error("Cannot write companion control");
  size_t offset=0;bool failed=false;
  while(offset<sizeof(control)){
    auto n=write(fd,reinterpret_cast<const char*>(&control)+offset,sizeof(control)-offset);
    if(n<0&&errno==EINTR)continue;if(n<=0){failed=true;break;}offset+=size_t(n);
  }
  if(close(fd))failed=true;
  if(failed){unlink(tmp.c_str());throw std::runtime_error("Companion control write failed");}
  try{fs::rename(tmp,target);}catch(...){unlink(tmp.c_str());throw;}
#else
  (void)root;(void)state;
#endif
}
inline json currentRestCompanionStatus(const fs::path& root) {
#ifdef __PS5__
  if((kernel_get_fw_version()&0xffff0000U)!=BOTTY_REST_FW)return {{"status","unsupported"},{"live",false}};
  timeval boot{};size_t length=sizeof(boot);
  if(sysctlbyname("kern.boottime",&boot,&length,nullptr,0)||length!=sizeof(boot))return {{"status","unavailable"},{"live",false}};
  auto state=restCompanionStatus(root,restCompanionMonotonicSeconds(),{{"seconds",boot.tv_sec},{"microseconds",boot.tv_usec}});
  if(state.value("status","")=="active"&&state.value("ownerPid",int64_t(0))!=getpid())state["status"]="waiting-for-manager";
  return state;
#else
  (void)root;return {{"status","unsupported"},{"live",false}};
#endif
}
}

namespace botty {
// Operational authorization has its own timer and owned callback data. It
// continues even when Sony IPC is unavailable or diagnostic storage stalls.
class RestCompanionAuthorization {
  RestModeObserver worker_;
public:
  explicit RestCompanionAuthorization(fs::path root) {
    worker_.start([root=std::move(root),sequence=uint64_t(0)](const json& state) mutable {
      auto control=state;control["attempts"]=++sequence;restCompanionControl(root,control);
    },10);
    worker_.publish({{"status","running"}});
  }
  ~RestCompanionAuthorization(){worker_.publish({{"status","stopped"}});worker_.stop();}
};
class RestModeDiagnostics {
  fs::path root_;
  std::mutex mutex_;
  json companion_={{"status","unavailable"},{"live",false}},previous_;
  int64_t nextSnapshot_=0;
public:
  explicit RestModeDiagnostics(fs::path root):root_(std::move(root)){}
  void observe(const json& state) {
    // This worker is separate from lease renewal. No disk reads in API status.
    auto companion=currentRestCompanionStatus(root_);
    {std::lock_guard<std::mutex> guard(mutex_);companion_=companion;}
    const auto now=restCompanionMonotonicSeconds();
    if(now>=nextSnapshot_||previous_.empty()||previous_.at("lastResult")!=state.at("lastResult")||previous_.at("renewalGaps")!=state.at("renewalGaps")||state.at("status")=="stopped") {
      auto snapshot=state;snapshot["pid"]=getpid();snapshot["companion"]=companion;
      writeJson(root_/"rest-mode.json",snapshot);nextSnapshot_=now+60;
    }
    previous_=state;
  }
  json companion() {
    std::lock_guard<std::mutex> guard(mutex_);auto state=companion_;
    if(state.value("live",false)){
      const auto at=state.at("monotonicSeconds").get<int64_t>(),now=restCompanionMonotonicSeconds();
      if(now<at||now-at>20){state["live"]=false;state["status"]="stale";}
      else state["ageSeconds"]=now-at;
    }
    return state;
  }
};
}
