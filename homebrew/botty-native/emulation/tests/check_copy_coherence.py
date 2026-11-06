"""Focused production-method checks; dedicated Linux host only."""
from pathlib import Path
import platform
import subprocess
import sys
assert platform.system() == 'Linux', 'Run on dedie'
engine, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
source = Path((engine/'.local/headless-cache').read_text().strip())/'source'
def function(text, signature):
    start=text.index(signature); brace=text.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]+'\n'
text=(source/'src/video_core/buffer_cache/buffer_cache.h').read_text()
assert 'if (!SynchronizeBuffer(buffer, segment.device_addr, segment.size))' in text
assert 'runtime.MarkStorageSourceWritten(buffer)' in text
assert 'runtime.MarkStorageSourceGpuWritten(slot_buffers[buffer_id])' in text
text=(engine/'headless/texture_partial.h').read_text()
code=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>
#include "staging_buffers.h"
using u64=uint64_t;using u32=uint32_t;using VkBuffer=u64;
namespace VideoCommon {
 enum class ImageFlagBits {Tracked=1,CpuModified=2};
 ImageFlagBits operator&(ImageFlagBits a,ImageFlagBits b){return ImageFlagBits(int(a)&int(b));}
 ImageFlagBits& operator|=(ImageFlagBits& a,ImageFlagBits b){return a=ImageFlagBits(int(a)|int(b));}
 struct ImageBase {u64 cpu_addr=4096,guest_size_bytes=4096*4096;ImageFlagBits flags=ImageFlagBits::Tracked;};
}
bool True(VideoCommon::ImageFlagBits x){return int(x)!=0;}
bool False(VideoCommon::ImageFlagBits x){return !True(x);}
struct State {bool uploaded=true;std::vector<u64> pages;};
std::unordered_map<u32,State> images;
bool Eligible(const VideoCommon::ImageBase&){return true;}
struct Memory {
 std::map<u64,int> counts;unsigned calls=0;
 void UpdatePagesCachedCount(u64 a,u64 n,int delta){++calls;assert(a%4096==0&&n%4096==0);for(u64 p=a/4096;p<(a+n)/4096;++p){int& c=counts.try_emplace(p,1).first->second;c+=delta;assert(c>=0&&c<=1);}}
};
'''
code+='template<class DeviceMemory>\n'+function(text,'bool Write(')
code+='template<class DeviceMemory>\n'+function(text,'std::vector<u64> End(')
code+=r'''
struct MultiRangeBufferCache {
 struct Entry {bool sparse_handle=false,dirty=false;std::vector<VkBuffer> owners;};
 std::unordered_map<u64,Entry> entries;
 std::unordered_map<VkBuffer,std::unordered_set<u64>> gathered_owners;
 void Invalidate(u64);void InvalidateOwner(VkBuffer);void ForgetOwners(u64,const Entry&);
};
'''
text=(source/'src/video_core/renderer_vulkan/vk_multi_range_buffer.cpp').read_text()
for signature in ['void MultiRangeBufferCache::Invalidate(', 'void MultiRangeBufferCache::InvalidateOwner(', 'void MultiRangeBufferCache::ForgetOwners(']:code+=function(text,signature)
code+=r'''
int main(){
 Memory m;VideoCommon::ImageBase i;images.emplace(1,State{});
 assert(Write(m,i,1,i.cpu_addr+7,1000*4096-9));assert(m.calls==1);
 assert(Write(m,i,1,i.cpu_addr+7,1000*4096-9));assert(m.calls==1);
 assert(Write(m,i,1,i.cpu_addr+1100*4096,10*4096));assert(m.calls==2);
 auto pages=End(m,1);assert(pages.size()==1010);assert(m.calls==4);
 for(auto [p,c]:m.counts)assert(c==1);
 assert(End(m,1).empty());assert(m.calls==4);
 MultiRangeBufferCache c;
 c.entries[1]={false,false,{10,10,11}};c.entries[2]={false,false,{10,12}};c.entries[3]={false,false,{13}};
 for(auto& [key,e]:c.entries)for(auto owner:e.owners)c.gathered_owners[owner].insert(key);
 c.InvalidateOwner(10);assert(c.entries[1].dirty&&c.entries[2].dirty&&!c.entries[3].dirty);
 c.entries[1].dirty=false;c.InvalidateOwner(10);assert(c.entries[1].dirty); // second write, same tick
 c.ForgetOwners(1,c.entries[1]);c.entries.erase(1);
 assert(!c.gathered_owners.contains(11));assert(c.gathered_owners[10].size()==1);
 c.ForgetOwners(2,c.entries[2]);c.entries.erase(2);assert(!c.gathered_owners.contains(10));
 using namespace Eden::StagingBuffers;
 constexpr u64 MiB=1ULL<<20;
 static_assert(Size(341*MiB,29,true)==352*MiB);
 static_assert(!Exact(16*MiB,true)&&Exact(17*MiB,true)&&!Exact(17*MiB,false));
 struct Entry {std::vector<int> mapped_span;unsigned log2_level;bool deferred,free;};
 struct Level {std::vector<Entry> entries;size_t delete_index=2,iterate_index=3;}level;
 level.entries={{{},3,false,true},{{},4,true,true},{{},5,false,false}};
 auto released=ReleaseFrom(level,[](const auto& e){return !e.deferred&&e.free;});
 assert(released.buffers==1&&released.bytes==8&&level.entries.size()==2);
 assert(level.delete_index==0&&level.iterate_index==0);
}
'''
path=out/'performance.cpp';path.write_text(code)
subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-I'+str(engine/'headless'),str(path),'-o',str(out/'performance')],check=True)
subprocess.run([str(out/'performance')],check=True)
print('PASS: production page batching, gathered-owner invalidation, staging size/release (ASan/UBSan)')
