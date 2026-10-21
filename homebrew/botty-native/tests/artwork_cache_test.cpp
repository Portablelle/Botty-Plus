// SPDX-License-Identifier: GPL-3.0-or-later
#include "artwork_cache.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
using botty::ArtworkCache;
int main(){
    char directory[]="/tmp/botty-covers-XXXXXX";assert(mkdtemp(directory));
    const std::string path=std::string(directory)+"/covers.cache";
    std::array<char,96> id{};std::snprintf(id.data(),id.size(),"m:fixture");
    const auto key=ArtworkCache::key(id,"Switch fixture");
    std::array<char,96> first{},second{};
    std::snprintf(first.data(),first.size(),"ranking-seeders-id");
    std::snprintf(second.data(),second.size(),"ranking-recent-id");
    const auto shared=ArtworkCache::key(first,"Astro.Bot.PS5.Release-A");
    assert(shared==ArtworkCache::key(second,"ASTRO BOT PS5 Release-B"));
    assert(shared!=ArtworkCache::key(second,"Another Game PS5 Release-B"));
    second[0]='m';second[1]=':';
    assert(shared==ArtworkCache::key(second,"Astro Bot PS5"));
    std::array<unsigned char,ArtworkCache::pixelBytes> pixels{},out{};pixels.fill(73);
    {
        auto cache=std::make_unique<ArtworkCache>(path.c_str());
        assert(!cache->load(key,out.data()));cache->store(key,pixels.data());
    }
    auto cache=std::make_unique<ArtworkCache>(path.c_str());
    assert(cache->load(key,out.data())&&out==pixels);
    cache->store(shared,pixels.data());
    assert(cache->load(ArtworkCache::key(first,"Astro Bot PS5 Release-C"),out.data())&&out==pixels);
    // Reset the fixture before the capacity/LRU checks below.
    cache.reset();std::remove(path.c_str());
    cache=std::make_unique<ArtworkCache>(path.c_str());cache->store(key,pixels.data());
    assert(cache->load(key,out.data())&&out==pixels); // Reopen, including embedded NUL bytes.
    assert(!cache->load(ArtworkCache::key(id,"Different title"),out.data()));
    // Same-size damage and truncation both fall back to the service.
    FILE* file=std::fopen(path.c_str(),"r+b");assert(file);
    assert(std::fseek(file,ArtworkCache::headerBytes+10,SEEK_SET)==0);assert(std::fputc(0,file)!=EOF);std::fclose(file);
    assert(!cache->load(key,out.data()));cache->store(key,pixels.data());
    assert(truncate(path.c_str(),ArtworkCache::headerBytes+10)==0);
    assert(!cache->load(key,out.data()));cache->store(key,pixels.data());
    // Fill all slots, touch the first, then force an LRU eviction.
    for(unsigned i=1;i<ArtworkCache::capacity;++i){
        std::snprintf(id.data(),id.size(),"j:%u",i);cache->store(ArtworkCache::key(id,"Fixture "+std::to_string(i)+" PS5"),pixels.data());
    }
    assert(cache->load(key,out.data()));
    std::snprintf(id.data(),id.size(),"j:overflow");cache->store(ArtworkCache::key(id,"Overflow PS5"),pixels.data());
    cache=std::make_unique<ArtworkCache>(path.c_str());
    assert(cache->load(key,out.data()));
    id.fill(0);std::snprintf(id.data(),id.size(),"j:1");
    assert(!cache->load(ArtworkCache::key(id,"Fixture 1 PS5"),out.data()));
    struct stat info{};assert(stat(path.c_str(),&info)==0);
    assert(info.st_size==ArtworkCache::capacity*(ArtworkCache::headerBytes+ArtworkCache::pixelBytes));
    ArtworkCache unavailable("/nonexistent/botty/covers");unavailable.store(key,pixels.data());assert(!unavailable.load(key,out.data()));
    const auto heroPath=std::string(directory)+"/heroes.cache";
    auto heroPixels=std::make_unique<std::array<unsigned char,botty::BackgroundCache::pixelBytes>>();heroPixels->fill(137);
    auto heroOut=std::make_unique<std::array<unsigned char,botty::BackgroundCache::pixelBytes>>();
    {auto heroes=std::make_unique<botty::BackgroundCache>(heroPath.c_str());heroes->store(key,heroPixels->data());}
    auto heroes=std::make_unique<botty::BackgroundCache>(heroPath.c_str());
    assert(heroes->load(key,heroOut->data())&&*heroPixels==*heroOut);
    assert(ArtworkCache::pixelBytes!=botty::BackgroundCache::pixelBytes);
    std::remove(heroPath.c_str());std::remove(path.c_str());rmdir(directory);
    std::puts("Artwork cache: persistence, identity, corruption, truncation, LRU and unavailable storage passed.");
}
