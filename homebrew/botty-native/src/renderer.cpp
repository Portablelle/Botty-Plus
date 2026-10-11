/*
 * ps5-native-app-boilerplate - CPU VideoOut demonstration implementation.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Provides the bounded drawing surface and PS5 presentation loop used by the
 * editable starter application.
 */

#include "renderer.hpp"
#include "platform.hpp"
#include "font_data.hpp"
#include "platform_logo.hpp"
#include "host_preview_event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

extern "C"
{
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                      std::size_t length, std::size_t alignment, int memory_type,
                                      std::int64_t *physical_address);
    int sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                                 std::int64_t physical_address, std::size_t alignment);
    int sceKernelSendNotificationRequest(std::uint32_t device, void *request, std::size_t size,
                                         int blocking);
    int sceKernelUsleep(std::uint32_t microseconds);
    int sceSystemServiceHideSplashScreen();
    int open(const char *path, int flags, ...);
    long read(int descriptor, void *buffer, std::size_t size);
    int close(int descriptor);
    int sceKernelCreateEqueue(struct kevent **, const char *);
    int sceKernelWaitEqueue(struct kevent*, struct kevent*, int, int*, unsigned*);
    int sceKernelDeleteEqueue(struct kevent*);
    int sceVideoOutAddFlipEvent(struct kevent*, int, void*);
    int sceVideoOutDeleteFlipEvent(struct kevent*, int);
    void sceVideoOutClose(int);
    int sceKernelMunmap(void*, std::size_t);
    int sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
    int sceVideoOutOpen(std::int32_t user_id, std::int32_t bus_type, std::int32_t index,
                        const void *param);
    int sceVideoOutSetFlipRate(std::int32_t handle, std::int32_t rate);
    int sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t buffer_index,
                              std::uint32_t flip_mode, std::int64_t flip_argument);
    int sceVideoOutWaitVblank(std::int32_t handle);
    bool ps5ObserveOwnedAllocation(const void *address) noexcept;
}

namespace ps5::demo
{
namespace
{
constexpr unsigned frame_width = 1920;
constexpr unsigned frame_height = 1080;
constexpr std::size_t frame_bytes = 0x1000000;
constexpr std::size_t raster_bytes=((frame_width+127)/128)*((frame_height+127)/128)*65536;
static_assert(raster_bytes<=frame_bytes);
constexpr std::size_t memory_bytes = frame_bytes * 2;
constexpr std::size_t memory_alignment = 0x200000;
constexpr int memory_type_wc_garlic = 3;
constexpr int map_protection = 0x33;
constexpr std::uint64_t pixel_format_rgba8_srgb = UINT64_C(0x8000000022000000);

struct VideoBuffer
{
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
};

struct VideoAttribute
{
    std::uint8_t reserved[80];
};

extern "C" void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute,
                                               std::uint64_t pixel_format,
                                               std::uint32_t tiling_mode, std::uint32_t width,
                                               std::uint32_t height, std::uint64_t option,
                                               std::uint32_t dcc_control,
                                               std::uint64_t dcc_clear_color);
extern "C" int sceVideoOutRegisterBuffers2(std::int32_t handle, std::int32_t set_index,
                                           std::int32_t buffer_index_start, VideoBuffer *buffers,
                                           std::int32_t buffer_count, VideoAttribute *attribute,
                                           std::int32_t category, void *option);

struct NotificationRequest
{
    std::uint8_t reserved[45];
    char message[3075];
};

struct Glyph
{
    char character;
    std::array<std::uint8_t, 7> rows;
};

constexpr std::array<Glyph, 41> glyphs{{
    {'.', {0,0,0,0,0,12,12}}, {':', {0,12,12,0,12,12,0}},
    {'-', {0,0,0,31,0,0,0}}, {'/', {1,1,2,4,8,16,16}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},        {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},     {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 16, 30, 1, 1, 30}},   {'6', {14, 16, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 1, 14}},   {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}}, {'G', {14, 17, 16, 23, 17, 17, 14}},
    {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {31, 4, 4, 4, 4, 4, 31}},
    {'J', {7, 2, 2, 2, 18, 18, 12}},     {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}}, {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 25, 21, 19, 17, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},  {'Y', {17, 17, 10, 4, 4, 4, 4}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},
}};

