// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.hpp"
#include <fcntl.h>
#include <atomic>
#include <string_view>
#include <sys/types.h>
extern "C" {
std::uint64_t sceKernelGetProcessTime();
int sceKernelUsleep(unsigned);
int sceKernelOpen(const char*,int,mode_t);
std::int64_t sceKernelWrite(int,const void*,std::size_t);
int sceKernelClose(int);
int scePthreadCreate(void**,const void*,void* (*)(void*),void*,const char*);
int scePthreadJoin(void*,void**);
int sceNetInit();
int* sceNetErrnoLoc();
int sceNetSocket(const char*,int,int,int);
int sceNetSetsockopt(int,int,int,const void*,std::uint32_t);
int sceNetConnect(int,const void*,std::uint32_t);
int sceNetSend(int,const void*,std::size_t,int);
int sceNetRecv(int,void*,std::size_t,int);
int sceNetSocketClose(int);
}
namespace botty::platform {
std::uint64_t now() noexcept {return sceKernelGetProcessTime();}
void sleep(unsigned us) noexcept {(void)sceKernelUsleep(us);}
void log(const char* message) noexcept {
    static std::atomic<unsigned> entries{0};
    if(entries.fetch_add(1)>=256)return;
    // Fixed messages only. No responses, tokens, credentials or user content.
    const int fd=sceKernelOpen("/download0/botty-native-network.log",O_WRONLY|O_CREAT|O_APPEND,0644);
    if(fd<0)return;
    const auto size=std::string_view(message).size();
    (void)sceKernelWrite(fd,message,size); (void)sceKernelWrite(fd,"\n",1);
    (void)sceKernelClose(fd);
}
void networkError(const char* operation,int result) noexcept {
    char line[128]{}; unsigned n=0;
    for(const char* p=operation;*p && n<80;++p)line[n++]=*p;
    const auto hex=[&](unsigned value) { for(int shift=28;shift>=0;shift-=4)line[n++]="0123456789abcdef"[(value>>shift)&15]; };
    line[n++]=' ';hex(static_cast<unsigned>(result));line[n++]=' ';hex(static_cast<unsigned>(*sceNetErrnoLoc()));
    log(line);
}
int connectLocal() noexcept {
    static const bool initialized=[] {
        const int result=sceNetInit();
        log(result==0?"Network initialized":"Network initialization returned nonzero - checking socket access");
        return true;
    }();
    (void)initialized;
    const int fd=sceNetSocket("botty_read_only",2,1,6);
    if(fd<0){networkError("socket",fd);return -1;}
    constexpr int timeout=1500000;
    // SceNet options use integer microseconds (BSD timeval is incompatible).
    for(const int option:{0x1105,0x1106,0x1109}) {
        const int result=sceNetSetsockopt(fd,0xffff,option,&timeout,sizeof(timeout));
        if(result<0) {
            networkError(option==0x1105?"send timeout":option==0x1106?"receive timeout":"connect timeout",result);
            (void)sceNetSocketClose(fd);return -1;
        }
    }
    struct Address {std::uint8_t length,family;std::uint16_t port;std::uint32_t host;std::uint16_t virtualPort;std::uint8_t zero[6];};
    constexpr Address address{16,2,0x981f,0x0100007f,0,{0}}; // 127.0.0.1:8088
    static_assert(sizeof(Address)==16);
    const int connected=sceNetConnect(fd,&address,sizeof(address));
    if(connected<0) {networkError("connect",connected);(void)sceNetSocketClose(fd);return -1;}
    return fd;
}
int send(int fd,const void* data,std::size_t size) noexcept {return sceNetSend(fd,data,size,0);}
int receive(int fd,void* data,std::size_t size) noexcept {return sceNetRecv(fd,data,size,0);}
void closeSocket(int fd) noexcept {(void)sceNetSocketClose(fd);}
bool startWorker(void* (*entry)(void*),void* context,void** handle) noexcept {
    return scePthreadCreate(handle,nullptr,entry,context,"botty-api")==0;
}
void joinWorker(void* handle) noexcept {(void)scePthreadJoin(handle,nullptr);}
}
