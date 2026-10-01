/*
 * ps5-native-app-boilerplate - Small CPU-rendered demonstration API.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Keeps PS5 VideoOut setup and the bitmap font out of the starter main file.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace ps5::demo
{
enum class Color : std::uint32_t
{
    background = UINT32_C(0xff190d0a),
    panel = UINT32_C(0xff301f17),
    white = UINT32_C(0xffffffff),
    cyan = UINT32_C(0xffffff00),
    magenta = UINT32_C(0xffff00ff),
    yellow = UINT32_C(0xff00ffff),
};

bool load_font() noexcept;
class Canvas;
using DrawScene = bool (*)(Canvas &) noexcept;
void run(DrawScene draw, std::string_view ready_message) noexcept;

class Canvas final
{
  public:
    bool needs_update(std::uint64_t revision) noexcept {
        if(has_revision_ && revision_==revision)return false;
        revision_=revision;has_revision_=true;dirty_=true;return true;
    }
    bool take_resumed() noexcept {const bool result=resumed_;resumed_=false;return result;}
    bool take_dirty() noexcept {const bool result=dirty_;dirty_=false;return result;}
    void poster(unsigned x,unsigned y,unsigned width,unsigned height,std::span<const unsigned char> rgb) noexcept;
    void clear(Color color) noexcept;
    void shade(unsigned alpha) noexcept;
    void rounded(unsigned x,unsigned y,unsigned width,unsigned height,unsigned radius,Color color) noexcept;
    unsigned text_width(std::string_view text,unsigned size) const noexcept;
    void label(unsigned x,unsigned y,std::string_view text,unsigned size,Color color) noexcept;
    void rectangle(unsigned x, unsigned y, unsigned width, unsigned height, Color color) noexcept;
    void circle(unsigned center_x, unsigned center_y, unsigned radius, Color color) noexcept;
    void triangle(unsigned center_x, unsigned top, unsigned half_width, unsigned height,
                  Color color) noexcept;
    void text(unsigned x, unsigned y, std::string_view value, unsigned scale, Color color) noexcept;

  private:
    explicit Canvas(std::uint32_t *pixels) noexcept : pixels_{pixels}
    {
    }

    std::uint32_t *pixels_;
    std::uint64_t revision_=0;
    bool has_revision_=false,dirty_=true,resumed_=false;

    friend void run(DrawScene draw, std::string_view ready_message) noexcept;
};

void read_asset_text(const char *path, std::span<char> destination,
                     std::string_view fallback) noexcept;
} // namespace ps5::demo
