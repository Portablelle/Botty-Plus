#pragma once
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <utility>
#include <functional>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace botty {
// Allocation budget only: neither blocks nor sparse file length are progress.
// st_blocks uses 512-byte units. Avoid multiplication overflow and allow reuse
// of already allocated, uncommitted bytes after an interrupted transfer.
inline uint64_t remoteAdditionalAllocation(uint64_t size,uint64_t committed,uint64_t blocks){
  const uint64_t allocated=blocks>size/512?size:blocks*512;
  return size-std::max(std::min(size,committed),allocated);
}

// Coalesce curl's small callbacks; only written bytes become resume progress.
class RemoteWriteBuffer {
public:
  static constexpr size_t capacity=512*1024;
  using Writer=std::function<ssize_t(const char*,size_t)>;
private:
  Writer writer_;
  std::vector<char> data_=std::vector<char>(capacity);
  size_t used_=0;
  uint64_t written_=0;
  bool failed_=false;
public:
  explicit RemoteWriteBuffer(int fd):writer_([fd](const char* p,size_t n){return ::write(fd,p,n);}){}
  explicit RemoteWriteBuffer(Writer writer):writer_(std::move(writer)){}
  uint64_t written() const noexcept{return written_;}
  bool pending() const noexcept{return used_!=0;}
  void flush(){
    if(failed_)throw std::runtime_error("Local download write failed");
    size_t at=0;
    while(at<used_){
      const auto n=writer_(data_.data()+at,used_-at);
      if(n<0&&errno==EINTR)continue;
      if(n<=0||static_cast<size_t>(n)>used_-at){failed_=true;throw std::runtime_error("Local download write failed");}
      at+=static_cast<size_t>(n);written_+=static_cast<size_t>(n);
    }
    used_=0;
  }
  void append(const char* p,size_t n){
    if(failed_)throw std::runtime_error("Local download write failed");
    while(n){const auto count=std::min(n,capacity-used_);std::memcpy(data_.data()+used_,p,count);used_+=count;p+=count;n-=count;if(used_==capacity)flush();}
  }
};
}
