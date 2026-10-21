// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include "platform.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
namespace {
std::atomic<unsigned> calls{0};std::atomic<bool> online{true};
thread_local std::string request,response;thread_local std::size_t offset=0;
std::string wire(const std::string& body){return "HTTP/1.1 200 OK\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;}
}
namespace botty::platform {
std::uint64_t now() noexcept{return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void sleep(unsigned us) noexcept{std::this_thread::sleep_for(std::chrono::microseconds(us));}
void log(const char*) noexcept{}
int connectLocal() noexcept{++calls;request.clear();response.clear();offset=0;return online?1:-1;}
int send(int,const void* data,std::size_t size) noexcept{request.append(static_cast<const char*>(data),size);return int(size);}
int receive(int,void* data,std::size_t size) noexcept{
    if(response.empty()){
        if(request.find("GET /api/bootstrap ")==0)response=wire(R"({"token":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})");
        else{const bool hero=request.find("kind=hero")!=std::string::npos;assert(hero||request.find("kind=cover")!=std::string::npos);response=wire(std::string(hero?640*360*3:160*240*3,hero?'h':'c'));}
    }
    const auto count=std::min(size,response.size()-offset);std::memcpy(data,response.data()+offset,count);offset+=count;return int(count);
}
void closeSocket(int) noexcept{}
bool startWorker(void* (*fn)(void*),void* context,void** handle) noexcept{*handle=new std::thread([=]{fn(context);});return true;}
void joinWorker(void* handle) noexcept{auto* thread=static_cast<std::thread*>(handle);thread->join();delete thread;}
}
int main(){
    char directory[]="/tmp/botty-artwork-worker-XXXXXX";assert(mkdtemp(directory));
    const auto coverPath=std::string(directory)+"/covers",heroPath=std::string(directory)+"/heroes";
    botty::CoverIds ids{};std::array<std::string_view,botty::coverSlots> names{};
    std::snprintf(ids[0].data(),ids[0].size(),"ranking-seeders");names[0]="Demo PS5";
    const auto await=[&](botty::Artwork& artwork){
        auto page=std::make_unique<botty::ArtworkPage>();
        for(unsigned i=0;i<500;++i){artwork.read(*page);if(page->ready[0]&&page->ids[0]==ids[0]&&page->backgroundReady&&page->backgroundId==ids[0]){
            assert(page->pixels[0][0]=='c'&&page->background[0]=='h');return;
        }std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        assert(false&&"Artwork worker timed out");
    };
    {
        auto artwork=std::make_unique<botty::Artwork>(coverPath.c_str(),heroPath.c_str());artwork->start();
        artwork->request(ids,names,ids[0],names[0]);await(*artwork);assert(calls==3);
        // Stop the worker entirely: a subsequent page must render in request()+read(),
        // with neither disk work nor a background thread able to fill the page.
        artwork->stop();online=false;calls=0;
        artwork->request({});
        auto immediate=std::make_unique<botty::ArtworkPage>();artwork->read(*immediate);assert(!immediate->ready[0]);
        artwork->request(ids,names,ids[0],names[0]);
        assert(artwork->read(*immediate)&&immediate->ready[0]&&immediate->backgroundReady);
        // Rankings can assign a different ID to the same game. No image/API fetch follows.
        for(const char* id:{"ranking-completed","ranking-recent","ranking-seeders"}){
            std::snprintf(ids[0].data(),ids[0].size(),"%s",id);artwork->request(ids,names,ids[0],names[0]);
            assert(artwork->read(*immediate)&&immediate->ready[0]&&immediate->backgroundReady);
            assert(immediate->ids[0]==ids[0]&&immediate->backgroundId==ids[0]);
        }
        assert(calls==0);artwork->stop();
    }
    // A fresh worker/app instance must restore both images with the service offline.
    auto artwork=std::make_unique<botty::Artwork>(coverPath.c_str(),heroPath.c_str());artwork->start();
    artwork->request(ids,names,ids[0],names[0]);await(*artwork);assert(calls==0);artwork->stop();
    std::remove(coverPath.c_str());std::remove(heroPath.c_str());rmdir(directory);
    std::puts("Artwork worker: shared ranking identities and offline persistent portraits/heroes passed.");
}