class File final
{
  public:
    explicit File(int descriptor = -1) noexcept : descriptor_{descriptor}
    {
    }

    ~File()
    {
        reset();
    }

    File(const File &) = delete;
    File &operator=(const File &) = delete;

    File(File &&other) noexcept : descriptor_{std::exchange(other.descriptor_, -1)}
    {
    }

    File &operator=(File &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return descriptor_ >= 0;
    }

    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

  private:
    void reset() noexcept
    {
        if (descriptor_ >= 0)
        {
            (void)close(descriptor_);
            descriptor_ = -1;
        }
    }

    int descriptor_;
};

class LifetimeProbe final
{
  public:
    explicit LifetimeProbe(bool &destroyed) noexcept : destroyed_{&destroyed}
    {
    }

    ~LifetimeProbe()
    {
        *destroyed_ = true;
    }

    LifetimeProbe(const LifetimeProbe &) = delete;
    LifetimeProbe &operator=(const LifetimeProbe &) = delete;

  private:
    bool *destroyed_;
};

NotificationRequest notification{};

void copy_message(std::span<char> destination, std::string_view source) noexcept
{
    if (destination.empty())
        return;

    const std::size_t count =
        source.size() < destination.size() - 1 ? source.size() : destination.size() - 1;
    for (std::size_t index = 0; index < count; ++index)
        destination[index] = source[index];
    destination[count] = '\0';
}

void notify(std::string_view message) noexcept
{
    copy_message(std::span{notification.message}, message);
    (void)sceKernelSendNotificationRequest(0, &notification, sizeof(notification), 0);
}

[[nodiscard]] bool verify_unique_ownership() noexcept
{
    bool destroyed = false;
    {
        std::unique_ptr<LifetimeProbe> probe{new (std::nothrow_t{}) LifetimeProbe{destroyed}};
        if (!ps5ObserveOwnedAllocation(probe.get()))
            return false;
    }
    return destroyed;
}

[[noreturn]] void halt(const char* message) noexcept
{
    botty::platform::log(message);
    notify(message);
    for (;;)
        (void)sceKernelUsleep(1000000);
}

[[nodiscard]] constexpr std::span<const std::uint8_t, 7> glyph_rows(char character) noexcept
{
    if(character>='a' && character<='z')character=static_cast<char>(character-'a'+'A');
    for (const auto &glyph : glyphs)
    {
        if (glyph.character == character)
            return glyph.rows;
    }
    return glyphs.front().rows;
}

[[nodiscard]] constexpr std::size_t tiled_byte_offset(unsigned x, unsigned y) noexcept
{
    const std::uint32_t offset = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^
                                 ((y << 8) & 0x4000U) ^ ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
                                 ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
    const std::uint32_t blocks_per_row = (frame_width + 127U) >> 7;
    const std::uint32_t block_index = (y >> 7) * blocks_per_row + (x >> 7);

    return (static_cast<std::size_t>(block_index) << 16) + offset;
}

void put_pixel_unchecked(std::uint32_t *pixels, unsigned x, unsigned y, Color color) noexcept
{
    auto *bytes = reinterpret_cast<std::uint8_t *>(pixels);
    *reinterpret_cast<std::uint32_t *>(bytes + tiled_byte_offset(x, y)) =
        static_cast<std::uint32_t>(color);
}

std::array<std::uint8_t,botty::font::dataSize> font_pixels{};
// Build-time RGB conversion avoids a decoder or network dependency on console.
std::array<std::uint8_t,960*540*3> backdrop_pixels{};
bool backdrop_ready=false;
std::array<std::array<std::uint8_t,512*512*4>,3> illustration_pixels{};
std::array<bool,3> illustration_ready{};
bool font_ready=false;
void blend_pixel(std::uint32_t* pixels,unsigned x,unsigned y,Color color,unsigned alpha) noexcept {
    if(x>=frame_width||y>=frame_height||alpha==0)return;
    if(alpha==255){put_pixel_unchecked(pixels,x,y,color);return;}
    auto* bytes=reinterpret_cast<std::uint8_t*>(pixels)+tiled_byte_offset(x,y);
    const auto value=static_cast<std::uint32_t>(color);
    for(unsigned channel=0;channel<3;++channel)
        bytes[channel]=static_cast<std::uint8_t>((((value>>(channel*8))&255)*alpha+bytes[channel]*(255-alpha)+127)/255);
    bytes[3]=255;
}

