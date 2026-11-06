"""ConvertImage decodes ASTC one layer at a time: same bytes, scratch bounded to one layer (issue #20).

On the console an ASTC array converted to BC1/BC3 on the CPU was decoded whole: a 46-layer
1434x1434 array asked for 361 MiB in one piece and the game ended in bad_alloc. Compiles the
production ConvertImage (prepared source, all layers applied) with the real ASTC decoder and BCn
encoders; checks its output against the whole-level decode and that no allocation during the
conversion is larger than one decoded layer. Fails on a tree without layer 99zf. Run on dedie.
"""
from pathlib import Path
import glob
import os
import platform
import shutil
import subprocess
import sys
import tempfile

assert platform.system() == 'Linux', 'Compile on dedie'
root = Path(sys.argv[1]).resolve()
if len(sys.argv) > 2:
    source = Path(sys.argv[2]).resolve()
else:
    source = Path((root / '.local/headless-cache').read_text().strip()) / 'source'
src = source / 'src'
util = (src / 'video_core/texture_cache/util.cpp').read_text()


def function(text, signature):
    assert text.count(signature) == 1, 'Missing production code: ' + signature
    start = text.index(signature)
    at = text.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


convert = function(util, 'void ConvertImage(std::span<const u8> input, const ImageInfo& info, std::span<u8> output,')

# One replacement of util.cpp in the console's video_core: prepare-vulkan-port.py's texture_util.cpp,
# made from the patched source. headless/CMakeLists.txt also added a derived texture_cache_util.cpp
# with a per-layer decode; both objects went into libvideo_core.a and the console linked the other.
replacing = [cmake.name for cmake in sorted((root / 'headless').glob('*.cmake')) + [root / 'headless/CMakeLists.txt']
             for line in cmake.read_text().splitlines()
             if not line.lstrip().startswith('#') and 'REMOVE_ITEM' in line and 'texture_cache/util.cpp' in line]
assert replacing == ['vulkan.cmake'], 'texture_cache/util.cpp replaced in: ' + ', '.join(replacing)

