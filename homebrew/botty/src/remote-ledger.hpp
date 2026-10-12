#pragma once
#include "json.hpp"
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace botty {
struct RemoteRange {uint64_t start,end;};
class RemoteRangeLedger {
  uint64_t size_,completed_=0;
  std::vector<RemoteRange> ranges_;
public:
  explicit RemoteRangeLedger(uint64_t size):size_(size){}
  static RemoteRangeLedger fromJson(const nlohmann::json& value,uint64_t size,uint64_t fileLength){
    if(!value.is_array())throw std::runtime_error("Invalid remote segment ledger");
    RemoteRangeLedger result(size);uint64_t previous=0;
    for(const auto& entry:value){
      if(!entry.is_array()||entry.size()!=2||!entry[0].is_number_integer()||!entry[1].is_number_integer()||entry[0]<0||entry[1]<0)throw std::runtime_error("Invalid remote segment ledger");
      const RemoteRange range{entry[0].get<uint64_t>(),entry[1].get<uint64_t>()};
      if(range.start<previous||range.end<=range.start||range.end>size||range.end>fileLength)throw std::runtime_error("Invalid remote segment bounds");
      result.commit(range);previous=range.end;
    }
    return result;
  }
  uint64_t completed() const noexcept{return completed_;}
  nlohmann::json toJson() const {
    auto result=nlohmann::json::array();for(const auto& range:ranges_)result.push_back({range.start,range.end});return result;
  }
  void commit(RemoteRange range){
    if(range.end<=range.start||range.end>size_)throw std::runtime_error("Invalid remote segment bounds");
    auto next=std::lower_bound(ranges_.begin(),ranges_.end(),range.start,[](const RemoteRange& a,uint64_t start){return a.start<start;});
    if((next!=ranges_.end()&&range.end>next->start)||(next!=ranges_.begin()&&std::prev(next)->end>range.start))throw std::runtime_error("Overlapping remote segment");
    const auto bytes=range.end-range.start;
    if(next!=ranges_.begin()&&std::prev(next)->end==range.start){
      auto previous=std::prev(next);previous->end=range.end;
      if(next!=ranges_.end()&&previous->end==next->start){previous->end=next->end;ranges_.erase(next);}
    }else if(next!=ranges_.end()&&range.end==next->start)next->start=range.start;
    else ranges_.insert(next,range);
    completed_+=bytes;
  }
  std::vector<RemoteRange> gaps(uint64_t& cursor,size_t maximum,uint64_t segmentSize) const {
    if(cursor>size_||!maximum||!segmentSize)throw std::runtime_error("Invalid remote gap request");
    std::vector<RemoteRange> result;
    while(cursor<size_&&result.size()<maximum){
      const auto next=std::lower_bound(ranges_.begin(),ranges_.end(),cursor,[](const RemoteRange& a,uint64_t at){return a.end<=at;});
      if(next!=ranges_.end()&&next->start<=cursor){cursor=next->end;continue;}
      const auto end=next==ranges_.end()?size_:next->start;
      const auto boundary=cursor+std::min(segmentSize,end-cursor);result.push_back({cursor,boundary});cursor=boundary;
    }
    return result;
  }
};
}