void fill_rect(std::uint32_t *pixels, unsigned x, unsigned y, unsigned width, unsigned height,
               Color color) noexcept
{
    if (x >= frame_width || y >= frame_height)
        return;

    const unsigned right = width > frame_width - x ? frame_width : x + width;
    const unsigned bottom = height > frame_height - y ? frame_height : y + height;
    for (unsigned row = y; row < bottom; ++row)
    {
        for (unsigned column = x; column < right; ++column)
            put_pixel_unchecked(pixels, column, row, color);
    }
}

void blend_rect(std::uint32_t *pixels,unsigned x,unsigned y,unsigned width,unsigned height,Color color,unsigned alpha) noexcept {
    if(alpha>=255){fill_rect(pixels,x,y,width,height,color);return;}
    if(x>=frame_width||y>=frame_height||!alpha)return;
    const unsigned right=width>frame_width-x?frame_width:x+width,bottom=height>frame_height-y?frame_height:y+height;
    for(unsigned row=y;row<bottom;++row)for(unsigned column=x;column<right;++column)blend_pixel(pixels,column,row,color,alpha);
}
// Coverage of an antialiased edge, from the signed distance inside it.
unsigned coverage(float inside) noexcept {return inside<=0?0:inside>=1?255:static_cast<unsigned>(inside*255+.5f);}
// Next UTF-8 code point as a glyph index. Malformed or missing characters show '?'.
unsigned next_glyph(std::string_view value,std::size_t& at) noexcept {
    constexpr unsigned unknown='?'-32;
    const unsigned lead=static_cast<unsigned char>(value[at++]);
    if(lead<128)return lead>=32&&lead<127?lead-32:unknown;
    const unsigned count=lead>=0xf8?0:lead>=0xf0?3:lead>=0xe0?2:lead>=0xc0?1:0;
    if(!count)return unknown;
    char32_t code=lead&(0x3fU>>count);
    for(unsigned i=0;i<count;++i){
        if(at>=value.size()||(static_cast<unsigned char>(value[at])&0xc0)!=0x80)return unknown;
        code=(code<<6)|(static_cast<unsigned char>(value[at++])&63);
    }
    for(unsigned i=0;i<botty::font::extras.size();++i)if(botty::font::extras[i]==code)return 95+i;
    return unknown;
}
const botty::font::Face& face_for(unsigned size,unsigned weight) noexcept {
    const unsigned wanted=weight>=700?800:600;
    const botty::font::Face* face=&botty::font::faces[0];bool found=false;
    for(const auto& candidate:botty::font::faces){
        if(candidate.weight!=wanted)continue;
        if(!found||candidate.size<=size){face=&candidate;found=true;}
    }
    return *face;
}

void fill_circle(std::uint32_t *pixels, unsigned center_x, unsigned center_y, unsigned radius,
                 Color color) noexcept
{
    const int signed_radius = static_cast<int>(radius);
    for (int y = -signed_radius; y <= signed_radius; ++y)
    {
        for (int x = -signed_radius; x <= signed_radius; ++x)
        {
            if (x * x + y * y <= signed_radius * signed_radius)
            {
                const int pixel_x = static_cast<int>(center_x) + x;
                const int pixel_y = static_cast<int>(center_y) + y;
                if (pixel_x >= 0 && pixel_y >= 0)
                {
                    const auto bounded_x = static_cast<unsigned>(pixel_x);
                    const auto bounded_y = static_cast<unsigned>(pixel_y);
                    if (bounded_x < frame_width && bounded_y < frame_height)
                        put_pixel_unchecked(pixels, bounded_x, bounded_y, color);
                }
            }
        }
    }
}

void fill_triangle(std::uint32_t *pixels, unsigned center_x, unsigned top, unsigned half_width,
                   unsigned height, Color color) noexcept
{
    if (height == 0 || center_x >= frame_width)
        return;

    for (unsigned row = 0; row < height; ++row)
    {
        const unsigned half = row * half_width / height;
        const unsigned left = half > center_x ? 0 : center_x - half;
        const unsigned right = half >= frame_width - center_x ? frame_width : center_x + half + 1;
        fill_rect(pixels, left, top + row, right - left, 1, color);
    }
}

