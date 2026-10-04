#pragma once
#include "core.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#ifdef __PS5__
#include <ps5/kernel.h>
#include <dlfcn.h>
#include <cerrno>
#endif

namespace botty {
// The request ABI and ~60-second ShellCore lease were verified on FW 13.00.
// Enable the user-requested portal range; surface runtime rejection separately.
inline bool restModeFirmwareSupported(uint32_t firmware) {
  const auto version=firmware&0xffff0000U;
  return version>=0x07000000U&&version<=0x13600000U;
}
class RestModeLease {
  bool supported_, stopped_=false, failed_=false;
  int result_=0;
  int64_t accepted_=-1, next_=0;
public:
  static constexpr int64_t renewalMs=10000, expiryMs=60000;
  explicit RestModeLease(bool supported):supported_(supported) {}
  bool due(int64_t now) const {return supported_&&!stopped_&&!failed_&&now>=next_;}
  void record(int64_t now,int result) {
    result_=result;failed_=result!=0;next_=now+renewalMs;
    if(!failed_)accepted_=now;
  }
  void stop() {stopped_=true;}
  json state(int64_t now) const {
    const bool active=supported_&&!stopped_&&!failed_&&accepted_>=0&&now>=accepted_&&now-accepted_<expiryMs;
    const char* status=!supported_?"unsupported":stopped_?"stopped":failed_?"failed":accepted_<0?"starting":active?"active":"expired";
    return {{"supported",supported_},{"active",active},{"status",status},
            {"lastResult",static_cast<uint32_t>(result_)},{"renewalSeconds",renewalMs/1000},{"leaseSeconds",expiryMs/1000}};
  }
};
class RestModeKeeper {
  using Clock=std::chrono::steady_clock;
  std::mutex mutex_;
  std::condition_variable wake_;
  RestModeLease lease_;
  std::function<int()> request_;
  std::thread worker_;
  bool stopping_=false;
  static int64_t now() {return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();}
  void renew() {
    {std::lock_guard<std::mutex> guard(mutex_);if(!lease_.due(now()))return;}
    // Keep IPC out of the status mutex; a delayed call cannot block API reads.
    const int result=request_();
    std::lock_guard<std::mutex> guard(mutex_);
    lease_.record(now(),result);
    if(result)std::fprintf(stderr,"Botty rest-mode request failed: 0x%08x; renewals disabled\n",static_cast<unsigned>(result));
  }
public:
  RestModeKeeper(bool supported,std::function<int()> request):lease_(supported),request_(std::move(request)) {}
  ~RestModeKeeper() {stop();}
  void start() {
    {std::lock_guard<std::mutex> guard(mutex_);if(worker_.joinable()||stopping_)return;}
    // Establish the first lease before HTTP readiness or background jobs.
    renew();
    {std::lock_guard<std::mutex> guard(mutex_);if(!lease_.state(now()).at("active").get<bool>()||stopping_)return;}
    std::fprintf(stderr,"Botty rest-mode maintenance active (renewal every 10 seconds)\n");
    worker_=std::thread([this]{
      std::unique_lock<std::mutex> guard(mutex_);
      while(!wake_.wait_for(guard,std::chrono::milliseconds(RestModeLease::renewalMs),[this]{return stopping_;})) {
        guard.unlock();renew();guard.lock();
        if(lease_.state(now()).at("status")=="failed")break;
      }
    });
  }
  void stop() {
    {std::lock_guard<std::mutex> guard(mutex_);stopping_=true;lease_.stop();}
    wake_.notify_all();if(worker_.joinable())worker_.join();
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
