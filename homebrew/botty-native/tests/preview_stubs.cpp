// Host-only ABI simulation for rendering the actual PS5 scene; not a hardware test.
#include <array>
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
#include "../src/host_preview_event.hpp"
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
// IME fixture used by the real draw-loop overlay regression.
std::uint16_t* overlayImeText=nullptr;
bool overlayImeFullMagnet=false;
int overlayImeInit(const void* p,const void*){std::memcpy(&overlayImeText,static_cast<const char*>(p)+40,sizeof(overlayImeText));return 0;}
int overlayImeStatus(){return is("search-typing")?1:2;}
int overlayImeResult(void* result){
    assert(overlayImeText);*static_cast<int*>(result)=0;
    if(overlayImeFullMagnet){
        constexpr char16_t prefix[]=u"magnet:?xt=urn:btih:";
        std::copy(std::begin(prefix),std::end(prefix)-1,overlayImeText);
        std::fill(overlayImeText+std::size(prefix)-1,overlayImeText+2048,u'a');overlayImeText[2048]=0;
    }else {constexpr char16_t edited[]=u"edited";std::copy(std::begin(edited),std::end(edited),overlayImeText);}
    return 0;
}
int overlayImeTerm(){return 0;}
int sceKernelLoadStartModule(const char*,std::size_t,const void*,unsigned,const void*,int*){return is("keyboard-overlay")||is("search-typing")?7:-1;}
int sceKernelDlsym(int,const char* name,void** address){
    if(!is("keyboard-overlay")&&!is("search-typing"))return -1;
    if(std::strcmp(name,"sceImeDialogInit")==0)*address=reinterpret_cast<void*>(overlayImeInit);
    else if(std::strcmp(name,"sceImeDialogGetStatus")==0)*address=reinterpret_cast<void*>(overlayImeStatus);
    else if(std::strcmp(name,"sceImeDialogGetResult")==0)*address=reinterpret_cast<void*>(overlayImeResult);
    else if(std::strcmp(name,"sceImeDialogTerm")==0)*address=reinterpret_cast<void*>(overlayImeTerm);
    else return -1;
    return 0;
}
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
        if(socketRequest.find("GET /api/state ")==0)body=R"json({"freeBytes":879609302220,"library":"/data/shadowmount","transmissionReady":true,"extracting":true,"extractionControls":true,"torrents":[{"id":1,"name":"Marvel's Wolverine","status":4,"peersConnected":42,"peersSendingToUs":8,"peersGettingFromUs":3,"percentDone":0.64,"leftUntilDone":38654705664,"totalSize":107374182400,"rateDownload":8388608,"rateUpload":262144,"errorString":""},{"id":2,"name":"007 First Light","status":0,"percentDone":0.22,"leftUntilDone":58411456102,"totalSize":75161927680,"rateDownload":0,"rateUpload":0},{"id":3,"name":"Astro Bot","files":[{"name":"Astro.Bot.PS5.part1.rar"},{"name":"Astro.Bot.PS5.part2.rar"}],"status":6,"percentDone":1,"leftUntilDone":0,"totalSize":48318382080,"rateDownload":0,"rateUpload":65536}],"jobs":[{"id":"job-1","name":"Onimusha: Way of the Sword","status":"extracting","phase":"Extracting and checking CRC","bytes":23622320128,"total":59055800320,"extractionRate":25165824,"eta":1406},{"id":"job-2","name":"Astro Bot","status":"ready","bytes":48318382080,"total":48318382080,"content":{"kind":"folder","destination":"Astro Bot-app"}},{"id":"job-3","name":"Ghost of Yotei","status":"moved","total":91268055040,"destination":"/data/shadowmount/Ghost of Yotei-app","content":{"kind":"folder"}},{"id":"job-4","name":"Assassin's Creed Black Flag Resynced","status":"moved","total":64424509440,"destination":"/data/shadowmount/Black Flag Resynced-app","content":{"kind":"folder"}}]})json";
        if((is("search")||is("search-typing"))&&socketRequest.find("GET /api/state ")==0)body=R"json({"freeBytes":879609302220,"transmissionReady":true,"torrents":[],"jobs":[],"searchSupported":true,"search":{"query":"Astro Bot","busy":false,"adding":false,"results":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Astro Bot","size":48318382080,"seeders":96,"leechers":14,"added":false},{"id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","name":"Astro Bot Digital Deluxe Edition","size":49392123904,"seeders":31,"leechers":4,"added":true}]}})json";
        if((is("explore")||is("sources")||is("discover")||is("get-game")||is("compare")||is("storage")||is("download-mode"))&&socketRequest.find("GET /api/state ")==0)body=R"json({"freeBytes":879609302220,"transmissionReady":true,"torrents":[],"jobs":[],"storageSupported":true,"storage":[{"id":"internal","label":"Internal SSD","available":true,"freeBytes":163208757248},{"id":"external-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","label":"External SSD (usb0)","available":true,"freeBytes":999653638144}],"exploreSupported":true,"explore":{"sort":"newest","busy":false,"adding":false,"results":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Marvel's Wolverine","size":107374182400,"seeders":112,"completed":246,"published":"2026-09-30T00:00:00Z","leechers":14,"added":false,"sources":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","tracker":"Indexer A","name":"Marvel's Wolverine","size":107374182400,"seeders":112,"leechers":14,"completed":246,"published":"2026-09-30T00:00:00Z"},{"id":"gggggggggggggggggggggggggggggggg","tracker":"Indexer B","name":"Marvel's Wolverine","size":107374182400,"seeders":48,"leechers":6,"completed":120,"published":"2026-09-29T00:00:00Z"},{"id":"hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh","tracker":"Indexer C","name":"Marvel's Wolverine","size":105226698752,"seeders":9,"leechers":2,"completed":31,"published":"2026-09-28T00:00:00Z"}]},{"id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","name":"Astro Bot","size":48318382080,"seeders":96,"completed":1820,"published":"2026-09-27T00:00:00Z","leechers":12,"added":false},{"id":"cccccccccccccccccccccccccccccccc","name":"007 First Light","size":75161927680,"seeders":64,"completed":410,"published":"2026-09-25T00:00:00Z","leechers":8,"added":false},{"id":"dddddddddddddddddddddddddddddddd","name":"Onimusha: Way of the Sword","size":59055800320,"seeders":51,"completed":388,"published":"2026-09-22T00:00:00Z","leechers":6,"added":false},{"id":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","name":"Ghost of Yotei","size":91268055040,"seeders":88,"completed":960,"published":"2026-09-20T00:00:00Z","leechers":11,"added":false},{"id":"ffffffffffffffffffffffffffffffff","name":"Assassin's Creed Black Flag Resynced","size":64424509440,"seeders":40,"completed":215,"published":"2026-09-18T00:00:00Z","leechers":5,"added":false}]}})json";
        if((is("delete-torrent")||is("confirm"))&&socketRequest.find("GET /api/state ")==0){body.insert(1,"\"torrentRemovalSupported\":true,");}
        if((is("extract")||is("password"))&&socketRequest.find("GET /api/state ")==0)body.insert(1,R"json("storageSupported":true,"storage":[{"id":"internal","label":"Internal SSD","available":true,"freeBytes":163208757248},{"id":"external-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","label":"External SSD (usb0)","available":true,"freeBytes":999653638144}],)json");
        if(mode&&std::string_view(mode).starts_with("update-")&&socketRequest.find("GET /api/state ")==0){
            const bool current=is("update-current"),waiting=is("update-waiting"),service=is("update-service");
            body=std::string(R"({"freeBytes":1000000000,"transmissionReady":true,"torrents":[],"jobs":[],"nativeUpdate":{"supported":true,"scope":"installation","status":")")+(current?"current":waiting?"waiting":"available")+
                R"(","installedVersion":"01.006.000","availableVersion":")"+(current||service?"01.006.000":"01.006.001")+
                R"(","installedServiceVersion":"1.5.4","availableServiceVersion":")"+(current?"1.5.4":"1.5.5")+
                R"(","installedWorkerVersion":"1.3.1","availableWorkerVersion":"1.3.1","installedEngineVersion":"0.16.24-botty8","availableEngineVersion":"0.16.24-botty8","updateAvailable":)"+(current?"false":"true")+R"(,"requested":)"+(waiting?"true":"false")+R"(,"closeRequired":)"+(waiting?"true":"false")+
                R"(,"message":")"+(current?"App and services are up to date.":waiting?"Waiting for compression and file operations to finish.":service?"Manager update available. The app is already current.":"A compatible app and services update is available.")+R"("}})";
        }
        // Cover fixtures need the artwork capability (host preview only).
        if(std::getenv("BOTTY_PREVIEW_COVERS")&&socketRequest.find("GET /api/state ")==0&&body.starts_with("{"))body.insert(1,"\"catalogArtworkSupported\":true,");
        // Optional synthetic state for layout checks (host preview only).
        if(socketRequest.find("GET /api/state ")==0){
            if(const char* path=std::getenv("BOTTY_PREVIEW_STATE_FILE")){
                std::ifstream fixture(path);
                if(fixture)body.assign(std::istreambuf_iterator<char>(fixture),std::istreambuf_iterator<char>());
            }
        }
        if(is("delete-game")&&socketRequest.find("GET /api/state ")==0){auto at=body.find("\"extracting\":true");if(at!=std::string::npos)body.replace(at,17,"\"extracting\":false");body.insert(1,"\"libraryDeletionSupported\":true,");}
        if(is("password")||is("extract")||is("job-actions")||is("move-confirm")||is("delete-torrent")||is("confirm")){auto at=body.find("\"extracting\":true");if(at!=std::string::npos)body.replace(at,17,"\"extracting\":false");}
        if(socketRequest.find("POST ")==0)body="{}";
        if(is("error-toast")&&socketRequest.find("POST /api/torrent ")==0){
            body=R"({"error":"rTorrent rejected the request. Check the download in Activity."})";
            socketResponse="HTTP/1.1 400 Bad Request\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
        }
        if(socketRequest.find("POST /api/native-update ")==0){nativeUpdateSent=true;if(is("update-exit"))body=R"({"apiVersion":1,"scope":"installation","status":"queued","version":"01.006.001","serviceVersion":"1.5.5","transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";}
        if(socketResponse.empty())socketResponse="HTTP/1.1 200 OK\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    }
    const auto size=std::min(n,socketResponse.size()-offset);std::memcpy(b,socketResponse.data()+offset,size);offset+=size;return static_cast<int>(size);
}
int sceNetSocketClose(int){return 0;}
int sceUserServiceInitialize(void*){return 0;}
int sceUserServiceGetInitialUser(int* u){*u=1;return 0;}
int scePadInit(){return 0;}
int scePadOpen(int,int,int,void*){return 1;}
// Scripted presses on reads 3, 5, 7... for each preview mode; 0 skips a read.
struct Script {const char* mode;std::array<unsigned,8> presses;};
constexpr unsigned X=PS5_PAD_BUTTON_CROSS,SQ=PS5_PAD_BUTTON_SQUARE,TR=PS5_PAD_BUTTON_TRIANGLE,OPT=PS5_PAD_BUTTON_OPTIONS;
constexpr unsigned D=PS5_PAD_BUTTON_DOWN,R=PS5_PAD_BUTTON_RIGHT;
// Activity fixture order (real PS5 titles): running download, extraction, paused
// download, completed download with RAR volumes, ready extraction.
constexpr Script scripts[]={
    {"get-game",{X}},{"sources",{X}},{"storage",{X}},{"download-mode",{X,D,D}},{"compare",{OPT}},
    {"search-typing",{0,0,SQ}},{"keyboard",{SQ}},{"details",{X}},{"library-details",{X}},
    {"actions",{OPT}},{"result",{OPT,X}},{"error-toast",{OPT,X}},
    {"extract",{D,D,D,X,R,R,X}},{"password",{D,D,D,X,R,R,X,SQ}},
    {"slow-password",{D,D,D,X,R,R,X,SQ}},{"buffered-password",{D,D,D,X,R,R,X,SQ}},
    {"confirm",{OPT,D,D,D,X}},{"delete-torrent",{OPT,D,D,D,X}},{"deleting",{OPT,D,D,D,X,R,X}},{"checking-deletion",{OPT,D,D,D,X,R,X}},
    {"job-actions",{D,OPT}},{"move-confirm",{D,D,D,D,OPT,X,X}},
    {"quick-actions",{R,OPT}},{"delete-game",{R,OPT,X}},{"checking-game-deletion",{R,OPT,X,R,X}},
    {"reconnecting",{TR}},{"quit",{D,D}},{"exit",{D,D,X}},
    {"update-confirm",{0,0,X}},{"update-exit",{0,0,X,R,X}},{"update-uncertain",{0,0,X,R,X}},
};
int scePadRead(int,PS5_PadData* p,int){
    *p={};p->connected=1;p->leftStick.x=p->leftStick.y=128;p->timestamp=2*++reads;
    if(mode&&reads>=3&&reads%2==1)for(const auto& script:scripts)if(std::strcmp(mode,script.mode)==0&&static_cast<unsigned>(reads-3)/2<script.presses.size())p->buttons=script.presses[static_cast<unsigned>(reads-3)/2];
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
