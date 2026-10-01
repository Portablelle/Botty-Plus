// Read-only console diagnostics. No tokens, credentials or user content.
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
extern int sceNetInit(void),sceNetSocket(const char*,int,int,int),sceNetSocketClose(int);
extern int sceNetSetsockopt(int,int,int,const void*,unsigned),sceNetConnect(int,const void*,unsigned);
extern int* sceNetErrnoLoc(void);
static void net_probe(FILE*out){
 fprintf(out,"SceNet init=%x\n",sceNetInit());int fd=sceNetSocket("botty_probe",2,1,6);fprintf(out,"SceNet socket=%d errno=%x\n",fd,*sceNetErrnoLoc());
 if(fd<0)return;int options[]={0x1105,0x1106,0x1109};int timeout=1500000;
 for(int i=0;i<3;i++){int result=sceNetSetsockopt(fd,0xffff,options[i],&timeout,4);fprintf(out,"SceNet option %x result=%x errno=%x\n",options[i],result,*sceNetErrnoLoc());}
 struct sockaddr_in a={0};a.sin_len=sizeof(a);a.sin_family=2;a.sin_port=htons(8088);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 int result=sceNetConnect(fd,&a,sizeof(a));fprintf(out,"SceNet connect=%x errno=%x\n",result,*sceNetErrnoLoc());sceNetSocketClose(fd);
}
static void health(FILE *out, int port) {
 int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0)return;
 struct timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 struct sockaddr_in a={0};a.sin_len=sizeof(a);a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 int r=connect(fd,(void*)&a,sizeof(a));fprintf(out,"PORT %d connect=%d errno=%d\n",port,r,r<0?errno:0);
 if(r==0&&port==8088){const char request[]="GET /health HTTP/1.0\r\nHost: 127.0.0.1:8088\r\nConnection: close\r\n\r\n";send(fd,request,sizeof(request)-1,0);char b[4096];int n;size_t total=0;while(total<8192&&(n=recv(fd,b,sizeof(b),0))>0){fwrite(b,1,n,out);total+=n;}fputs("\n",out);}
 close(fd);
}
int main(void){
 FILE*out=fopen("/data/botty/manager/service-inspection.log","w");if(!out)return 1;
 int mib[4]={1,14,8,0};size_t n=0;
 if(sysctl(mib,4,NULL,&n,NULL,0)==0&&n<4*1024*1024){n+=65536;unsigned char*b=malloc(n);if(b&&sysctl(mib,4,b,&n,NULL,0)==0){for(size_t i=0;i+480<=n;){int32_t size,pid;memcpy(&size,b+i,4);memcpy(&pid,b+i+72,4);if(size<480||i+size>n)break;char name[33]={0};memcpy(name,b+i+447,32);fprintf(out,"PROCESS %d %s\n",pid,name);i+=size;}}free(b);}
 health(out,8088);health(out,9091);health(out,8080);net_probe(out);fclose(out);return 0;
}
