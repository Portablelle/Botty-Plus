// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
namespace {
struct Parameters {
    std::int32_t user;std::uint32_t type;std::uint64_t languages;
    std::uint32_t enterLabel,inputMethod;void* filter;std::uint32_t option,maxLength;
    std::uint16_t* buffer;float x,y;std::uint32_t horizontal,vertical;
    const std::uint16_t *placeholder,*title;std::int8_t reserved[16];
};
struct Result {int endStatus;char reserved[12];};
std::uint64_t now=0;
int state=0,endStatus=0,initError=0,resultError=0,termError=0,userError=0,terms=0;
std::uint16_t* text=nullptr;
Parameters param{};
int init(const Parameters* p,const void* extended){assert(!extended);param=*p;text=p->buffer;return initError;}
int status(){return state;}
int result(Result* r){r->endStatus=endStatus;return resultError;}
int term(){++terms;return termError;}
void finish(const std::u16string& value){assert(text&&value.size()<=param.maxLength);std::copy(value.begin(),value.end(),text);text[value.size()]=0;state=2;}
}
namespace botty::platform {std::uint64_t now() noexcept {return ::now;}}
extern "C" {
int sceUserServiceGetInitialUser(int* user){*user=42;return userError;}
int sceKernelLoadStartModule(const char* path,std::size_t,const void*,unsigned,const void*,int*){assert(std::strcmp(path,"/system/common/lib/libSceImeDialog.sprx")==0);return 7;}
int sceKernelDlsym(int module,const char* name,void** address){
    assert(module==7);
    if(std::strcmp(name,"sceImeDialogInit")==0)*address=reinterpret_cast<void*>(init);
    else if(std::strcmp(name,"sceImeDialogGetStatus")==0)*address=reinterpret_cast<void*>(status);
    else if(std::strcmp(name,"sceImeDialogGetResult")==0)*address=reinterpret_cast<void*>(result);
    else if(std::strcmp(name,"sceImeDialogTerm")==0)*address=reinterpret_cast<void*>(term);
    else return -1;
    return 0;
}
}
int main(){
    botty::NativeKeyboard keyboard;using R=botty::NativeKeyboard::Result;
    assert(keyboard.poll()==R::idle);
    assert(keyboard.open("Pokémon 🚀","Search games",200,false,false));
    assert(param.user==42&&param.maxLength==200&&param.type==0&&param.enterLabel==2);
    assert(param.option==(2|512));assert(text[3]==u'é'&&text[8]==0xd83d&&text[9]==0xde80);
    assert(keyboard.poll()==R::pending);state=1;assert(keyboard.poll()==R::pending);
    assert(!keyboard.open("other","Search games",200,false,false));
    finish(u"Pokémon 🚀");assert(keyboard.poll()==R::accepted);
    assert(keyboard.text()=="Pokémon 🚀"&&!keyboard.active()&&text[0]==0&&terms==1);
    keyboard.clearText();assert(keyboard.text().empty());
    assert(keyboard.open("secret","Archive password (optional)",1024,true,false));
    assert((param.option&(4|32))==(4|32)&&param.maxLength==1024);
    endStatus=1;state=2;assert(keyboard.poll()==R::cancelled&&keyboard.text().empty()&&text[0]==0);
    endStatus=0;assert(keyboard.open("","Archive password (optional)",1024,true,false));
    finish(u"");assert(keyboard.poll()==R::accepted&&keyboard.text().empty());
    assert(keyboard.open("magnet:?xt=urn:btih:","Add a magnet link",16384,false,true));
    assert(param.maxLength==2048&&param.type==2&&param.enterLabel==3);
    finish(u"magnet:?xt=urn:btih:abcdef&dn=test");termError=-1;
    assert(keyboard.poll()==R::pending&&keyboard.active()&&text[0]);
    termError=0;assert(keyboard.poll()==R::accepted&&!keyboard.active()&&text[0]==0);
    keyboard.clearText();
    assert(keyboard.open("old query","Search games",200,false,false));
    finish(std::u16string(101,u'é'));assert(keyboard.poll()==R::tooLong&&keyboard.active()&&keyboard.text().empty());
    assert(std::all_of(text,text+101,[](auto c){return c==u'é';})&&text[101]==0);
    finish(std::u16string(100,u'é'));assert(keyboard.poll()==R::accepted&&keyboard.text().size()==200);
    keyboard.clearText();
    assert(!keyboard.open(std::string(2049,'a'),"Add a magnet link",16384,false,true));
    assert(!keyboard.open(std::string("\xc0\xaf",2),"Search games",200,false,false));
    assert(!keyboard.open(std::string("\xed\xa0\x80",3),"Search games",200,false,false));
    assert(!keyboard.open(std::string("\xf0\x9f",2),"Search games",200,false,false));
    assert(!keyboard.open("abc","Search games",2,false,false));
    assert(keyboard.open("","Search games",200,false,false));
    finish(std::u16string(1,0xd800));assert(keyboard.poll()==R::failed);
    assert(keyboard.open("","Search games",200,false,false));
    state=0;now+=3000000;assert(keyboard.poll()==R::pending&&keyboard.active());
    state=1;assert(keyboard.poll()==R::pending);finish(u"late startup");assert(keyboard.poll()==R::accepted);keyboard.clearText();
    assert(keyboard.open("","Search games",200,false,false));
    state=0;now+=10000001;termError=-1;assert(keyboard.poll()==R::failed&&!keyboard.active());termError=0;
    assert(keyboard.open("","Search games",200,false,false));
    state=2;resultError=-1;assert(keyboard.poll()==R::failed);resultError=0;
    assert(keyboard.open("","Search games",200,false,false));
    state=2;endStatus=2;assert(keyboard.poll()==R::failed);endStatus=0;
    const auto wiped=[&] {assert(std::all_of(text,text+2049,[](auto c){return c==0;}));assert(std::all_of(param.title,param.title+64,[](auto c){return c==0;}));};
    assert(!keyboard.open(std::string("secret\xc0\xaf",8),"Password",1024,true,false));wiped();
    assert(!keyboard.open("secret",std::string(64,'t').c_str(),1024,true,false));wiped();
    initError=-1;assert(!keyboard.open("secret","Password",1024,true,false));wiped();initError=0;
    userError=-1;assert(!keyboard.open("secret","Password",1024,true,false));wiped();
    userError=0;assert(keyboard.open("","Search games",200,false,false));
    finish(u"secret");termError=-1;assert(keyboard.poll()==R::pending);
    now+=2000001;assert(keyboard.poll()==R::failed&&!keyboard.active());
    assert(keyboard.text().empty()&&!keyboard.open("new","Search games",200,false,false));
    std::puts("Native keyboard UTF conversion, limits, privacy, cancellation and lifecycle passed.");
}
