#include "remote-buffer.hpp"
#include "remote-ledger.hpp"
#include "remote-outcome.hpp"
#include <cassert>
#include <iostream>
#include <string>
using botty::RemoteWriteBuffer;
int main(){
  using botty::RemoteRangeLedger;using botty::RemoteBatchOutcome;using botty::RemoteTransferError;
  auto ledger=RemoteRangeLedger::fromJson(nlohmann::json::array(),100,100);
  assert(ledger.completed()==0);uint64_t cursor=0;
  auto gaps=ledger.gaps(cursor,2,30);assert(gaps.size()==2&&gaps[0].start==0&&gaps[0].end==30&&gaps[1].start==30&&gaps[1].end==60);
  ledger.commit({30,60});ledger.commit({0,30});ledger.commit({60,70});
  assert(ledger.completed()==70&&ledger.toJson()==nlohmann::json({{0,70}}));
  ledger.commit({80,100});cursor=0;gaps=ledger.gaps(cursor,4,8);
  assert(gaps.size()==2&&gaps[0].start==70&&gaps[0].end==78&&gaps[1].start==78&&gaps[1].end==80);
  auto roundtrip=RemoteRangeLedger::fromJson(ledger.toJson(),100,100);assert(roundtrip.completed()==90);
  for(const auto& bad:{nlohmann::json({{0,101}}),nlohmann::json({{0,40},{30,50}}),nlohmann::json({{-1,4}}),nlohmann::json({{0.5,4}})}){try{RemoteRangeLedger::fromJson(bad,100,100);assert(false);}catch(const std::runtime_error&){}}
  try{RemoteRangeLedger::fromJson(nlohmann::json({{0,20}}),100,10);assert(false);}catch(const std::runtime_error&){}
  try{ledger.commit({1,2});assert(false);}catch(const std::runtime_error&){}
  auto failure=[](long status){try{throw RemoteTransferError(status,"typed transport failure");}catch(...){return RemoteBatchOutcome::failure(std::current_exception());}};
  const auto busy=failure(429),fatal=failure(416),other=failure(500);
  assert(!busy.cancelSiblings()&&busy.busy()&&busy.connectionsAfterFailure(4)==2);
  assert(fatal.cancelSiblings()&&fatal.pause()&&!fatal.busy());
  for(bool reverse:{false,true}){
    RemoteBatchOutcome summary;summary.include(other);summary.include(reverse?fatal:busy);summary.include(reverse?busy:fatal);
    assert(summary.pause());try{summary.rethrow();assert(false);}catch(const RemoteTransferError& e){assert(e.status==416);}
  }

  constexpr uint64_t MiB=1024*1024;
  // Near the reserve, physical uncommitted ranges can be overwritten without
  // allocating their bytes twice. A full-sized sparse file still needs space.
  assert(botty::remoteAdditionalAllocation(32*MiB,0,16*MiB/512)==16*MiB);
  assert(botty::remoteAdditionalAllocation(32*MiB,0,0)==32*MiB);
  assert(botty::remoteAdditionalAllocation(32*MiB,8*MiB,0)==24*MiB);
  assert(botty::remoteAdditionalAllocation(32*MiB,0,UINT64_MAX)==0);
  assert(botty::remoteAdditionalAllocation(73,0,1)==0);
  assert(botty::remoteAdditionalAllocation(0,0,0)==0);
  std::string input(2*RemoteWriteBuffer::capacity+73,'x');
  for(size_t i=0;i<input.size();++i)input[i]=static_cast<char>(i%251);
  std::string output;size_t calls=0;
  RemoteWriteBuffer buffered([&](const char* p,size_t n){++calls;output.append(p,n);return static_cast<ssize_t>(n);});
  for(size_t at=0;at<input.size();at+=16384)buffered.append(input.data()+at,std::min(size_t(16384),input.size()-at));
  assert(buffered.written()==2*RemoteWriteBuffer::capacity&&buffered.pending());
  assert(calls==2);buffered.flush();assert(output==input&&calls==3&&!buffered.pending());
  buffered.flush();assert(calls==3);
  // An interrupted response retains the short valid prefix when explicitly flushed.
  output.clear();calls=0;
  RemoteWriteBuffer shortWrites([&](const char* p,size_t n){if(++calls==1){errno=EINTR;return ssize_t(-1);}n=std::min(n,size_t(137));output.append(p,n);return static_cast<ssize_t>(n);});
  shortWrites.append(input.data(),777);assert(shortWrites.written()==0);shortWrites.flush();
  assert(output==input.substr(0,777)&&shortWrites.written()==777);
  // ENOSPC after a partial write must not retry and duplicate the written prefix.
  calls=0;RemoteWriteBuffer failed([&](const char*,size_t){if(++calls==1)return ssize_t(5);errno=ENOSPC;return ssize_t(-1);});
  failed.append(input.data(),20);try{failed.flush();assert(false);}catch(const std::runtime_error&){}
  assert(failed.written()==5&&calls==2);
  try{failed.flush();assert(false);}catch(const std::runtime_error&){}assert(calls==2);
  // Destroying an unflushed tail is safe: resume uses the on-disk length only.
  output.clear();{RemoteWriteBuffer interrupted([&](const char* p,size_t n){output.append(p,n);return static_cast<ssize_t>(n);});interrupted.append(input.data(),17);}assert(output.empty());
  std::cout<<"Buffered writes preserve exact bytes, coalesce 16 KiB callbacks, retain interrupted tails and stop after disk failures\n";
}
