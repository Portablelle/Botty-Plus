#pragma once
#include <chrono>
#include <string>
#include <cstdint>
#include <deque>
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
    if(!started || bytes<observedBytes || total!=lastTotal || now-observedAt>std::chrono::seconds(30)){
      started=true;sampled=false;last=lastProgress=observedAt=now;lastBytes=observedBytes=bytes;lastTotal=total;rate=0;eta=total>0&&bytes>=total?0:-1;
      history.clear();history.push_back({now,double(bytes)});return;
    }
    if(bytes>observedBytes)lastProgress=now;
    observedBytes=bytes;observedAt=now;
    const double seconds=std::chrono::duration<double>(now-last).count();
    if(seconds>=1){const double sample=double(bytes-lastBytes)/seconds;
      rate=sampled?rate+(sample-rate)*(seconds/(5+seconds)):sample;
      sampled=true;last=now;lastBytes=bytes;history.push_back({now,double(bytes)});
      const auto cutoff=now-std::chrono::seconds(60);
      while(history.size()>1&&history[1].time<=cutoff)history.pop_front();
      if(history.size()>1&&history.front().time<cutoff){
        auto& first=history.front();const auto& next=history[1];
        const double fraction=std::chrono::duration<double>(cutoff-first.time).count()/std::chrono::duration<double>(next.time-first.time).count();
        first.bytes+=(next.bytes-first.bytes)*fraction;first.time=cutoff;
      }
    }
    const double elapsed=std::chrono::duration<double>(now-history.front().time).count();
    const double progressed=double(bytes)-history.front().bytes;
    eta=total>bytes && progressed>0 && elapsed>=5 && now-lastProgress<std::chrono::seconds(15) ? double(total-bytes)*elapsed/progressed : -1;
    if(total>0 && bytes>=total)eta=0;
  }
 private:
  bool started=false,sampled=false;
  struct Sample {Clock::time_point time;double bytes;};
  std::deque<Sample> history;
  Clock::time_point last{},lastProgress{},observedAt{};uint64_t lastBytes=0,observedBytes=0,lastTotal=0;
};

}