void draw_text(std::uint32_t *pixels, unsigned x, unsigned y, std::string_view value,
               unsigned scale, Color color) noexcept
{
    for (const char character : value)
    {
        const auto rows = glyph_rows(character);
        for (unsigned row = 0; row < rows.size(); ++row)
        {
            for (unsigned column = 0; column < 5; ++column)
            {
                if ((rows[row] & (1U << (4 - column))) != 0)
                    fill_rect(pixels, x + column * scale, y + row * scale, scale, scale, color);
            }
        }
        x += 6 * scale;
        if (x >= frame_width)
            return;
    }
}

void flush_range(void *address, std::size_t length) noexcept
{
    auto *at = static_cast<std::uint8_t *>(address);
    const auto *end = at + length;

#ifndef BOTTY_HOST_PREVIEW
    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
#else
    (void)at; (void)end;
#endif
}
} // namespace

bool load_font() noexcept {
#ifdef BOTTY_HOST_PREVIEW
    File file{open("assets/ui-font.bin",0)};
#else
    File file{open("/app0/assets/ui-font.bin",0)};
#endif
    if(!file.valid())return false;
    std::size_t total=0;
    while(total<font_pixels.size()) {
        const long n=read(file.get(),font_pixels.data()+total,font_pixels.size()-total);
        if(n<=0)return false;
        total+=static_cast<std::size_t>(n);
    }
    char extra=0;
    if(read(file.get(),&extra,1)!=0)return false;
    font_ready=true;
    return true;
}
bool load_backdrop() noexcept {
#ifdef BOTTY_HOST_PREVIEW
    File file{open("assets/nebula.rgb",0)};
#else
    File file{open("/app0/assets/nebula.rgb",0)};
#endif
    if(!file.valid())return false;
    std::size_t total=0;
    while(total<backdrop_pixels.size()) {
        const long n=read(file.get(),backdrop_pixels.data()+total,backdrop_pixels.size()-total);
        if(n<=0)return false;
        total+=static_cast<std::size_t>(n);
    }
    char extra=0;
    backdrop_ready=read(file.get(),&extra,1)==0;
    return backdrop_ready;
}
void Canvas::backdrop(bool subdued) noexcept {
    for(unsigned y=0;y<frame_height;++y)for(unsigned x=0;x<frame_width;++x) {
        const auto source=((y/2)*960+x/2)*3;
        // Darken the lower content area and chrome; keep the horizon vivid.
        unsigned light=y<260?90+y*120/260:y<780?210-(y-260)*140/520:70;
        if(subdued)light=light/5;
        unsigned r=9,g=13,b=27;
        if(backdrop_ready){
            r+=backdrop_pixels[source]*light/255;
            g+=backdrop_pixels[source+1]*light/255;
            b+=backdrop_pixels[source+2]*light/255;
        }else {
            // Missing or damaged artwork must not make the UI unusable.
            const unsigned glow=(x/24)*(1080-y)/1080;
            r+=glow/2;g+=glow/3;b+=glow;
        }
        put_pixel_unchecked(pixels_,x,y,static_cast<Color>(0xff000000U|(r>255?255:r)|((g>255?255:g)<<8)|((b>255?255:b)<<16)));
    }
}
void Canvas::fade(unsigned x,unsigned y,unsigned width,unsigned height,Color color,unsigned from,unsigned to,bool horizontal) noexcept {
    const unsigned steps=horizontal?width:height;if(!steps)return;
    for(unsigned i=0;i<steps;++i){
        const unsigned alpha=steps>1?(from*(steps-1-i)+to*i)/(steps-1):from;
        if(horizontal)blend_rect(pixels_,x+i,y,1,height,color,alpha);else blend_rect(pixels_,x,y+i,width,1,color,alpha);
    }
}
void Canvas::gradient(unsigned x,unsigned y,unsigned width,unsigned height,Color top,Color bottom) noexcept {
    if(!height)return;
    const auto a=static_cast<std::uint32_t>(top),b=static_cast<std::uint32_t>(bottom);
    for(unsigned row=0;row<height;++row){
        std::uint32_t color=0xff000000;
        for(unsigned shift=0;shift<24;shift+=8){
            const unsigned channel=(((a>>shift)&255)*(height-row)+((b>>shift)&255)*row)/height;
            color|=channel<<shift;
        }
        fill_rect(pixels_,x,y+row,width,1,static_cast<Color>(color));
    }
}
void load_illustrations() noexcept {
    constexpr const char* names[]={"courier","extractor","vault"};
    for(unsigned i=0;i<illustration_pixels.size();++i){
        char path[96];
#ifdef BOTTY_HOST_PREVIEW
        std::snprintf(path,sizeof(path),"assets/%s.rgba",names[i]);
#else
        std::snprintf(path,sizeof(path),"/app0/assets/%s.rgba",names[i]);
#endif
        File file{open(path,0)};if(!file.valid())continue;
        auto& pixels=illustration_pixels[i];std::size_t total=0;
        while(total<pixels.size()){
            const long n=read(file.get(),pixels.data()+total,pixels.size()-total);
            if(n<=0)break;total+=static_cast<std::size_t>(n);
        }
        char extra=0;illustration_ready[i]=total==pixels.size()&&read(file.get(),&extra,1)==0;
    }
}
bool Canvas::illustration(unsigned asset,unsigned x,unsigned y,unsigned width,unsigned height) noexcept {
    if(asset>=illustration_pixels.size()||!illustration_ready[asset]||!width||!height)return false;
    const auto& pixels=illustration_pixels[asset];
    for(unsigned row=0;row<height&&y+row<frame_height;++row)for(unsigned col=0;col<width&&x+col<frame_width;++col){
        const auto source=((row*512/height)*512+col*512/width)*4;
        const Color color=static_cast<Color>(0xff000000U|pixels[source]|(pixels[source+1]<<8)|(pixels[source+2]<<16));
        if(pixels[source+3])blend_pixel(pixels_,x+col,y+row,color,pixels[source+3]);
    }
    return true;
}
void Canvas::rounded(unsigned x,unsigned y,unsigned width,unsigned height,unsigned radius,Color color,unsigned alpha) noexcept {
    if(radius>width/2)radius=width/2;
    if(radius>height/2)radius=height/2;
    if(alpha>255)alpha=255;
    blend_rect(pixels_,x+radius,y,width-radius*2,height,color,alpha);
    blend_rect(pixels_,x,y+radius,radius,height-radius*2,color,alpha);
    blend_rect(pixels_,x+width-radius,y+radius,radius,height-radius*2,color,alpha);
    for(unsigned py=0;py<radius;++py)for(unsigned px=0;px<radius;++px) {
        unsigned inside=0;
        for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx) {
            const int dx=static_cast<int>((radius-px)*8)-sx*2-1;
            const int dy=static_cast<int>((radius-py)*8)-sy*2-1;
            if(dx*dx+dy*dy<=static_cast<int>(radius*radius*64))++inside;
        }
        const auto corner=(inside*255+8)/16*alpha/255;
        blend_pixel(pixels_,x+px,y+py,color,corner);
        blend_pixel(pixels_,x+width-1-px,y+py,color,corner);
        blend_pixel(pixels_,x+px,y+height-1-py,color,corner);
        blend_pixel(pixels_,x+width-1-px,y+height-1-py,color,corner);
    }
}
unsigned Canvas::text_width(std::string_view value,unsigned size,unsigned weight,int tracking) const noexcept {
    if(!font_ready)return static_cast<unsigned>(value.size())*6*(size/8?size/8:1);
    const auto& face=face_for(size,weight);
    int width=0;bool first=true;
    for(std::size_t at=0;at<value.size();){width+=face.glyphs[next_glyph(value,at)].advance+(first?0:tracking);first=false;}
    return width>0?static_cast<unsigned>(width):0;
}
void Canvas::label(unsigned x,unsigned y,std::string_view value,unsigned size,Color color,unsigned weight,int tracking) noexcept {
    if(!font_ready){text(x,y,value,size/8?size/8:1,color);return;}
    const auto& face=face_for(size,weight);
    int pen=static_cast<int>(x);
    for(std::size_t at=0;at<value.size();) {
        const auto& glyph=face.glyphs[next_glyph(value,at)];
        for(unsigned gy=0;gy<glyph.height;++gy)for(unsigned gx=0;gx<glyph.width;++gx) {
            const int dx=pen+glyph.left+static_cast<int>(gx);
            const int dy=static_cast<int>(y)+glyph.top+static_cast<int>(gy);
            if(dx>=0&&dy>=0)blend_pixel(pixels_,static_cast<unsigned>(dx),static_cast<unsigned>(dy),color,font_pixels[glyph.offset+gy*glyph.width+gx]);
        }
        pen+=glyph.advance+tracking;
        if(pen>=static_cast<int>(frame_width))break;
    }
}
void Canvas::stroke(float x0,float y0,float x1,float y1,float width,Color color) noexcept {
    const float radius=width/2,dx=x1-x0,dy=y1-y0,length=dx*dx+dy*dy;
    const float left=std::fmin(x0,x1)-radius-1,top=std::fmin(y0,y1)-radius-1,right=std::fmax(x0,x1)+radius+1,bottom=std::fmax(y0,y1)+radius+1;
    for(int py=static_cast<int>(top>0?top:0);py<=static_cast<int>(bottom)&&py<static_cast<int>(frame_height);++py)
        for(int px=static_cast<int>(left>0?left:0);px<=static_cast<int>(right)&&px<static_cast<int>(frame_width);++px){
            const float cx=px+.5f,cy=py+.5f;
            float t=length>0?((cx-x0)*dx+(cy-y0)*dy)/length:0;t=t<0?0:t>1?1:t;
            const float ex=cx-(x0+t*dx),ey=cy-(y0+t*dy);
            blend_pixel(pixels_,static_cast<unsigned>(px),static_cast<unsigned>(py),color,coverage(radius+.5f-std::sqrt(ex*ex+ey*ey)));
        }
}
void Canvas::clip_corners(unsigned x,unsigned y,unsigned width,unsigned height,unsigned radius,Color behind) noexcept {
    if(radius>width/2)radius=width/2;
    if(radius>height/2)radius=height/2;
    for(unsigned py=0;py<radius;++py)for(unsigned px=0;px<radius;++px){
        const float dx=radius-px-.5f,dy=radius-py-.5f;
        const unsigned outside=255-coverage(radius+.5f-std::sqrt(dx*dx+dy*dy));
        blend_pixel(pixels_,x+px,y+py,behind,outside);
        blend_pixel(pixels_,x+width-1-px,y+py,behind,outside);
        blend_pixel(pixels_,x+px,y+height-1-py,behind,outside);
        blend_pixel(pixels_,x+width-1-px,y+height-1-py,behind,outside);
    }
}
void Canvas::ring(float center_x,float center_y,float radius,float width,Color color) noexcept {
    const float reach=radius+width/2+1;
    for(int py=static_cast<int>(center_y-reach>0?center_y-reach:0);py<=static_cast<int>(center_y+reach)&&py<static_cast<int>(frame_height);++py)
        for(int px=static_cast<int>(center_x-reach>0?center_x-reach:0);px<=static_cast<int>(center_x+reach)&&px<static_cast<int>(frame_width);++px){
            const float ex=px+.5f-center_x,ey=py+.5f-center_y;
            blend_pixel(pixels_,static_cast<unsigned>(px),static_cast<unsigned>(py),color,coverage(width/2+.5f-std::fabs(std::sqrt(ex*ex+ey*ey)-radius)));
        }
}

