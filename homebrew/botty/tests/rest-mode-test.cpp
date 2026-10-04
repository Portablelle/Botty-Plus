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
  assert(lease.state(130000).at("lastResult")==0x81130001U&&!lease.due(200000));
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
  std::puts("Rest-mode lease expiry, firmware guard, rejection and orderly stop passed");
}
