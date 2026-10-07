#pragma once
#include "core.hpp"
#include <cstring>
#include <regex>
#include <unistd.h>
#ifdef __PS5__
#include <sys/sysctl.h>
#endif

namespace botty {
inline void nativeProcessesStopped(const std::vector<unsigned char>& bytes,int self) {
  if(bytes.empty()||bytes.size()>4*1024*1024)throw std::runtime_error("Cannot confirm the native process table.");
  bool foundSelf=false;
  for(size_t offset=0;offset<bytes.size();) {
    if(bytes.size()-offset<480)throw std::runtime_error("Unknown native process table layout.");
    int32_t size=0,pid=0;
    std::memcpy(&size,bytes.data()+offset,4);
    std::memcpy(&pid,bytes.data()+offset+72,4);
    if(size<480||size_t(size)>bytes.size()-offset||pid<0)throw std::runtime_error("Unknown native process table layout.");
    const char* name=reinterpret_cast<const char*>(bytes.data()+offset+447);
    const auto end=static_cast<const char*>(std::memchr(name,0,32));
    if(!end)throw std::runtime_error("Malformed native process name.");
    const std::string value(name,end);
    if(value.empty())throw std::runtime_error("Malformed native process name.");
    for(unsigned char c:value)if(c<32||c>126)throw std::runtime_error("Malformed native process name.");
    if(pid!=self&&std::regex_match(value,std::regex("(eboot(\\.bin)?|botty.*)",std::regex::icase)))throw std::runtime_error("Close Botty+ and other native apps to finish the update.");
    foundSelf=foundSelf||pid==self;
    offset+=size_t(size);
  }
  if(!foundSelf)throw std::runtime_error("The manager is missing from the native process table.");
}
inline void confirmNativeProcessesStopped() {
#ifdef __PS5__
  int mib[4]={1,14,8,0};size_t size=0;
  if(sysctl(mib,4,nullptr,&size,nullptr,0)||size>4*1024*1024-65536)throw std::runtime_error("Cannot inspect native processes. Keep Botty+ closed.");
  std::vector<unsigned char> bytes(size+65536);size=bytes.size();
  if(sysctl(mib,4,bytes.data(),&size,nullptr,0)||size>bytes.size())throw std::runtime_error("Native process table changed. Waiting for a safe update.");
  bytes.resize(size);nativeProcessesStopped(bytes,getpid());
#else
  throw std::runtime_error("Native process inspection requires PS5 runtime validation.");
#endif
}
}
