#pragma once
#include <chrono>
#include <string>
#include <cstdint>
namespace botty {
// Live progress is cheap; durable checkpoints force storage synchronization.
// Start/phase transitions are immediate. Final job status is always persisted
// separately, including failures. Interrupted jobs are never auto-resumed.
class ProgressSchedule {
 public:
  using Clock=std::chrono::steady_clock;
  struct Decision {bool publish, checkpoint;};
  Decision update(Clock::time_point now,const std::string& phase) {
    const bool transition=!started || phase!=lastPhase;
    const bool checkpoint=transition || now-lastCheckpoint>=std::chrono::seconds(10);
    const bool publish=checkpoint || now-lastPublish>=std::chrono::milliseconds(250);
    if(publish){lastPublish=now;lastPhase=phase;started=true;}
    if(checkpoint)lastCheckpoint=now;
    return {publish,checkpoint};
  }
 private:
  bool started=false;
  Clock::time_point lastPublish{},lastCheckpoint{};
  std::string lastPhase;
};
class ExtractionEstimate {
 public:
  using Clock=std::chrono::steady_clock;
  double rate=0,eta=-1;
  void update(Clock::time_point now,uint64_t bytes,uint64_t total) {
    if(!started || bytes<lastBytes){started=true;sampled=false;start=last=now;lastBytes=bytes;rate=0;eta=-1;return;}
    const double seconds=std::chrono::duration<double>(now-last).count();
    if(seconds>=1){const double sample=double(bytes-lastBytes)/seconds;
      rate=sampled?rate+(sample-rate)*(seconds/(5+seconds)):sample;
      sampled=true;last=now;lastBytes=bytes;}
    eta=total>bytes && rate>0 && std::chrono::duration<double>(now-start).count()>=5 ? double(total-bytes)/rate : -1;
    if(total>0 && bytes>=total)eta=0;
  }
 private:
  bool started=false,sampled=false;
  Clock::time_point start{},last{};uint64_t lastBytes=0;
};

}
