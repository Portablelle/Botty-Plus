#include "remote-buffer.hpp"
#include <cassert>
#include <iostream>
#include <string>
using botty::RemoteWriteBuffer;
int main(){
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
