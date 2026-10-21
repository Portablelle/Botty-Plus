// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
namespace botty {
// One bounded file in the title's writable, persistent download directory.
// Fixed slots avoid directory scans and extra filesystem dependencies on PS5.
template<unsigned PixelBytes=160*240*3,unsigned Capacity=512> class PersistentArtworkCache final {
public:
    static constexpr unsigned capacity=Capacity, pixelBytes=PixelBytes, headerBytes=128;
    using Key=std::array<unsigned char,112>;
    static Key key(const std::array<char,96>& id,std::string_view title) noexcept {
        Key out{};
        if(title.empty()){
            // A missing title must never share an entry with another game.
            for(unsigned i=0;i<id.size()&&id[i];++i)out[i]=static_cast<unsigned char>(id[i]);
        }else{
            // Rankings use random result IDs; retain their shared title identity
            // within a platform, without sharing remake art with emulation.
            std::array<unsigned char,200> normalized{};unsigned length=0;
            const bool emulation=id[0]=='m'&&id[1]==':';unsigned char platform=emulation?2:0;
            const auto lower=[](unsigned char c){return c>='A'&&c<='Z'?static_cast<unsigned char>(c+32):c;};
            const auto wordChar=[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c>=128;};
            for(std::size_t at=0;at<title.size();){
                while(at<title.size()&&!wordChar(title[at]))++at;
                const auto start=at;while(at<title.size()&&wordChar(title[at]))++at;
                if(start==at)break;
                const std::string_view word(title.data()+start,at-start);
                const auto equals=[&](std::string_view expected){
                    if(word.size()!=expected.size())return false;
                    for(unsigned i=0;i<word.size();++i)if(lower(word[i])!=expected[i])return false;
                    return true;
                };
                bool ppsa=word.size()>4&&lower(word[0])=='p'&&lower(word[1])=='p'&&lower(word[2])=='s'&&lower(word[3])=='a';
                for(unsigned i=4;ppsa&&i<word.size();++i)ppsa=word[i]>='0'&&word[i]<='9';
                if(equals("ps5")){
                    if(!emulation)platform=1;
                    if(length)break;
                    continue; // Leading platform tags are not a game identity.
                }
                if(ppsa&&!emulation)platform=1;
                else if(platform!=1&&(equals("nsw")||equals("nsp")||equals("xci")||equals("switch")))platform=2;
                if(length&&length<normalized.size())normalized[length++]=' ';
                for(auto i=start;i<at&&length<normalized.size();++i)normalized[length++]=lower(title[i]);
            }
            if(!length)return key(id,{});
            // Keep a readable prefix as well as a hash of the full normalized key.
            for(unsigned i=0;i<length&&i<96;++i)out[i]=normalized[i];
            std::uint64_t hash=14695981039346656037ULL;
            for(unsigned i=0;i<length;++i){hash^=normalized[i];hash*=1099511628211ULL;}
            for(unsigned i=0;i<8;++i)out[96+i]=static_cast<unsigned char>(hash>>(i*8));
            // Emulation and PS5 may resolve different releases of the same title.
            out[104]=2;out[105]=platform;return out;
        }
        std::uint64_t hash=14695981039346656037ULL;
        for(unsigned char c:title){hash^=c;hash*=1099511628211ULL;}
        for(unsigned i=0;i<8;++i)out[96+i]=static_cast<unsigned char>(hash>>(i*8));
        return out;
    }
private:
    struct Entry {Key key{};std::uint64_t age=0;std::uint32_t checksum=0;};
    std::array<Entry,capacity> entries_{};
    const char* path_;
    bool scanned_=false;
    std::uint64_t clock_=0;
    static std::uint32_t checksum(const void* data) noexcept {
        auto* bytes=static_cast<const unsigned char*>(data);std::uint32_t hash=2166136261U;
        for(unsigned i=0;i<pixelBytes;++i){hash^=bytes[i];hash*=16777619U;}return hash;
    }
    static bool seek(FILE* file,unsigned slot) noexcept {return std::fseek(file,long(slot)*(headerBytes+pixelBytes),SEEK_SET)==0;}
    static std::array<unsigned char,headerBytes> header(const Entry& entry) noexcept {
        std::array<unsigned char,headerBytes> out{};std::memcpy(out.data(),"BC01",4);
        std::memcpy(out.data()+4,entry.key.data(),entry.key.size());
        for(unsigned i=0;i<8;++i)out[116+i]=static_cast<unsigned char>(entry.age>>(8*i));
        for(unsigned i=0;i<4;++i)out[124+i]=static_cast<unsigned char>(entry.checksum>>(8*i));
        return out;
    }
    void scan() noexcept {
        if(scanned_||!path_)return;
        scanned_=true;FILE* file=std::fopen(path_,"rb");if(!file)return;
        for(unsigned i=0;i<capacity;++i){
            std::array<unsigned char,headerBytes> bytes{};
            if(!seek(file,i)||std::fread(bytes.data(),1,bytes.size(),file)!=bytes.size())break;
            if(std::memcmp(bytes.data(),"BC01",4))continue;
            auto& entry=entries_[i];std::memcpy(entry.key.data(),bytes.data()+4,entry.key.size());
            for(unsigned b=0;b<8;++b)entry.age|=std::uint64_t(bytes[116+b])<<(8*b);
            for(unsigned b=0;b<4;++b)entry.checksum|=std::uint32_t(bytes[124+b])<<(8*b);
            // Reject damaged ages before advancing the logical LRU clock.
            if(entry.age>UINT64_MAX/2){entry={};continue;}
            if(entry.age>clock_)clock_=entry.age;
        }
        std::fclose(file);
    }
public:
    explicit PersistentArtworkCache(const char* path) noexcept:path_(path){}
    bool load(const Key& key,void* pixels) noexcept {
        scan();if(!path_)return false;
        for(unsigned i=0;i<capacity;++i){auto& entry=entries_[i];if(!entry.age||entry.key!=key)continue;
            FILE* file=std::fopen(path_,"rb");if(!file)return false;
            const bool read=seek(file,i)&&std::fseek(file,headerBytes,SEEK_CUR)==0&&std::fread(pixels,1,pixelBytes,file)==pixelBytes;
            std::fclose(file);
            if(!read||checksum(pixels)!=entry.checksum){entry={};return false;}
            entry.age=++clock_;
            file=std::fopen(path_,"r+b");if(file){const auto bytes=header(entry);if(seek(file,i))(void)std::fwrite(bytes.data(),1,bytes.size(),file);std::fclose(file);}
            return true;
        }
        return false;
    }
    void store(const Key& key,const void* pixels) noexcept {
        scan();if(!path_)return;
        unsigned slot=0;
        for(unsigned i=0;i<capacity;++i){
            if(entries_[i].age&&entries_[i].key==key){slot=i;break;}
            if(entries_[i].age<entries_[slot].age)slot=i;
        }
        FILE* file=std::fopen(path_,"r+b");if(!file)file=std::fopen(path_,"w+b");if(!file)return;
        entries_[slot]={};
        const std::array<unsigned char,headerBytes> invalid{};
        // Invalidate before replacing pixels. Interrupted writes remain misses.
        bool ok=seek(file,slot)&&std::fwrite(invalid.data(),1,invalid.size(),file)==invalid.size()&&std::fflush(file)==0;
        ok=ok&&std::fwrite(pixels,1,pixelBytes,file)==pixelBytes&&std::fflush(file)==0;
        Entry next{key,++clock_,checksum(pixels)};const auto bytes=header(next);
        ok=ok&&seek(file,slot)&&std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
        if(std::fclose(file)!=0)ok=false;
        if(ok)entries_[slot]=next;
    }
};
using ArtworkCache=PersistentArtworkCache<>;
using BackgroundCache=PersistentArtworkCache<640*360*3,128>;
}

namespace botty {
// A bounded decoded LRU keeps recent pages immediately available on the UI thread.
// Access is protected by Artwork's gate; it performs no filesystem or network work.
template<unsigned Bytes,unsigned Capacity> class RetainedArtwork final {
    struct Item {ArtworkCache::Key key{};std::array<unsigned char,Bytes> pixels{};std::uint64_t age=0;};
    std::array<Item,Capacity> items_{};
    std::uint64_t clock_=0;
public:
    bool load(const ArtworkCache::Key& key,void* output) noexcept {
        for(auto& item:items_)if(item.age&&item.key==key){
            std::memcpy(output,item.pixels.data(),Bytes);item.age=++clock_;return true;
        }
        return false;
    }
    void store(const ArtworkCache::Key& key,const void* pixels) noexcept {
        unsigned slot=0;
        for(unsigned i=0;i<items_.size();++i){
            if(items_[i].age&&items_[i].key==key){slot=i;break;}
            if(items_[i].age<items_[slot].age)slot=i;
        }
        auto& item=items_[slot];item.key=key;std::memcpy(item.pixels.data(),pixels,Bytes);item.age=++clock_;
    }
};
}
