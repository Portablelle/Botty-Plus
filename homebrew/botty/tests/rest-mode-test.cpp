#include "rest-mode.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
using namespace botty;
int main() {
  assert(restModeFirmwareSupported(0x13000000));
  assert(restModeFirmwareSupported(0x13000001));
  for(auto firmware:{0x07000000U,0x07610000U,0x09000000U,0x11400000U,0x13010000U,0x13600000U})assert(restModeFirmwareSupported(firmware));
  for(auto firmware:{0U,0x06990000U,0x13610000U,0x14000000U})assert(!restModeFirmwareSupported(firmware));
  RestModeLease lease(true);
  assert(lease.due(0)&&!lease.state(0).at("active"));
  lease.record(0,0);
  assert(lease.state(0).at("active")&&!lease.due(9999)&&lease.due(10000));
  // Delayed scheduling must never advertise an expired request as active.
  assert(lease.state(59999).at("active"));
  assert(!lease.state(60000).at("active")&&lease.state(60000).at("status")=="expired");
  assert(!lease.state(-1).at("active"));
  lease.record(120000,0);
  assert(lease.state(120000).at("active"));
  lease.record(130000,static_cast<int>(0x81130001U));
  assert(!lease.state(130000).at("active")&&lease.state(130000).at("status")=="failed");
  assert(lease.state(130000).at("lastResult")==0x81130001U&&lease.state(130000).at("retrying"));
  assert(!lease.due(134999)&&lease.due(135000));
  lease.record(135000,0);
  assert(lease.state(135000).at("active")&&!lease.state(135000).at("retrying"));
  assert(lease.state(135000).at("failures")==1&&lease.state(135000).at("consecutiveFailures")==0);
  RestModeLease unavailable(true);
  unavailable.record(0,-ENOSYS);
  assert(!unavailable.due(200000)&&!unavailable.state(0).at("retrying"));
  // A paused monotonic clock must not hide the observed wall-clock gap.
  RestModeLease suspended(true);
  suspended.record(0,0,1000000);suspended.record(10000,0,8200000);
  assert(suspended.state(10000).at("renewalGaps")==1);
  assert(suspended.state(10000).at("maxRenewalGapMs")==7200000);
  suspended.record(20000,0,8190000); // Wall-clock corrections are not negative gaps.
  assert(suspended.state(20000).at("renewalGaps")==1);
  RestModeLease durable(true);
  for(int64_t time=0;time<24*60*60*1000;time+=10000){
    assert(durable.due(time));durable.record(time,0);assert(durable.state(time+9999).at("active"));
  }
  durable.stop();assert(!durable.due(24*60*60*1000)&&!durable.state(24*60*60*1000).at("active"));
  std::atomic<int> calls{0};
  {
    RestModeKeeper unsupported(false,[&]{++calls;return 0;});
    unsupported.start();assert(calls==0&&unsupported.state().at("status")=="unsupported");
  }
  {
    RestModeKeeper failed(true,[&]{++calls;return -1;});
    failed.start();assert(calls==1&&!failed.state().at("active"));
    failed.stop();assert(failed.state().at("status")=="stopped");
  }
  {
    RestModeKeeper active(true,[&]{++calls;return 0;});
    active.start();assert(calls==2&&active.state().at("active"));
    active.start();assert(calls==2);
    const auto start=std::chrono::steady_clock::now();active.stop();
    assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
    assert(active.state().at("status")=="stopped"&&!active.state().at("active"));
  }
  {
    std::atomic<int> retries{0},snapshots{0};
    RestModeKeeper recovering(true,[&]{return ++retries==1?-EAGAIN:0;},[&](const json&){
      ++snapshots;throw std::runtime_error("simulated snapshot failure");
    });
    recovering.start();assert(recovering.state().at("retrying"));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(7);
    while(!recovering.state().at("active").get<bool>()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(recovering.state().at("active")&&retries==2);
    recovering.stop();assert(snapshots>=2);
  }
  {
    std::mutex mutex;std::condition_variable wake;bool entered=false,released=false;
    std::atomic<int> retries{0};
    RestModeKeeper independent(true,[&]{return ++retries==1?-EAGAIN:0;},[&](const json&){
      std::unique_lock<std::mutex> guard(mutex);entered=true;wake.notify_all();
      wake.wait(guard,[&]{return released;});
    });
    independent.start();
    {std::unique_lock<std::mutex> guard(mutex);assert(wake.wait_for(guard,std::chrono::seconds(1),[&]{return entered;}));}
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(7);
    while(!independent.state().at("active").get<bool>()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(independent.state().at("active")&&retries==2);
    {std::lock_guard<std::mutex> guard(mutex);released=true;}wake.notify_all();independent.stop();
  }
  {
    struct Blocked {std::mutex mutex;std::condition_variable wake;bool entered=false,released=false,done=false;};
    auto blocked=std::make_shared<Blocked>();
    RestModeKeeper keeper(true,[]{return 0;},[blocked](const json& state){
      std::unique_lock<std::mutex> guard(blocked->mutex);blocked->entered=true;blocked->wake.notify_all();
      blocked->wake.wait(guard,[&]{return blocked->released;});
      if(state.at("status")=="stopped"){blocked->done=true;blocked->wake.notify_all();}
    });
    keeper.start();
    {std::unique_lock<std::mutex> guard(blocked->mutex);assert(blocked->wake.wait_for(guard,std::chrono::seconds(1),[&]{return blocked->entered;}));}
    const auto begin=std::chrono::steady_clock::now();keeper.stop();
    assert(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1));
    {std::unique_lock<std::mutex> guard(blocked->mutex);blocked->released=true;blocked->wake.notify_all();assert(blocked->wake.wait_for(guard,std::chrono::seconds(1),[&]{return blocked->done;}));}
  }
  {
    std::atomic<int> ticks{0};RestModeObserver periodic;
    periodic.start([&](const json&){++ticks;},1);periodic.publish({{"status","running"}});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(ticks<2&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    periodic.stop();assert(ticks>=2);
  }
  std::puts("Rest-mode expiry, retries, suspend gaps, snapshot errors and shutdown passed");
}
