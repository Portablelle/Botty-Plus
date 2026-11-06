"""Exercise prepared production performance code on dedie, including cache misses/remaps."""
from pathlib import Path
import platform
import subprocess
import sys

assert platform.system() == 'Linux', 'Compile on dedie'
engine, output = (Path(p).resolve() for p in sys.argv[1:])
source = Path((engine / '.local/headless-cache').read_text().strip()) / 'source'
output.mkdir(parents=True, exist_ok=True)


def function(text, signature):
    assert text.count(signature) == 1, signature
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


buffer = (source / 'src/video_core/buffer_cache/buffer_cache.h').read_text()
code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <vector>
#include "video_core/page_filter.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/texture_cache/descriptor_table.h"
#include "staging_buffers.h"
using DAddr=u64;using GPUVAddr=u64;
using BufferId=u32;
constexpr u64 operator""_MiB(unsigned long long n){return n*1024*1024;}
namespace Core {constexpr u64 DEVICE_PAGESIZE=4096;}
#define LOG_DEBUG(...) ((void)0)
#define ASSERT_MSG(x,...) assert(x)
struct Binding {DAddr device_addr{};GPUVAddr gpu_addr{};u32 size{},buffer_id{};bool operator==(const Binding&)const=default;};
constexpr Binding NULL_BINDING{};
struct Memory {
 u64 pointer=0x1024,word=4096,serial=1;size_t id=1;u64 base=0x100000;mutable int layouts=0;
 template<class T>T Read(u64 address)const{return T(address==16?pointer:word);}
 u64 GetMemoryLayoutSize(u64)const{++layouts;return 16*1024*1024;}
 std::optional<DAddr> GpuToCpuAddress(u64 a)const{return a+base;}
 u64 MapSerial()const{return serial;}size_t GetID()const{return id;}
};
struct Runtime {u32 alignment=256;u32 GetStorageBufferAlignment()const{return alignment;}};
struct Tracker {
 static constexpr u64 HIGHER_PAGE_BITS=22, NUM_HIGH_PAGES=16;
 struct Manager{u64 cpu_epoch=1;};
 std::array<Manager*,NUM_HIGH_PAGES> top_tier{};
 u64 CpuWriteEpoch(u64,u64)const noexcept;
};
struct Buffer{};
template<class P>struct BufferCache {
 Memory* gpu_memory;Runtime runtime;static inline bool enabled=true;
 static bool BottyDrawTuning(){return enabled;}
 Binding StorageBufferBinding(GPUVAddr,u32,bool)const;
 Tracker memory_tracker;int walks=0;bool clean=false;
 struct BottySyncedRange{const void* buffer{};DAddr device_addr{};u32 size{};u64 epoch=~u64{0};};
 std::array<BottySyncedRange,512> botty_synced_ranges{};
 bool SynchronizeBuffer(Buffer&,DAddr,u32);
 bool SynchronizeBufferPages(Buffer&,DAddr,u32){++walks;return clean;}
};
'''
code += 'template<class P>\n' + function(buffer, 'Binding BufferCache<P>::StorageBufferBinding(')
code += 'template<class P>\n' + function(buffer, 'bool BufferCache<P>::SynchronizeBuffer(')
tracker = (source / 'src/video_core/buffer_cache/memory_tracker_base.h').read_text()
code += function(tracker, '[[nodiscard]] u64 CpuWriteEpoch(').replace('CpuWriteEpoch(', 'Tracker::CpuWriteEpoch(').replace('VAddr cpu_addr', 'u64 cpu_addr')
code += r'''
int main(){
 // A conservative page filter must never hide registered or overflowing intervals.
 VideoCommon::PageFilter filter;
 assert(!filter.Any(4095,2));filter.Mark(4095,2);
 assert(filter.Any(4095,1)&&filter.Any(4096,1)&&!filter.Any(8192,4096));
 assert(!filter.Any(0,0));assert(filter.Any(~u64{0}-3,10));
 filter.Mark(~u64{0}-3,10);assert(filter.Any(8192,1));
 // Compare usage accounting against an independent 64-byte-cell oracle across resets.
 std::mt19937 random(42);VideoCommon::UsageTracker usage(65536);
 std::array<bool,1024> used{};
 for(int i=0;i<20000;++i){
   if(i%199==0){usage.Reset();used.fill(false);}
   u64 a=random()%65536,n=random()%(65536-a+1);
   if(i%2){usage.Track(a,n);if(n)for(u64 k=a/64;k<Common::DivCeil(a+n,u64{64});++k)used[k]=true;}
   else {bool expected=false;if(n)for(u64 k=a/64;k<Common::DivCeil(a+n,u64{64});++k)expected|=used[k];assert(usage.IsUsed(a,n)==expected);}
 }
 // Shrinking and exposing a descriptor limit must not resurrect old validity bits.
 VideoCommon::DescriptorTable<u64> descriptors;
 descriptors.Refresh(16,63);assert(descriptors.descriptors.size()==64);
 descriptors.read_descriptors[0]=~u64{0};descriptors.Invalidate();assert(descriptors.read_descriptors[0]==0);
 descriptors.Refresh(16,128);descriptors.read_descriptors[2]=~u64{0};
 descriptors.Refresh(16,1);descriptors.Refresh(16,128);assert(descriptors.read_descriptors[2]==0);
 // Exact staging capacity, completed-only releases and cursor bounds.
 using namespace Eden::StagingBuffers;
 constexpr u64 MiB=1ULL<<20;
 static_assert(Size(341*MiB,29,true)==352*MiB);
 static_assert(Holds(352*MiB,29)==352*MiB);
 struct Entry{std::vector<int> mapped_span;unsigned log2_level;bool deferred,free;};
 struct Level{std::vector<Entry> entries;size_t delete_index=2,iterate_index=3;}level;
 level.entries={{{},3,false,true},{{},4,true,true},{{},5,false,false}};
 auto released=ReleaseFrom(level,[](const auto&e){return !e.deferred&&e.free;});
 assert(released.buffers==1&&released.bytes==8&&level.entries.size()==2);
 assert(level.delete_index==0&&level.iterate_index==0);
 std::array<Level,6> idle_cache;
 idle_cache[5].entries={{{},5,false,true},{{},5,true,true}};
 idle_cache[4].entries={{{},4,false,true}};
 auto trimmed=TrimIdle(std::array<decltype(idle_cache)*,1>{&idle_cache},
     [](const auto&e){return !e.deferred&&e.free;},16);
 assert(trimmed.bytes==32&&trimmed.buffers==1);
 assert(idle_cache[5].entries.size()==1&&idle_cache[5].entries[0].deferred);
 // Differential storage resolution: every descriptor/mapping/alignment change must miss.
 Memory memory;BufferCache<int> cache{&memory};
 for(int i=0;i<4000;++i){
   switch(i%11){
    case 0:memory.pointer+=4096;break;case 1:memory.word+=64;break;
    case 2:++memory.serial;memory.base+=4096;break;case 3:++memory.id;break;
    case 4:cache.runtime.alignment=cache.runtime.alignment==256?16:256;break;
   }
   const bool written=i%3==0;
   cache.enabled=false;auto expected=cache.StorageBufferBinding(16,0,written);
   cache.enabled=true;auto actual=cache.StorageBufferBinding(16,0,written);assert(actual==expected);
   const int before=memory.layouts;
   assert(cache.StorageBufferBinding(16,0,written)==expected);assert(memory.layouts==before);
 }
 memory.pointer=0;assert(cache.StorageBufferBinding(16,0,false)==NULL_BINDING);
 // Page-aligned guest buffers must spread across the table rather than thrash
 // one four-way set: warm a modest working set, then require layout reuse.
 memory.id=99999;memory.word=4096;
 for(u64 k=0;k<64;++k){memory.pointer=0x10000000+k*4096;cache.StorageBufferBinding(16,0,false);}
 const int warm_layouts=memory.layouts;
 for(u64 k=0;k<64;++k){memory.pointer=0x10000000+k*4096;cache.StorageBufferBinding(16,0,false);}
 assert(memory.layouts-warm_layouts<=16);
 // Epoch hits skip page walks; writes, buffer replacement, untracked and wrapped
 // ranges retain the original synchronization path. A prior upload returns false.
 Buffer first,second;Tracker::Manager manager;
 cache.memory_tracker.top_tier[0]=&manager;
 assert(!cache.SynchronizeBuffer(first,4096,4096));assert(cache.walks==1);
 assert(cache.SynchronizeBuffer(first,4096,4096));assert(cache.walks==1);
 ++manager.cpu_epoch;assert(!cache.SynchronizeBuffer(first,4096,4096));assert(cache.walks==2);
 assert(!cache.SynchronizeBuffer(second,4096,4096));assert(cache.walks==3);
 assert(!cache.SynchronizeBuffer(first,1<<23,4096));assert(cache.walks==4);
 assert(!cache.SynchronizeBuffer(first,1<<23,4096));assert(cache.walks==5);
 cache.enabled=false;cache.SynchronizeBuffer(second,4096,4096);assert(cache.walks==6);
 assert(cache.memory_tracker.CpuWriteEpoch(~u64{0}-10,32)==~u64{0});
}
'''

# Shim only unrelated GPU/rasterizer declarations; the table, filter, usage tracker and
# binding resolver are production code. The complete PS5 build checks their real types.
overlay = output / 'overlay'
for name, text in {
    'common/assert.h': '#pragma once\n#include <cassert>\n#define DEBUG_ASSERT(x) assert(x)\n',
    'video_core/memory_manager.h': '#pragma once\nnamespace Tegra {struct MemoryManager {template<class... T>void ReadBlockUnsafe(T&&...)const{}};}\n',
    'video_core/rasterizer_interface.h': '#pragma once\n',
}.items():
    path = overlay / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
cpp = output / 'performance.cpp'
cpp.write_text(code)
subprocess.run(['c++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
                '-I' + str(overlay), '-I' + str(source / 'src'), '-I' + str(engine / 'headless'),
                str(cpp), '-o', str(output / 'performance')], check=True)
subprocess.run([str(output / 'performance')], check=True)
print('PASS: production page filters, usage accounting, descriptor bounds, staging and mapping-safe bindings (ASan/UBSan)')
