// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.hpp"
#include <chrono>
#include <map>
namespace botty {
// Completion counters can precede the torrent client's final timestamp writes.
// Observe an unchanged inventory before admitting a preparation; the worker
// still checks every source identity throughout extraction and publication.
class SwitchSourceSettlement {
public:
 using Clock=std::chrono::steady_clock;
 bool observe(const std::string& key,const json& fingerprint,Clock::time_point now=Clock::now()){
  auto it=seen.find(key);
  if(it==seen.end()){
   if(seen.size()>=64){
    auto oldest=std::min_element(seen.begin(),seen.end(),[](const auto&a,const auto&b){return a.second.lastSeen<b.second.lastSeen;});
    if(now-oldest->second.lastSeen<std::chrono::minutes(5))return false;
    seen.erase(oldest);
   }
   seen.emplace(key,Observation{fingerprint,now,now});return false;
  }
  it->second.lastSeen=now;
  if(it->second.fingerprint!=fingerprint){it->second={fingerprint,now,now};return false;}
  if(now-it->second.since<std::chrono::seconds(2))return false;
  seen.erase(it);return true;
 }
private:
 struct Observation{json fingerprint;Clock::time_point since,lastSeen;};
 std::map<std::string,Observation> seen;
};
}