# Boost (astc.cpp's static_vector): the prepared source's CPM copy, the system's, or the pinned .deb.
boost = sorted(glob.glob(str(source / '.cache/cpm/boost/*/libs/*/include')))
temp_root = tempfile.mkdtemp(prefix='eden-astc-layers-')
try:
    temp = Path(temp_root)
    if not boost and not Path('/usr/include/boost/container/static_vector.hpp').exists():
        debs = sorted((root / '.deps').glob('libboost*-dev_*.deb'))
        assert debs, 'No Boost headers: run after a configure (CPM cache) or tools/prepare-build.sh (.deps)'
        subprocess.run(['dpkg-deb', '-x', str(debs[-1]), str(temp / 'boost')], check=True)
        boost = [str(temp / 'boost/usr/include')]
    # Synchronous workers: the decoder and encoders queue rows and wait per slice.
    overlay = temp / 'overlay/common'
    overlay.mkdir(parents=True)
    (overlay / 'thread_worker.h').write_text('''#pragma once
#include <utility>
namespace Common {
struct ThreadWorker {
    template <class F> void QueueWork(F&& work) { std::forward<F>(work)(); }
    void WaitForRequests() {}
};
}
''')
    fixture = r'''
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <vector>
#include "common/alignment.h"
#include "common/common_types.h"
#include "common/scratch_buffer.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"
#include "video_core/textures/workers.h"

static bool tracking = false;
static size_t largest_allocation = 0;
static void* Allocate(size_t size) {
    if (tracking) largest_allocation = std::max(largest_allocation, size);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t size) { return Allocate(size); }
void* operator new[](size_t size) { return Allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace Tegra::Texture {
Common::ThreadWorker& GetThreadWorkers() { static Common::ThreadWorker workers; return workers; }
}
#define ASSERT(test) do { if (!(test)) { std::fprintf(stderr, "ASSERT %s\n", #test); std::abort(); } } while (0)
namespace Settings {
enum class AstcRecompression { Uncompressed, Bc1, Bc3 };
struct Setting { AstcRecompression value; AstcRecompression GetValue() const { return value; } };
struct Values { Setting astc_recompression{AstcRecompression::Bc3}; };
Values values;
}
namespace VideoCore::Surface {
enum class PixelFormat { A8B8G8R8_UNORM, ASTC_2D_4X4_UNORM, ASTC_2D_6X6_UNORM, ASTC_2D_8X8_UNORM, ASTC_2D_12X12_UNORM };
constexpr u32 BytesPerBlock(PixelFormat format) { return format == PixelFormat::A8B8G8R8_UNORM ? 4 : 16; }
constexpr bool IsPixelFormatASTC(PixelFormat format) { return format != PixelFormat::A8B8G8R8_UNORM; }
}
namespace VideoCommon {
using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::IsPixelFormatASTC;
using VideoCore::Surface::PixelFormat;
struct Extent2D { u32 width, height; };
struct Extent3D { u32 width, height, depth; bool operator==(const Extent3D&) const = default; };
struct Offset3D { s32 x, y, z; bool operator==(const Offset3D&) const = default; };
struct SubresourceLayers { s32 base_level; s32 base_layer; s32 num_layers; };
struct BufferImageCopy {
    size_t buffer_offset; size_t buffer_size; u32 buffer_row_length; u32 buffer_image_height;
    SubresourceLayers image_subresource; Offset3D image_offset; Extent3D image_extent;
};
struct ImageInfo { PixelFormat format; Extent3D size; bool astc_opaque = false; };
Extent2D DefaultBlockSize(PixelFormat format) {
    switch (format) {
    case PixelFormat::ASTC_2D_4X4_UNORM: return {4, 4};
    case PixelFormat::ASTC_2D_6X6_UNORM: return {6, 6};
    case PixelFormat::ASTC_2D_8X8_UNORM: return {8, 8};
    case PixelFormat::ASTC_2D_12X12_UNORM: return {12, 12};
    default: return {1, 1};
    }
}
Extent3D AdjustMipSize(Extent3D size, s32 level) {
    return {std::max(size.width >> level, 1U), std::max(size.height >> level, 1U), std::max(size.depth >> level, 1U)};
}
u32 ConvertedBytesPerBlock(PixelFormat) { std::abort(); }
void DecompressBCn(std::span<const u8>, std::span<u8>, BufferImageCopy&, PixelFormat) { std::abort(); }
'''
    main = r'''
} // namespace VideoCommon
using namespace VideoCommon;

static u64 state = 0x9e3779b97f4a7c15ULL;
static u8 Random() { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return u8(state >> 56); }

// One conversion of every level of a w x h x depth image with `layers` layers.
static void Check(PixelFormat format, u32 w, u32 h, u32 depth, u32 layers, u32 levels,
                  Settings::AstcRecompression mode, bool opaque) {
    Settings::values.astc_recompression.value = mode;
    const ImageInfo info{format, {w, h, depth}, opaque};
    const Extent2D tile = DefaultBlockSize(format);
    const bool bc1 = mode == Settings::AstcRecompression::Bc1;
    std::vector<BufferImageCopy> copies;
    size_t input_size = 0, output_size = 0, largest_slice = 0;
    std::vector<size_t> output_offsets;
    for (u32 level = 0; level < levels; ++level) {
        const Extent3D size = AdjustMipSize(info.size, level);
        const size_t slices = size_t{layers} * size.depth;
        copies.push_back({input_size, 0, Common::AlignUp(size.width, tile.width),
                          Common::AlignUp(size.height, tile.height), {s32(level), 0, s32(layers)}, {}, size});
        input_size += size_t{Common::DivideUp(size.width, tile.width)} * Common::DivideUp(size.height, tile.height) * 16 * slices;
        output_offsets.push_back(output_size);
        output_size += size_t{Common::AlignUp(size.width, 4U)} * Common::AlignUp(size.height, 4U) * slices / (bc1 ? 2 : 1);
        largest_slice = std::max(largest_slice, size_t{size.width} * size.height * 4);
    }
    std::vector<u8> input(input_size);
    for (u8& byte : input) byte = Random();
    // Reference: the whole level decoded and encoded at once, as Eden did.
    std::vector<u8> expected(output_size, 0xcd), actual(output_size, 0xcd);
    for (u32 level = 0; level < levels; ++level) {
        const BufferImageCopy& copy = copies[level];
        const u32 slices = layers * copy.image_extent.depth;
        std::vector<u8> rgba(size_t{copy.image_extent.width} * copy.image_extent.height * 4 * slices);
        Tegra::Texture::ASTC::Decompress(std::span<const u8>(input).subspan(copy.buffer_offset), copy.image_extent.width,
                                         copy.image_extent.height, slices, tile.width, tile.height, rgba);
        (bc1 ? Tegra::Texture::BCN::CompressBC1 : Tegra::Texture::BCN::CompressBC3)(
            rgba, copy.image_extent.width, copy.image_extent.height, slices,
            std::span<u8>(expected).subspan(output_offsets[level]));
    }
    largest_allocation = 0;
    tracking = true;
    ConvertImage(input, info, actual, copies);
    tracking = false;
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: output differs from the whole-level decode (%ux%ux%u, %u layers)\n", w, h, depth, layers);
        std::exit(1);
    }
    for (u32 level = 0; level < levels; ++level) {
        ASSERT(copies[level].buffer_offset == output_offsets[level]);
        ASSERT(copies[level].buffer_size == (level + 1 < levels ? output_offsets[level + 1] : output_size) - output_offsets[level]);
    }
    if (largest_allocation > largest_slice) {
        std::fprintf(stderr, "FAIL: %ux%ux%u, %u layers: ConvertImage allocated %zu bytes at once, more than one decoded layer (%zu)\n",
                     w, h, depth, layers, largest_allocation, largest_slice);
        std::exit(1);
    }
}

int main() {
    using M = Settings::AstcRecompression;
    for (PixelFormat format : {PixelFormat::ASTC_2D_4X4_UNORM, PixelFormat::ASTC_2D_6X6_UNORM,
                               PixelFormat::ASTC_2D_8X8_UNORM, PixelFormat::ASTC_2D_12X12_UNORM}) {
        Check(format, 37, 23, 1, 5, 4, M::Bc3, false);  // array, odd sizes, mips
        Check(format, 37, 23, 1, 5, 4, M::Bc1, false);
        Check(format, 64, 48, 1, 3, 1, M::Bc3, false);  // BC3 array
        Check(format, 30, 18, 4, 1, 2, M::Bc3, false);  // 3D: depth slices
        Check(format, 13, 9, 1, 1, 3, M::Bc3, false);   // one layer, unchanged
    }
    // The console's texture: 1434x1434, 46 layers, 6x6 blocks, to BC3 (mip 0).
    Check(PixelFormat::ASTC_2D_6X6_UNORM, 1434, 1434, 1, 46, 1, M::Bc3, false);
    std::printf("PASS: per-layer ASTC->BC1/BC3 in ConvertImage is byte-identical to the whole-level decode "
                "(4x4..12x12, arrays, mips, 3D) and allocates at most one decoded layer "
                "(1434x1434x46: %zu bytes; the whole level was 378369504)\n", largest_allocation);
}
'''
    cpp = temp / 'check.cpp'
    cpp.write_text(fixture + convert + main)  # ConvertImage inside the fixture's namespace VideoCommon
    compiler = os.environ.get('CXX') or next((c for c in ('clang++-18', 'clang++', 'g++') if shutil.which(c)), None)
    assert compiler, 'No host C++ compiler'
    # NDEBUG as in the console build: the decoder's assert()s flag invalid blocks it then decodes
    # to the error colour, and random blocks have many.
    flags = ['-std=c++20', '-O2', '-g', '-DNDEBUG', '-fsanitize=address,undefined', '-D_GLIBCXX_ASSERTIONS',
             '-I' + str(temp / 'overlay'), '-I' + str(src), '-I' + str(source / 'externals/stb')]
    flags += ['-isystem' + b for b in boost]
    binary = temp / 'check'
    subprocess.run([compiler, *flags, str(cpp), str(src / 'video_core/textures/astc.cpp'),
                    str(src / 'video_core/textures/bcn.cpp'), str(source / 'externals/stb/stb_dxt.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
finally:
    shutil.rmtree(temp_root, ignore_errors=True)
