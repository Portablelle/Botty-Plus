#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace botty {
// PS5 uses 16-bit wchar_t. Avoid the runtime's incompatible codecvt facet;
// explicitly handle UTF-16 pairs and UTF-32 scalars without locale conversion.
template<class Wide> std::string wideToUtf8(const Wide* value, size_t capacity) {
  std::string out;
  for(size_t i=0;i<capacity;++i) {
    uint32_t cp=uint32_t(value[i]);
    if(cp==0)return out;
    if(cp>=0xd800 && cp<=0xdbff) {
      if(++i>=capacity || uint32_t(value[i])<0xdc00 || uint32_t(value[i])>0xdfff)
        throw std::runtime_error("Invalid Unicode archive filename");
      cp=0x10000+((cp-0xd800)<<10)+(uint32_t(value[i])-0xdc00);
    }
    if(cp>0x10ffff || (cp>=0xdc00 && cp<=0xdfff))
      throw std::runtime_error("Invalid Unicode archive filename");
    if(cp<0x80)out.push_back(char(cp));
    else {
      if(cp<0x800)out.push_back(char(0xc0|(cp>>6)));
      else {
        if(cp<0x10000)out.push_back(char(0xe0|(cp>>12)));
        else {out.push_back(char(0xf0|(cp>>18)));out.push_back(char(0x80|((cp>>12)&63)));}
        out.push_back(char(0x80|((cp>>6)&63)));
      }
      out.push_back(char(0x80|(cp&63)));
    }
  }
  throw std::runtime_error("Unterminated archive filename");
}

template<class Wide> std::basic_string<Wide> utf8ToWide(std::string_view value) {
  std::basic_string<Wide> out;
  const auto invalid=[](){throw std::runtime_error("Password must contain valid UTF-8 characters");};
  for(size_t i=0;i<value.size();) {
    uint32_t cp=static_cast<unsigned char>(value[i++]);unsigned extra=0;uint32_t minimum=0;
    if(cp<0x80){if(cp==0)invalid();}
    else if(cp>=0xc2 && cp<=0xdf){cp&=31;extra=1;minimum=0x80;}
    else if(cp>=0xe0 && cp<=0xef){cp&=15;extra=2;minimum=0x800;}
    else if(cp>=0xf0 && cp<=0xf4){cp&=7;extra=3;minimum=0x10000;}
    else invalid();
    if(extra>value.size()-i)invalid();
    for(unsigned n=0;n<extra;++n){unsigned byte=static_cast<unsigned char>(value[i++]);if((byte&0xc0)!=0x80)invalid();cp=(cp<<6)|(byte&63);}
    if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))invalid();
    if constexpr(sizeof(Wide)==2) {
      if(cp>=0x10000){cp-=0x10000;out.push_back(Wide(0xd800+(cp>>10)));out.push_back(Wide(0xdc00+(cp&1023)));continue;}
    }
    out.push_back(Wide(cp));
  }
  return out;
}
}