void Canvas::shade(unsigned alpha) noexcept {
    if(alpha>255)alpha=255;
    for(unsigned y=0;y<frame_height;++y)for(unsigned x=0;x<frame_width;++x)
        blend_pixel(pixels_,x,y,static_cast<Color>(0xff000000),alpha);
}
void Canvas::clear(Color color) noexcept
{
    fill_rect(pixels_, 0, 0, frame_width, frame_height, color);
}

void Canvas::poster(unsigned x,unsigned y,unsigned width,unsigned height,std::span<const unsigned char> rgb,unsigned radius,unsigned alpha,bool crop) noexcept {
    if(rgb.size()!=160*240*3||!width||!height||!alpha)return;
    if(radius>width/2)radius=width/2;
    if(radius>height/2)radius=height/2;
    // Source window in 1/256 pixels. Cover crops the long axis around its center.
    std::uint64_t windowX=0,windowY=0,windowWidth=159*256,windowHeight=239*256;
    if(crop&&std::uint64_t{width}*240>std::uint64_t{height}*160){windowHeight=std::uint64_t{159}*256*height/width;windowY=(239*256-windowHeight)/2;}
    else if(crop){windowWidth=std::uint64_t{239}*256*width/height;windowX=(159*256-windowWidth)/2;}
    for(unsigned row=0;row<height&&y+row<frame_height;++row)for(unsigned col=0;col<width&&x+col<frame_width;++col){
        unsigned mask=alpha>255?255:alpha;
        if(radius){
            const float dx=col<radius?radius-col-.5f:col>=width-radius?col+.5f-(width-radius):0;
            const float dy=row<radius?radius-row-.5f:row>=height-radius?row+.5f-(height-radius):0;
            if(dx>0&&dy>0)mask=mask*coverage(radius+.5f-std::sqrt(dx*dx+dy*dy))/255;
            if(!mask)continue;
        }
        // Bilinear enlargement keeps covers smooth at TV viewing sizes.
        const unsigned sx=static_cast<unsigned>(windowX+(width>1?windowWidth*col/(width-1):0));
        const unsigned sy=static_cast<unsigned>(windowY+(height>1?windowHeight*row/(height-1):0));
        const unsigned x0=sx/256,y0=sy/256,x1=x0<159?x0+1:x0,y1=y0<239?y0+1:y0;
        const unsigned fx=sx%256,fy=sy%256;std::uint32_t color=0xff000000U;
        for(unsigned channel=0;channel<3;++channel){
            const unsigned top=rgb[(y0*160+x0)*3+channel]*(256-fx)+rgb[(y0*160+x1)*3+channel]*fx;
            const unsigned bottom=rgb[(y1*160+x0)*3+channel]*(256-fx)+rgb[(y1*160+x1)*3+channel]*fx;
            color|=((top*(256-fy)+bottom*fy)/65536)<<(channel*8);
        }
        blend_pixel(pixels_,x+col,y+row,static_cast<Color>(color),mask);
    }
}
void Canvas::gameCase(unsigned x,unsigned y,unsigned width,unsigned height,std::span<const unsigned char> rgb) noexcept {
    if(width<32||height<48)return;
    const unsigned band=width/7,spine=width/45+2;
    rounded(x,y,width,height,6,static_cast<Color>(0xffd95515));
    rectangle(x+spine,y+3,width-spine-3,band,Color::white);
    // Use the real platform wordmark, independently of the game's illustration.
    const unsigned logoWidth=width*3/5,logoHeight=logoWidth*70/320;
    const unsigned logoX=x+spine+width/10,logoY=y+3+(band-logoHeight)/2;
    for(unsigned row=0;row<logoHeight;++row)for(unsigned col=0;col<logoWidth;++col)
        blend_pixel(pixels_,logoX+col,logoY+row,static_cast<Color>(0xff0b0908),botty::art::ps5Logo[(row*70/logoHeight)*320+col*320/logoWidth]);
    poster(x+spine,y+band+3,width-spine-3,height-band-6,rgb);
}

