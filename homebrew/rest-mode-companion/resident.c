// SPDX-License-Identifier: GPL-3.0-only
// System-process residency follows PS5Tailscale v1.0.3; see README for credits.
#include "../botty/src/rest-companion-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef void* ScePthread;
extern int scePthreadCreate(ScePthread*,const void*,void*(*)(void*),void*,const char*);
extern int scePthreadDetach(ScePthread);
extern unsigned int sceKernelSleep(unsigned int);
extern int sceSystemStateMgrRequestToKeepMainOnStandby(const char*);
static int64_t mono(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return t.tv_sec;}
static int read_control(botty_rest_control* c) {
  int fd=open(BOTTY_REST_CONTROL,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return 0;
  struct stat info;if(fstat(fd,&info)||!S_ISREG(info.st_mode)||info.st_size!=(off_t)sizeof(*c)){close(fd);return 0;}
  unsigned char bytes[sizeof(*c)+1];size_t count=0;
  while(count<sizeof(bytes)){
    ssize_t n=read(fd,bytes+count,sizeof(bytes)-count);if(n<0&&errno==EINTR)continue;
    if(n<0){close(fd);return 0;}if(!n)break;count+=(size_t)n;
  }
  close(fd);if(count!=sizeof(*c))return 0;memcpy(c,bytes,sizeof(*c));return 1;
}
static void status(int64_t now,struct timeval boot,int active,int result,uint64_t calls,uint64_t failures,uint64_t owner,uint64_t sequence) {
  char data[768];int size=snprintf(data,sizeof(data),"{\"schema\":1,\"firmware\":%llu,\"boot\":{\"seconds\":%lld,\"microseconds\":%ld},\"pid\":%d,\"ownerPid\":%llu,\"sequence\":%llu,\"monotonicSeconds\":%lld,\"status\":\"%s\",\"lastResult\":%d,\"attempts\":%llu,\"failures\":%llu}\n",(unsigned long long)BOTTY_REST_FW,(long long)boot.tv_sec,(long)boot.tv_usec,getpid(),(unsigned long long)owner,(unsigned long long)sequence,(long long)now,active?(result?"failed":"active"):"inactive",result,(unsigned long long)calls,(unsigned long long)failures);
  if(size<0||(size_t)size>=sizeof(data))return;
  char temporary[]=BOTTY_REST_STATUS ".XXXXXX";
  int fd=mkstemp(temporary);if(fd<0)return;
  size_t offset=0;while(offset<(size_t)size){ssize_t n=write(fd,data+offset,(size_t)size-offset);if(n<0&&errno==EINTR)continue;if(n<=0)break;offset+=(size_t)n;}
  int closed=close(fd);if(offset==(size_t)size&&!closed)(void)rename(temporary,BOTTY_REST_STATUS);
  (void)unlink(temporary);
}
static struct timeval resident_boot;
static void* worker(void* unused) {
  (void)unused;struct timeval boot=resident_boot;
  uint64_t calls=0,failures=0,owner=0,sequence=0;int previous=0,result=-1;int64_t next=0,next_status=0;
  for(;;){
    int64_t now=mono();if(now<0){sceKernelSleep(1);continue;}
    botty_rest_control c={0};int valid=read_control(&c);
    now=mono();if(now<0){sceKernelSleep(1);continue;}
    int active=valid&&botty_rest_control_active(&c,(uint64_t)now,(uint64_t)boot.tv_sec,(uint64_t)boot.tv_usec);
    if(active&&kill((pid_t)c.pid,0)&&errno==ESRCH)active=0;
    if(active&&(!previous||owner!=c.pid)){next=0;result=-1;}
    owner=active?c.pid:0;sequence=active?c.sequence:0;
    now=mono();
    if(now<0){sceKernelSleep(1);continue;}
    if(active&&!botty_rest_control_active(&c,(uint64_t)now,(uint64_t)boot.tv_sec,(uint64_t)boot.tv_usec))active=0;
    if(active&&now>=next){result=sceSystemStateMgrRequestToKeepMainOnStandby("BottySystemCompanion");++calls;if(result)++failures;next=now+(result?5:10);}
    if(now>=next_status||active!=previous){status(now,boot,active,result,calls,failures,owner,sequence);next_status=now+10;}
    previous=active;(void)sceKernelSleep(1);
  }
  return 0;
}
int __wrap___patch_init(void){return 0;}
int main(void){
  size_t length=sizeof(resident_boot);
  int rc=sysctlbyname("kern.boottime",&resident_boot,&length,0,0);
  if(!rc&&(length!=sizeof(resident_boot)||resident_boot.tv_sec<=0))rc=-1;
  ScePthread thread=0;
  if(!rc)rc=scePthreadCreate(&thread,0,worker,0,BOTTY_REST_THREAD);
  if(rc){
    int fd=open("/data/botty/rest-mode-init.failed",O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd>=0){(void)write(fd,"failed\n",7);(void)close(fd);}
    return rc;
  }
  // Thread creation is the readiness boundary; detach failure cannot undo it.
  (void)scePthreadDetach(thread);return 0;
}
