#pragma once
#include "core.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <algorithm>
#include <cerrno>
#include <mutex>
#include <thread>
#include <memory>
#ifdef __PS5__
#include <ps5/kernel.h>
#include <dlfcn.h>
#endif

namespace botty {
// The request ABI and ~60-second ShellCore lease were verified on FW 13.00.
// Enable the user-requested portal range; surface runtime rejection separately.
inline bool restModeFirmwareSupported(uint32_t firmware) {
  const auto version=firmware&0xffff0000U;
  return version>=0x07000000U&&version<=0x13600000U;
}
class RestModeLease {
  bool supported_, stopped_=false, failed_=false, unavailable_=false;
  int result_=0;
  int64_t accepted_=-1, next_=0;
  int64_t attempted_=-1, wallAttempted_=-1, maxGap_=0;
  uint64_t attempts_=0, failures_=0, consecutiveFailures_=0, gaps_=0;
public:
  static constexpr int64_t renewalMs=10000, retryMs=5000, expiryMs=60000;
  explicit RestModeLease(bool supported):supported_(supported) {}
  bool due(int64_t now) const {return supported_&&!stopped_&&!unavailable_&&now>=next_;}
  bool running() const {return supported_&&!stopped_&&!unavailable_;}
  int64_t delayMs() const {return failed_?retryMs:renewalMs;}
  void record(int64_t now,int result,int64_t wallNow=-1) {
    // Wall time also exposes gaps if the monotonic clock pauses in suspend.
    if(attempted_>=0){
      auto gap=std::max<int64_t>(0,now-attempted_);
      if(wallAttempted_>=0&&wallNow>=wallAttempted_)gap=std::max(gap,wallNow-wallAttempted_);
      maxGap_=std::max(maxGap_,gap);if(gap>=expiryMs)++gaps_;
    }
    attempted_=now;wallAttempted_=wallNow;++attempts_;
    result_=result;failed_=result!=0;unavailable_=result==-ENOSYS;next_=now+delayMs();
    if(failed_){++failures_;++consecutiveFailures_;}else{accepted_=now;consecutiveFailures_=0;}
  }
  void stop() {stopped_=true;}
  json state(int64_t now) const {
    const bool active=supported_&&!stopped_&&!failed_&&accepted_>=0&&now>=accepted_&&now-accepted_<expiryMs;
    const char* status=!supported_?"unsupported":stopped_?"stopped":failed_?"failed":accepted_<0?"starting":active?"active":"expired";
    return {{"supported",supported_},{"active",active},{"status",status},
            {"lastResult",static_cast<uint32_t>(result_)},{"renewalSeconds",renewalMs/1000},{"leaseSeconds",expiryMs/1000},
            {"retrySeconds",retryMs/1000},{"retrying",running()&&failed_},
            {"attempts",attempts_},{"failures",failures_},{"consecutiveFailures",consecutiveFailures_},
            {"lastSuccessAgeMs",accepted_<0?-1:std::max<int64_t>(0,now-accepted_)},
            {"lastAttemptUnixMs",wallAttempted_},{"maxRenewalGapMs",maxGap_},{"renewalGaps",gaps_},
            {"strategy","manager-lease"},{"experimental",true}};
  }
};
// Callback state is owned by the worker. A stalled filesystem callback may
// outlive shutdown, but cannot access a destroyed keeper or stack reference.
class RestModeObserver {
  struct Shared {
    std::mutex mutex;
    std::condition_variable wake;
    json pending;
    bool ready=false, stopping=false, done=false;
  };
  std::shared_ptr<Shared> shared_=std::make_shared<Shared>();
  std::thread thread_;
public:
  void start(std::function<void(const json&)> callback,int periodicSeconds=0) {
    auto shared=shared_;
    thread_=std::thread([shared,callback=std::move(callback),periodicSeconds]{
      std::unique_lock<std::mutex> guard(shared->mutex);
      json latest;
      for(;;){
        if(periodicSeconds)shared->wake.wait_for(guard,std::chrono::seconds(periodicSeconds),[&]{return shared->stopping||shared->ready;});
        else shared->wake.wait(guard,[&]{return shared->stopping||shared->ready;});
        if(shared->ready){latest=std::move(shared->pending);shared->ready=false;}
        if(!latest.empty()){
          auto snapshot=latest;guard.unlock();
          try{callback(snapshot);}catch(const std::exception& error){std::fprintf(stderr,"Botty rest-mode observer failed: %s\n",error.what());}
          guard.lock();
        }
        if(shared->stopping&&!shared->ready)break;
      }
      shared->done=true;shared->wake.notify_all();
    });
  }
  void publish(json state) {
    std::lock_guard<std::mutex> guard(shared_->mutex);
    if(shared_->stopping)return;
    shared_->pending=std::move(state);shared_->ready=true;shared_->wake.notify_all();
  }
  void stop() {
    if(!thread_.joinable())return;
    std::unique_lock<std::mutex> guard(shared_->mutex);shared_->stopping=true;shared_->wake.notify_all();
    const bool done=shared_->wake.wait_for(guard,std::chrono::milliseconds(250),[&]{return shared_->done;});
    guard.unlock();if(done)thread_.join();else thread_.detach();
  }
  ~RestModeObserver(){stop();}
};
class RestModeKeeper {
  using Clock=std::chrono::steady_clock;
  std::mutex mutex_;
  std::condition_variable wake_;
  RestModeLease lease_;
  std::function<int()> request_;
  std::function<void(const json&)> observe_;
  std::thread worker_;
  RestModeObserver observer_;
  bool started_=false;
  bool stopping_=false;
  static int64_t now() {return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();}
  void renew() {
    {std::lock_guard<std::mutex> guard(mutex_);if(!lease_.due(now()))return;}
    // Keep IPC out of the status mutex; a delayed call cannot block API reads.
    const auto started=now();
    const auto wall=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int result=request_();
    json snapshot;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      // Timestamp conservatively before IPC, not after a potentially slow call.
      lease_.record(started,result,wall);snapshot=lease_.state(now());
    }
    // Coalesce snapshots: even blocked storage cannot hold up lease renewal.
    observer_.publish(std::move(snapshot));
  }
public:
  RestModeKeeper(bool supported,std::function<int()> request,std::function<void(const json&)> observe={}):lease_(supported),request_(std::move(request)),observe_(std::move(observe)) {}
  ~RestModeKeeper() {stop();}
  void start() {
    {std::lock_guard<std::mutex> guard(mutex_);if(started_||stopping_)return;started_=true;}
    observer_.start([observe=observe_,previous=json{}](const json& snapshot) mutable {
      const auto result=snapshot.at("lastResult").get<uint32_t>();
      const bool changed=previous.empty()||previous.at("lastResult")!=snapshot.at("lastResult");
      if(snapshot.at("status")!="stopped"){
        if(result&&changed)std::fprintf(stderr,"Botty rest-mode request failed: 0x%08x; %s\n",result,snapshot.at("retrying").get<bool>()?"retrying in 5 seconds":"API unavailable");
        if(!result&&changed&&!previous.empty())std::fprintf(stderr,"Botty rest-mode renewals recovered\n");
        if(!previous.empty()&&previous.at("renewalGaps")!=snapshot.at("renewalGaps"))std::fprintf(stderr,"Botty rest-mode renewal gap exceeded lease lifetime\n");
      }
      if(observe)observe(snapshot);
      previous=snapshot;
    },10);
    // Publish unsupported status too, replacing diagnostics from a prior run.
    observer_.publish(state());
    // Establish the first lease before HTTP readiness or background jobs.
    renew();
    {std::lock_guard<std::mutex> guard(mutex_);if(!lease_.running()||stopping_)return;}
    worker_=std::thread([this]{
      std::unique_lock<std::mutex> guard(mutex_);
      while(!wake_.wait_for(guard,std::chrono::milliseconds(lease_.delayMs()),[this]{return stopping_;})) {
        guard.unlock();renew();guard.lock();
        if(!lease_.running())break;
      }
    });
  }
  void stop() {
    {std::lock_guard<std::mutex> guard(mutex_);if(stopping_)return;stopping_=true;lease_.stop();}
    wake_.notify_all();if(worker_.joinable())worker_.join();
    observer_.publish(state());observer_.stop();
  }
  json state() {std::lock_guard<std::mutex> guard(mutex_);return lease_.state(now());}
};
inline bool currentRestModeSupported() {
#ifdef __PS5__
  return restModeFirmwareSupported(kernel_get_fw_version());
#else
  return false;
#endif
}
inline int requestRestMode() {
#ifdef __PS5__
  // Optional runtime lookup keeps Botty usable if a firmware lacks the module/API.
  // Retain the module for the process lifetime, including every renewal.
  static void* module=dlopen("libSceSystemService.sprx",RTLD_NOW|RTLD_LOCAL);
  using Request=int (*)(const char*);
  static auto request=module?reinterpret_cast<Request>(dlsym(module,"sceSystemStateMgrRequestToKeepMainOnStandby")):nullptr;
  return request?request("BottyBackgroundServices"):-ENOSYS;
#else
  return -1;
#endif
}
}