void Canvas::rectangle(unsigned x, unsigned y, unsigned width, unsigned height,
                       Color color) noexcept
{
    fill_rect(pixels_, x, y, width, height, color);
}

void Canvas::circle(unsigned center_x, unsigned center_y, unsigned radius, Color color) noexcept
{
    fill_circle(pixels_, center_x, center_y, radius, color);
}

void Canvas::triangle(unsigned center_x, unsigned top, unsigned half_width, unsigned height,
                      Color color) noexcept
{
    fill_triangle(pixels_, center_x, top, half_width, height, color);
}

void Canvas::text(unsigned x, unsigned y, std::string_view value, unsigned scale,
                  Color color) noexcept
{
    draw_text(pixels_, x, y, value, scale, color);
}

void read_asset_text(const char *path, std::span<char> destination,
                     std::string_view fallback) noexcept
{
    copy_message(destination, fallback);
    File descriptor{open(path, 0)};
    if (!descriptor.valid() || destination.empty())
        return;

    const long count = read(descriptor.get(), destination.data(), destination.size() - 1);
    if (count <= 0)
        return;

    std::size_t length = static_cast<std::size_t>(count);
    while (length > 0 && (destination[length - 1] == '\r' || destination[length - 1] == '\n'))
        --length;
    destination[length] = '\0';
}

