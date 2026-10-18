// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
namespace botty {
inline uint64_t switchLE(const unsigned char* p,unsigned n){uint64_t v=0;
 for(unsigned i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);
 return v;
 }
// Validate the container tables and extents. Encrypted NCA authenticity and title
// compatibility still belong to the emulator; a valid container is not that proof.
struct SwitchMember {uint64_t start,size;
 };
struct SwitchContainer {
 int fd=-1;
 uint64_t size=0;
 struct stat identity{};
 std::map<std::string,SwitchMember> contents;
 ~SwitchContainer(){if(fd>=0)::close(fd);
 }
 SwitchContainer(const SwitchContainer&)=delete;
 SwitchContainer& operator=(const SwitchContainer&)=delete;
 std::vector<unsigned char> read(uint64_t at,size_t n)const{if(at>size||n>size-at)throw std::runtime_error("Truncated Switch container");
 std::vector<unsigned char>b(n);
 size_t done=0;
 while(done<n){auto got=::pread(fd,b.data()+done,n-done,off_t(at+done));
 if(got<0&&errno==EINTR)continue;
 if(got<=0)throw std::runtime_error("Cannot read Switch container");
 done+=size_t(got);
 }return b;
 }
 explicit SwitchContainer(const fs::path& path){try{
 const auto ext=asciiLower(path.extension().string());
 if(ext!=".nsp"&&ext!=".xci")throw std::runtime_error("Expected an NSP or XCI container");
 fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
 if(fd<0)throw std::runtime_error("Cannot read Switch container");
 struct stat st{};
 if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<16)throw std::runtime_error("Invalid Switch container size");
 size=st.st_size;
 identity=st;
 const auto table=[&](uint64_t base,uint64_t length,bool hashed){
  if(base>size||length>size-base||length<16)throw std::runtime_error("Invalid Switch partition range");
  const auto h=read(base,16);
 if(std::memcmp(h.data(),hashed?"HFS0":"PFS0",4))throw std::runtime_error("Invalid NSP/XCI partition signature");
  const auto count=switchLE(h.data()+4,4),strings=switchLE(h.data()+8,4);
 const uint64_t entrySize=hashed?64:24;
  if(!count||count>16384||!strings||strings>4*1024*1024||count*entrySize+16+strings>length)throw std::runtime_error("Invalid Switch partition table");
  const uint64_t dataAt=base+16+count*entrySize+strings;
 const auto entries=read(base+16,size_t(count*entrySize));
 const auto names=read(base+16+count*entrySize,size_t(strings));
  std::map<std::string,SwitchMember> out;
 std::vector<std::pair<uint64_t,uint64_t>> ranges;
  for(uint64_t i=0;i<count;++i){const auto* e=entries.data()+i*entrySize;
 const auto at=switchLE(e,8),n=switchLE(e+8,8),nameAt=switchLE(e+16,4);
 if(nameAt>=strings||!n||at>length-(dataAt-base)||n>length-(dataAt-base)-at)throw std::runtime_error("Invalid Switch member extent");
   const auto* name=reinterpret_cast<const char*>(names.data()+nameAt);
 const auto* end=static_cast<const char*>(std::memchr(name,0,size_t(strings-nameAt)));
 if(!end||end==name||end-name>1024)throw std::runtime_error("Invalid Switch member name");
 std::string key(name,end);
 if(key.find_first_of("/\\")!=std::string::npos||key=="."||key==".."||!out.emplace(key,SwitchMember{dataAt+at,n}).second)throw std::runtime_error("Duplicate or unsafe Switch member");
 ranges.push_back({at,at+n});
  }
  std::sort(ranges.begin(),ranges.end());
 for(size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)throw std::runtime_error("Overlapping Switch members");
 return out;
 };

 if(ext==".nsp")contents=table(0,size,false);
 else{if(size<512)throw std::runtime_error("Truncated XCI header");
 const auto h=read(256,80);
 if(std::memcmp(h.data(),"HEAD",4))throw std::runtime_error("Invalid XCI signature");
 const auto at=switchLE(h.data()+48,8);
 const auto root=table(at,at<=size?size-at:0,true);
 const auto secure=root.find("secure");
 if(secure==root.end())throw std::runtime_error("XCI has no secure partition");
 contents=table(secure->second.start,secure->second.size,true);
 }
 bool nca=false;
 for(const auto& e:contents)if(asciiLower(fs::path(e.first).extension().string())==".nca")nca=true;
 if(!nca)throw std::runtime_error("Switch container has no NCA content");
 struct stat after{};
 if(::fstat(fd,&after)||after.st_size!=st.st_size||after.st_mtime!=st.st_mtime)throw std::runtime_error("Switch container changed during verification");
 }catch(...){if(fd>=0){::close(fd);
 fd=-1;
 }throw;
 }}
};
inline void validateSwitchContainer(const fs::path& path){SwitchContainer container(path);
 }
}
