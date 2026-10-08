// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace botty {
// The system dialog retains these UTF-16 buffers until Term succeeds.
class NativeKeyboard {
public:
    NativeKeyboard()=default;
    NativeKeyboard(const NativeKeyboard&)=delete;
    NativeKeyboard& operator=(const NativeKeyboard&)=delete;
    // tooLong is a nonterminal notice: the dialog reopens with its edits intact.
    enum class Result { idle, pending, accepted, cancelled, failed, tooLong };
    bool open(std::string_view initial,const char* title,unsigned byteLimit,bool password,bool url) noexcept;
    Result poll() noexcept;
    void clearText() noexcept {output.fill(0);}
    bool active() const noexcept {return running;}
    std::string_view text() const noexcept {return output.data();}
private:
    struct Parameters {
        std::int32_t user;std::uint32_t type;std::uint64_t languages;
        std::uint32_t enterLabel,inputMethod;void* filter;
        std::uint32_t option,maxLength;std::uint16_t* buffer;
        float x,y;std::uint32_t horizontal,vertical;
        const std::uint16_t *placeholder,*title;std::int8_t reserved[16];
    } parameters{};
    std::array<std::uint16_t,2049> buffer{};
    std::array<std::uint16_t,64> titleBuffer{};
    std::array<char,16385> output{};
    std::uint64_t opened=0,finishing=0;
    unsigned limit=0;
    bool running=false,retained=false,started=false;
    Result completed=Result::idle;
};
}