void run(DrawScene draw, std::string_view ready_message) noexcept
{
    if (!verify_unique_ownership())
        halt("Botty Native: unique ownership failed");
    if (draw == nullptr)
        halt("Botty Native: scene callback missing");

    (void)sceSystemServiceHideSplashScreen();
    const int video = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (video < 0)
        halt("Botty Native: sceVideoOutOpen failed");

    const std::size_t pool_size = sceKernelGetDirectMemorySize();
    if (pool_size < memory_bytes)
        halt("Botty Native: insufficient direct memory");

    std::int64_t physical_address = 0;
    int result =
        sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(pool_size), memory_bytes,
                                      memory_alignment, memory_type_wc_garlic, &physical_address);
    if (result < 0)
        halt("Botty Native: direct-memory allocation failed");

    void *mapped = nullptr;
    result = sceKernelMapDirectMemory(&mapped, memory_bytes, map_protection, 0, physical_address,
                                      memory_alignment);
    if (result < 0)
        halt("Botty Native: direct-memory mapping failed");

    // CPU blending must never read write-combined VideoOut memory. Draw the
    // tiled image in ordinary cached RAM, then copy it sequentially for scanout.
    std::unique_ptr<std::uint32_t[]> raster{new (std::nothrow_t{}) std::uint32_t[raster_bytes/4]{}};
    if(!raster)halt("Botty Native: cached raster allocation failed");
    Canvas canvas{raster.get()};
    auto *second_frame = static_cast<std::uint8_t *>(mapped) + frame_bytes;
    botty::platform::log("Cached CPU raster enabled; VideoOut buffers are write-only");

    std::array<VideoBuffer, 2> buffers{{
        {mapped, nullptr, nullptr, nullptr},
        {second_frame, nullptr, nullptr, nullptr},
    }};
    VideoAttribute attribute{};
    (void)sceVideoOutSetFlipRate(video, 0);
    sceVideoOutSetBufferAttribute2(&attribute, pixel_format_rgba8_srgb, 0, frame_width,
                                   frame_height, 0, 0, 0);

    result = sceVideoOutRegisterBuffers2(video, 0, 0, buffers.data(),
                                         static_cast<std::int32_t>(buffers.size()), &attribute, 0,
                                         nullptr);
    if (result < 0)
        halt("Botty Native: buffer registration failed");
    struct kevent* queue=nullptr;
    if(sceKernelCreateEqueue(&queue,"Botty flip")<0 || sceVideoOutAddFlipEvent(queue,video,nullptr)<0)
        halt("Botty Native: flip event initialization failed");
    botty::platform::log("VideoOut initialized at 1920x1080");
    notify(ready_message);
    unsigned index=0,loggedFrames=0;
    std::int64_t serial=1;
    for (;;) {
        const auto started=botty::platform::now();
        if(!draw(canvas))break;
        if(!canvas.take_dirty()){(void)sceKernelUsleep(16667);continue;}
        auto* pixels=index==0 ? mapped : second_frame;
        const auto drawn=botty::platform::now();
        std::memcpy(pixels,raster.get(),raster_bytes);
        flush_range(pixels,raster_bytes);
        if(sceVideoOutSubmitFlip(video,static_cast<int>(index),1,serial++)<0)
            halt("Botty Native: submit flip failed");
        struct kevent event{};
        int count=0;
        // Wait for the submitted frame before reusing buffers. Shell suspension
        // can delay this event; resume continues here, then refreshes input/API.
        const auto waiting=botty::platform::now();
        if(sceKernelWaitEqueue(queue,&event,1,&count,nullptr)<0 || count!=1)
            halt("Botty Native: flip event failed - close using PS menu");
        const auto finished=botty::platform::now();
        // A long CPU draw/copy is not suspension and must not discard input.
        if(finished-waiting>2000000)canvas.resumed_=true;
        if(loggedFrames<8 || (drawn-started>100000 && loggedFrames<24)){
            char timing[128];std::snprintf(timing,sizeof(timing),"Frame draw=%llu us copy=%llu us wait=%llu us",static_cast<unsigned long long>(drawn-started),static_cast<unsigned long long>(waiting-drawn),static_cast<unsigned long long>(finished-waiting));botty::platform::log(timing);++loggedFrames;
        }
        index=1-index;
    }
    (void)sceVideoOutDeleteFlipEvent(queue,video);
    (void)sceKernelDeleteEqueue(queue);
    sceVideoOutClose(video);
    (void)sceKernelMunmap(mapped,memory_bytes);
    (void)sceKernelReleaseDirectMemory(physical_address,memory_bytes);
    botty::platform::log("VideoOut resources released");
}
} // namespace ps5::demo
