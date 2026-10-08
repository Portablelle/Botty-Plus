// Host-only ABI simulation for rendering the actual PS5 scene; not a hardware test.
#include <chrono>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <string>
#include <fstream>
#include <iterator>
#include <sys/event.h>
extern "C" {
#include "../vendor/ps5-pad.h"
}
namespace {
void* mapped;
int currentBuffer=0, frames=0, reads=0, connections=0;
const char* mode=std::getenv("BOTTY_PREVIEW_MODE");
bool is(const char* text){return mode && (std::strcmp(mode,text)==0 || (std::strcmp(mode,"checking-game-deletion")==0&&std::strcmp(text,"delete-game")==0) || ((std::strcmp(mode,"slow-password")==0||std::strcmp(mode,"buffered-password")==0)&&std::strcmp(text,"password")==0) || ((std::strcmp(mode,"deleting")==0||std::strcmp(mode,"checking-deletion")==0)&&std::strcmp(text,"delete-torrent")==0));}
const auto mainThread=std::this_thread::get_id();
std::uint64_t artificialRenderTime=0;
bool keyboardSeen=false,resumeSeen=false;
thread_local std::size_t offset=0;
bool videoClosed=false,padClosed=false,workerJoined=false;
thread_local std::string socketRequest,socketResponse;
bool deletionSent=false;
bool nativeUpdateSent=false;
void snapshot() {
    FILE* f=std::fopen("build/preview.ppm","wb");
    if(!f)std::exit(2);
    std::fprintf(f,"P6\n1920 1080\n255\n");
    auto* bytes=static_cast<unsigned char*>(mapped)+currentBuffer*0x1000000;
    for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1920;++x) {
        const unsigned offset=((y<<4)&0x70)^((y<<5)&0xf00)^((y<<9)&0x1000)^((y<<8)&0x4000)^((x<<2)&0xc)^((x<<5)&0x380)^((x<<4)&0x400)^((x<<6)&0x800)^((x<<9)&0xa000);
        const auto block=(y>>7)*15+(x>>7);
        std::fwrite(bytes+(block<<16)+offset,1,3,f);
    }
    std::fclose(f);
}
}
extern "C" {
std::uint64_t sceKernelGetProcessTime(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()+(std::this_thread::get_id()==mainThread?artificialRenderTime:0);}
int sceKernelUsleep(unsigned us){std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}
int sceKernelOpen(const char*,int,mode_t){return is("slow-password")||is("buffered-password")?55:-1;}
std::int64_t sceKernelWrite(int,const void* data,std::size_t size){std::string_view message(static_cast<const char*>(data),size);if(message=="Workflow: keyboard")keyboardSeen=true;if(message.find("resumed")!=std::string_view::npos)resumeSeen=true;return static_cast<std::int64_t>(size);}
int sceKernelClose(int){return 0;}
int scePthreadCreate(void** handle,const void*,void* (*entry)(void*),void* context,const char*){*handle=new std::thread([=]{entry(context);});return 0;}
int scePthreadJoin(void* handle,void**){auto* t=static_cast<std::thread*>(handle);t->join();delete t;workerJoined=true;return 0;}
int* sceNetErrnoLoc(){static int error=0;return &error;}
int sceNetInit(){return 0;}
int sceNetSocket(const char*,int,int,int){offset=0;socketRequest.clear();socketResponse.clear();return 1;}
int sceNetSetsockopt(int,int,int,const void*,std::uint32_t){return 0;}
int sceNetConnect(int,const void*,std::uint32_t){return is("offline")||(is("reconnecting")&&++connections>4)?-1:0;}
int sceNetSend(int,const void* bytes,std::size_t n,int){socketRequest.append(static_cast<const char*>(bytes),n);return static_cast<int>(n);}
int sceNetRecv(int,void* b,std::size_t n,int){
    if(socketRequest.find("POST /api/torrent ")==0||socketRequest.find("POST /api/delete-library-game ")==0){
        deletionSent=true;
        if(is("deleting"))std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if((is("checking-deletion")||is("checking-game-deletion"))&&deletionSent)return -1;
    if(socketResponse.empty()) {
        std::string body=R"({"app":"Botty","version":"0.1.1","titleId":"BTTY00001","apiVersion":1})";
        if(socketRequest.find("GET /api/processing ")==0){
            body=R"({"tasks":[]})";
            if(const char* path=std::getenv("BOTTY_PREVIEW_PROCESSING_FILE")){std::ifstream input(path);body.assign(std::istreambuf_iterator<char>(input),{});}
        }
        if(socketRequest.find("GET /api/bootstrap ")==0)body=R"({"apiVersion":1,"token":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
        if(socketRequest.find("GET /api/connections ")==0) {
            assert(socketRequest.find("X-Botty-Token: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")!=std::string::npos);
            body=R"({"apiVersion":1,"url":"http://192.168.1.50:8088","username":"botty","password":"B7mQ2x"})";
        }
        if(socketRequest.find("GET /api/state ")==0)body=R"({"freeBytes":879609302220,"library":"/data/shadowmount","transmissionReady":true,"extracting":true,"extractionControls":true,"torrents":[{"id":1,"name":"Open source game collection - Volume 1","status":4,"peersConnected":42,"peersSendingToUs":8,"peersGettingFromUs":3,"percentDone":0.64,"leftUntilDone":4638564679,"totalSize":12884901888,"rateDownload":8388608,"rateUpload":262144,"errorString":""},{"id":2,"name":"Homebrew showcase archive","status":0,"percentDone":0.22,"leftUntilDone":4187593114,"totalSize":5368709120,"rateDownload":0,"rateUpload":0},{"id":3,"name":"Community demo assets","files":[{"name":"Community Demo.part1.rar"},{"name":"Community Demo.part2.rar"}],"status":6,"percentDone":1,"leftUntilDone":0,"totalSize":1073741824,"rateDownload":0,"rateUpload":65536}],"jobs":[{"id":"job-1","name":"Homebrew showcase archive","status":"extracting","phase":"Extracting and checking CRC","bytes":2147483648,"total":5368709120,"extractionRate":25165824,"eta":128},{"id":"job-2","name":"Community demo assets","status":"ready","bytes":1073741824,"total":1073741824,"content":{"kind":"folder","destination":"DEMO00001-app"}},{"id":"job-3","name":"Open source sample","status":"moved","destination":"/data/shadowmount/SAMPLE001-app","content":{"kind":"folder"}}]})";
        if(is("search")&&socketRequest.find("GET /api/state ")==0)body=R"({"freeBytes":879609302220,"transmissionReady":true,"torrents":[],"jobs":[],"searchSupported":true,"search":{"query":"Homebrew","busy":false,"adding":false,"results":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Homebrew demo collection - PS5","size":1073741824,"seeders":24,"leechers":3,"added":false},{"id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","name":"Community sample game - PS5","size":536870912,"seeders":12,"leechers":1,"added":true}]}})";
        if((is("explore")||is("sources"))&&socketRequest.find("GET /api/state ")==0)body=R"({"freeBytes":879609302220,"transmissionReady":true,"torrents":[],"jobs":[],"exploreSupported":true,"explore":{"sort":"seeders","busy":false,"adding":false,"results":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Elden Ring - PS5","size":1073741824,"seeders":24,"completed":642,"published":"2026-09-30T00:00:00Z","leechers":3,"added":false},{"id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","name":"Community sample game - PS5","size":536870912,"seeders":12,"completed":91,"published":"2026-09-28T00:00:00Z","leechers":1,"added":false}]}})";
        if(is("delete-torrent")&&socketRequest.find("GET /api/state ")==0){body.insert(1,"\"torrentRemovalSupported\":true,");}
        if(mode&&std::string_view(mode).starts_with("update-")&&socketRequest.find("GET /api/state ")==0){
            const bool current=is("update-current"),waiting=is("update-waiting"),service=is("update-service");
            body=std::string(R"({"freeBytes":1000000000,"transmissionReady":true,"torrents":[],"jobs":[],"nativeUpdate":{"supported":true,"scope":"installation","status":")")+(current?"current":waiting?"waiting":"available")+
                R"(","installedVersion":"01.006.000","availableVersion":")"+(current||service?"01.006.000":"01.006.001")+
                R"(","installedServiceVersion":"1.5.4","availableServiceVersion":")"+(current?"1.5.4":"1.5.5")+
                R"(","installedWorkerVersion":"1.3.1","availableWorkerVersion":"1.3.1","installedEngineVersion":"0.16.24-botty8","availableEngineVersion":"0.16.24-botty8","updateAvailable":)"+(current?"false":"true")+R"(,"requested":)"+(waiting?"true":"false")+R"(,"closeRequired":)"+(waiting?"true":"false")+
                R"(,"message":")"+(current?"App and services are up to date.":waiting?"Waiting for compression and file operations to finish.":service?"Manager update available. The app is already current.":"A compatible app and services update is available.")+R"("}})";
        }
        // Optional synthetic state for layout checks (host preview only).
        if(socketRequest.find("GET /api/state ")==0){
            if(const char* path=std::getenv("BOTTY_PREVIEW_STATE_FILE")){
                std::ifstream fixture(path);
                if(fixture)body.assign(std::istreambuf_iterator<char>(fixture),std::istreambuf_iterator<char>());
            }
        }
        if(is("delete-game")&&socketRequest.find("GET /api/state ")==0){auto at=body.find("\"extracting\":true");if(at!=std::string::npos)body.replace(at,17,"\"extracting\":false");body.insert(1,"\"libraryDeletionSupported\":true,");}
        if(is("password")||is("job-actions")||is("move-confirm")||is("delete-torrent")){auto at=body.find("\"extracting\":true");if(at!=std::string::npos)body.replace(at,17,"\"extracting\":false");}
        if(socketRequest.find("POST ")==0)body="{}";
        if(socketRequest.find("POST /api/native-update ")==0){nativeUpdateSent=true;if(is("update-exit"))body=R"({"apiVersion":1,"scope":"installation","status":"queued","version":"01.006.001","serviceVersion":"1.5.5","transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";}
        socketResponse="HTTP/1.1 200 OK\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    }
    const auto size=std::min(n,socketResponse.size()-offset);std::memcpy(b,socketResponse.data()+offset,size);offset+=size;return static_cast<int>(size);
}
int sceNetSocketClose(int){return 0;}
int sceUserServiceInitialize(void*){return 0;}
int sceUserServiceGetInitialUser(int* u){*u=1;return 0;}
int scePadInit(){return 0;}
int scePadOpen(int,int,int,void*){return 1;}
int scePadRead(int,PS5_PadData* p,int){
    *p={};p->connected=1;p->leftStick.x=p->leftStick.y=128;p->timestamp=2*++reads;
    if((reads==3&&(is("diagnostics")||is("extracted")||is("library")||is("connections")))||(reads==5&&(is("library")||is("connections")))||(reads==7&&is("connections")))p->buttons=PS5_PAD_BUTTON_R1;
    if(reads==3&&is("reconnecting"))p->buttons=PS5_PAD_BUTTON_TRIANGLE;
    if(reads==3&&(is("quit")||is("exit")))p->buttons=PS5_PAD_BUTTON_CIRCLE;
    if(reads==4&&is("exit"))p->buttons=PS5_PAD_BUTTON_RIGHT;
    if(reads==5&&is("exit"))p->buttons=PS5_PAD_BUTTON_CROSS;
    if(reads==5&&is("details"))p->buttons=PS5_PAD_BUTTON_CROSS;
    if(reads==3&&(is("actions")||is("confirm")||is("password")||is("result")))p->buttons=PS5_PAD_BUTTON_OPTIONS;
    if(reads==3&&is("delete-torrent"))p->buttons=PS5_PAD_BUTTON_OPTIONS;
    if((reads==5||reads==7||reads==9)&&is("delete-torrent"))p->buttons=PS5_PAD_BUTTON_DOWN;
    if(reads==11&&is("delete-torrent"))p->buttons=PS5_PAD_BUTTON_CROSS;
    if(reads==13&&(is("deleting")||is("checking-deletion")))p->buttons=PS5_PAD_BUTTON_RIGHT;
    if(reads==15&&(is("deleting")||is("checking-deletion")))p->buttons=PS5_PAD_BUTTON_CROSS;
    if(reads==3&&is("keyboard"))p->buttons=PS5_PAD_BUTTON_SQUARE;
    if(reads==5&&(is("confirm")||is("result")))p->buttons=PS5_PAD_BUTTON_CROSS;
    if(reads==3&&is("password"))p->buttons=PS5_PAD_BUTTON_DOWN;
    if(reads==5&&is("password"))p->buttons=PS5_PAD_BUTTON_DOWN;
    if(reads==7&&is("password"))p->buttons=PS5_PAD_BUTTON_OPTIONS;
    if(reads==9&&is("password"))p->buttons=PS5_PAD_BUTTON_DOWN;
    if(reads==11&&is("password"))p->buttons=PS5_PAD_BUTTON_DOWN;
    if((reads==13||reads==15)&&is("password"))p->buttons=PS5_PAD_BUTTON_CROSS;
    if(reads==7&&is("result"))p->buttons=PS5_PAD_BUTTON_RIGHT;
    if(reads==9&&is("result"))p->buttons=PS5_PAD_BUTTON_CROSS;
    if((reads==3||reads==5)&&(is("job-actions")||is("move-confirm")||is("delete-game")))p->buttons=PS5_PAD_BUTTON_R1;
    if(reads==7&&(is("job-actions")||is("move-confirm")))p->buttons=PS5_PAD_BUTTON_OPTIONS;
    if(reads==7&&is("delete-game"))p->buttons=PS5_PAD_BUTTON_RIGHT;
    if(reads==9&&is("delete-game"))p->buttons=PS5_PAD_BUTTON_OPTIONS;
    if(reads==11&&is("delete-game"))p->buttons=PS5_PAD_BUTTON_CROSS;
    if(reads==13&&is("checking-game-deletion"))p->buttons=PS5_PAD_BUTTON_RIGHT;
    if(reads==15&&is("checking-game-deletion"))p->buttons=PS5_PAD_BUTTON_CROSS;
    if(reads==9&&is("move-confirm"))p->buttons=PS5_PAD_BUTTON_CROSS;
    if((is("sources")||is("storage")||is("download-mode"))&&reads==3)p->buttons=PS5_PAD_BUTTON_CROSS;
    if(is("download-mode")&&reads==5)p->buttons=PS5_PAD_BUTTON_CROSS;
    if((is("update-confirm")||is("update-exit")||is("update-uncertain"))&&reads==7)p->buttons=PS5_PAD_BUTTON_CROSS;
    if((is("update-exit")||is("update-uncertain"))&&reads==9)p->buttons=PS5_PAD_BUTTON_RIGHT;
    if((is("update-exit")||is("update-uncertain"))&&reads==11)p->buttons=PS5_PAD_BUTTON_CROSS;
    if(is("update-uncertain")&&reads==18)assert(nativeUpdateSent&&!videoClosed);
    if(is("update-exit")&&reads==18){std::fputs("Expected acknowledged update to exit before capture\n",stderr);std::exit(3);}
    if(reads==18){if(is("slow-password")||is("buffered-password")){assert(keyboardSeen);assert(!resumeSeen);std::puts("Archive selection reached the password keyboard with single taps.");}snapshot();std::exit(0);}
    if(is("buffered-password")&&p->buttons){p[1]=p[0];p[1].buttons=0;++p[1].timestamp;return 2;}
    return 1;
}
int scePadClose(int){padClosed=true;return 0;}
int sceSystemServiceLoadExec(const char* path,const char**){
    assert(std::strcmp(path,"exit")==0);
    assert(videoClosed&&padClosed&&workerJoined);
    if(is("update-exit"))assert(nativeUpdateSent);
    std::puts("Orderly quit released video, controller and network worker");
    std::exit(0);
}
std::size_t sceKernelGetDirectMemorySize(){return 0x4000000;}
int sceKernelAllocateDirectMemory(std::int64_t,std::int64_t,std::size_t,std::size_t,int,std::int64_t* p){*p=0;return 0;}
int sceKernelMapDirectMemory(void** p,std::size_t n,int,int,std::int64_t,std::size_t){*p=std::calloc(1,n);mapped=*p;return 0;}
int sceKernelMunmap(void* p,std::size_t){std::free(p);return 0;}
int sceKernelReleaseDirectMemory(std::int64_t,std::size_t){return 0;}
int sceKernelSendNotificationRequest(std::uint32_t,void*,std::size_t,int){return 0;}
int sceSystemServiceHideSplashScreen(){return 0;}
int sceVideoOutOpen(std::int32_t,std::int32_t,std::int32_t,const void*){return 1;}
int sceVideoOutSetFlipRate(std::int32_t,std::int32_t){return 0;}
int sceVideoOutSubmitFlip(std::int32_t,std::int32_t index,std::uint32_t,std::int64_t){currentBuffer=index;if(is("slow-password"))artificialRenderTime+=2500000;return 0;}
int sceVideoOutWaitVblank(std::int32_t){return 0;}
void sceVideoOutSetBufferAttribute2(void*,std::uint64_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint64_t,std::uint32_t,std::uint64_t){}
int sceVideoOutRegisterBuffers2(std::int32_t,std::int32_t,std::int32_t,void*,std::int32_t,void*,std::int32_t,void*){return 0;}
bool ps5ObserveOwnedAllocation(const void* p) noexcept{return p!=nullptr;}
int sceKernelCreateEqueue(struct kevent** q,const char*){*q=nullptr;return 0;}
int sceKernelWaitEqueue(struct kevent*,struct kevent*,int,int* n,unsigned*){*n=1;++frames;sceKernelUsleep(50000);return 0;}
int sceKernelDeleteEqueue(struct kevent*){return 0;}
int sceVideoOutAddFlipEvent(struct kevent*,int,void*){return 0;}
int sceVideoOutDeleteFlipEvent(struct kevent*,int){return 0;}
void sceVideoOutClose(int){videoClosed=true;}
}
