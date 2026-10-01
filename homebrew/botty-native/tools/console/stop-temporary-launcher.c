// Stop only the recorded temporary websrv process started for this installation.
#include <sys/types.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
int main(void){
 FILE*f=fopen("/data/botty/manager/temporary-helper.pid","r");int expected=0;
 if(!f)return 1;int ok=fscanf(f,"%d",&expected)==1;fclose(f);if(!ok||expected<=1)return 2;
 int mib[]={1,14,8,0};size_t n=0;if(sysctl(mib,4,0,&n,0,0)||n>4*1024*1024)return 3;
 n+=65536;unsigned char*b=malloc(n);if(!b||sysctl(mib,4,b,&n,0,0))return 4;
 int matches=0;for(size_t i=0;i+480<=n;){int32_t size,pid;memcpy(&size,b+i,4);memcpy(&pid,b+i+72,4);if(size<480||i+size>n){free(b);return 5;}char name[33]={0};memcpy(name,b+i+447,32);if(pid==expected&&!strcmp(name,"websrv.elf"))matches++;i+=size;}free(b);
 if(matches!=1)return 6;int result=kill(expected,SIGTERM);
 f=fopen("/data/botty/manager/helper-stop.log","w");if(f){fprintf(f,"SIGTERM websrv pid=%d result=%d\n",expected,result);fclose(f);}return result?7:0;
}
