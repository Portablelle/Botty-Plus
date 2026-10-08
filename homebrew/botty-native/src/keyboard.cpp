// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard.hpp"
#include "platform.hpp"
#include <algorithm>
#include <cstddef>
#include <type_traits>
extern "C" {
int sceUserServiceGetInitialUser(int*);
int sceKernelLoadStartModule(const char*,std::size_t,const void*,unsigned,const void*,int*);
int sceKernelDlsym(int,const char*,void**);
}
namespace {
// PS5's inherited IME dialog ABI, also used by PS-Play:
// https://github.com/MounirHero/PS-Play/blob/main/main.c
struct DialogResult {std::int32_t endStatus;std::int8_t reserved[12];};
static_assert(sizeof(DialogResult)==16);
using Init=int (*)(const void*,const void*);
using Status=int (*)();
using GetResult=int (*)(DialogResult*);
Init init=nullptr;
Status status=nullptr,term=nullptr;
GetResult getResult=nullptr;
int module=-1;
bool load() noexcept {
    if(init&&status&&getResult&&term)return true;
    if(module<0)module=sceKernelLoadStartModule("/system/common/lib/libSceImeDialog.sprx",0,nullptr,0,nullptr,nullptr);
    if(module<0)return false;
    const auto resolve=[](const char* name,auto& target){void* address=nullptr;if(sceKernelDlsym(module,name,&address)<0||!address)return false;target=reinterpret_cast<std::remove_reference_t<decltype(target)>>(address);return true;};
    return resolve("sceImeDialogInit",init)&&resolve("sceImeDialogGetStatus",status)&&resolve("sceImeDialogGetResult",getResult)&&resolve("sceImeDialogTerm",term);
}
// Reject malformed input and capacity overflow; never truncate a password/link.
bool toUtf16(std::string_view input,std::uint16_t* out,std::size_t capacity) noexcept {
    std::size_t n=0;
    for(std::size_t i=0;i<input.size();){
        const unsigned lead=static_cast<unsigned char>(input[i++]);unsigned cp=lead,count=0,minimum=0;
        if(lead>=0xc2&&lead<=0xdf){cp=lead&31;count=1;minimum=0x80;}
        else if(lead>=0xe0&&lead<=0xef){cp=lead&15;count=2;minimum=0x800;}
        else if(lead>=0xf0&&lead<=0xf4){cp=lead&7;count=3;minimum=0x10000;}
        else if(lead>=0x80||!lead)return false;
        if(i+count>input.size())return false;
        for(unsigned j=0;j<count;++j){const unsigned byte=static_cast<unsigned char>(input[i++]);if((byte&0xc0)!=0x80)return false;cp=(cp<<6)|(byte&63);}
        if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
        const unsigned units=cp>0xffff?2:1;if(n+units>=capacity)return false;
        if(units==2){cp-=0x10000;out[n++]=static_cast<std::uint16_t>(0xd800+(cp>>10));out[n++]=static_cast<std::uint16_t>(0xdc00+(cp&1023));}
        else out[n++]=static_cast<std::uint16_t>(cp);
    }
    out[n]=0;return true;
}
bool toUtf8(const std::uint16_t* input,std::size_t capacity,char* out,unsigned limit) noexcept {
    unsigned n=0;
    for(std::size_t i=0;i<capacity;++i){
        unsigned cp=input[i];if(!cp){out[n]=0;return true;}
        if(cp>=0xd800&&cp<=0xdbff){if(i+1>=capacity)return false;const unsigned low=input[++i];if(low<0xdc00||low>0xdfff)return false;cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);}
        else if(cp>=0xdc00&&cp<=0xdfff)return false;
        const unsigned bytes=cp<128?1:cp<2048?2:cp<65536?3:4;if(n+bytes>limit)return false;
        if(bytes==1)out[n++]=static_cast<char>(cp);
        else {out[n++]=static_cast<char>(bytes==2?0xc0|(cp>>6):bytes==3?0xe0|(cp>>12):0xf0|(cp>>18));if(bytes==4)out[n++]=static_cast<char>(0x80|((cp>>12)&63));if(bytes>=3)out[n++]=static_cast<char>(0x80|((cp>>6)&63));out[n++]=static_cast<char>(0x80|(cp&63));}
    }
    return false;
}
}
namespace botty {
bool NativeKeyboard::open(std::string_view initial,const char* title,unsigned byteLimit,bool password,bool url) noexcept {
    if(running||retained||byteLimit>=output.size()||initial.size()>byteLimit||!load())return false;
    buffer.fill(0);titleBuffer.fill(0);output.fill(0);completed=Result::idle;
    limit=byteLimit;
    const unsigned units=std::min(byteLimit,2048U);
    const auto fail=[&] {buffer.fill(0);titleBuffer.fill(0);return false;};
    if(!toUtf16(initial,buffer.data(),units+1)||!toUtf16(title,titleBuffer.data(),titleBuffer.size()))return fail();
    int user=-1;if(sceUserServiceGetInitialUser(&user)<0)return fail();
    static_assert(sizeof(Parameters)==96&&offsetof(Parameters,buffer)==40&&offsetof(Parameters,title)==72);
    parameters=Parameters{};auto& p=parameters;p.user=user;p.type=url?2:0;p.enterLabel=url?3:password?0:2;
    // PASSWORD / NO_LEARNING for secrets. Disable auto casing/spacing for exact input.
    p.option=2|512|(password?4|32:0);p.maxLength=units;p.buffer=buffer.data();p.horizontal=p.vertical=1;p.title=titleBuffer.data();
    if(init(&p,nullptr)<0)return fail();
    running=true;opened=platform::now();finishing=0;started=false;return true;
}
NativeKeyboard::Result NativeKeyboard::poll() noexcept {
    if(!running)return Result::idle;
    if(completed==Result::idle){
        const int state=status();
        if(state==1){started=true;return Result::pending;}
        // Allow a loaded console time to start IME, without waiting indefinitely.
        if(state==0&&!started&&platform::now()-opened<10000000)return Result::pending;
        DialogResult result{};
        if(state!=2||getResult(&result)<0)completed=Result::failed;
        else if(result.endStatus==1)completed=Result::cancelled;
        else if(result.endStatus!=0||!toUtf8(buffer.data(),buffer.size(),output.data(),output.size()-1))completed=Result::failed;
        else if(std::string_view(output.data()).size()>limit)completed=Result::tooLong;
        else completed=Result::accepted;
    }
    // A never-started/already-closed dialog may reject Term as uninitialized.
    // Bound other teardown retries; quarantine the buffers if ownership is unknown.
    if(term()<0){
        if(status()!=0){
            if(!finishing)finishing=platform::now()+2000000;
            if(platform::now()<finishing)return Result::pending;
            retained=true;
        }
        running=false;output.fill(0);completed=Result::idle;
        if(!retained){buffer.fill(0);titleBuffer.fill(0);}
        return Result::failed;
    }
    if(completed==Result::tooLong){
        // Term succeeded: retain the edits and reopen so they can be shortened.
        output.fill(0);completed=Result::idle;
        (void)toUtf16("Text too long. Please shorten it.",titleBuffer.data(),titleBuffer.size());
        if(init(&parameters,nullptr)>=0){opened=platform::now();finishing=0;started=false;return Result::tooLong;}
        running=false;buffer.fill(0);titleBuffer.fill(0);return Result::failed;
    }
    running=false;buffer.fill(0);titleBuffer.fill(0);
    const auto result=completed;completed=Result::idle;
    if(result!=Result::accepted)output.fill(0);
    return result;
}
}
