#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
static int stage, destroyed, created;
static int init(pthread_attr_t *a){(void)a;return stage==1?ENOMEM:0;}
static int stack(pthread_attr_t *a,size_t size){(void)a;assert(size>=1024*1024);return stage==2?EINVAL:0;}
static int create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void*),void *p){(void)t;(void)a;(void)fn;(void)p;created++;return stage==3?EAGAIN:0;}
static int destroy(pthread_attr_t *a){(void)a;destroyed++;return 0;}
#define pthread_attr_init init
#define pthread_attr_setstacksize stack
#define pthread_create create
#define pthread_attr_destroy destroy
#include "cr_worker_thread.h"
int main(void){pthread_t t;for(stage=0;stage<4;stage++){created=destroyed=0;int r=cr_worker_thread_create(&t,0,0);assert((r==0)==(stage==0));assert(destroyed==(stage!=1));assert(created==(stage==0||stage==3));}return 0;}
