// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "switch-metadata.hpp"

namespace botty {
// Display data only. Import verification remains owned by switch-metadata.hpp.
// All reads are bounded and offset checked; no whole-ROM reads or content writes.
class SwitchLibraryFile {
  SwitchContainer file_;
  const SwitchKeys& keys_;
public:
  SwitchLibraryFile(const fs::path& file,const SwitchKeys& keys):file_(file),keys_(keys){
    if(file_.contents.size()>512)throw std::runtime_error("Display inventory limit reached");
  }
  std::vector<unsigned char> header(const SwitchMember& entry) const {
    return switchNcaHeader(file_,entry,keys_,true);
  }
  // Returns root Control RomFS files, used for NACP and the embedded JPEG icon.
  std::map<std::string,std::vector<unsigned char>> control(const SwitchMember& entry,
      const std::vector<unsigned char>& h,bool icon) const {
    for(unsigned section=0;section<4;++section){
      const uint64_t start=switchLE(h.data()+0x240+section*16,4)*512,end=switchLE(h.data()+0x244+section*16,4)*512;
      if(!start&&!end)continue;
      if(start<0xc00||end<=start||end>entry.size)throw std::runtime_error("Invalid Control section");
      const auto* fs=h.data()+0x400+section*512;
      if(fs[2]!=0||fs[3]!=3||std::memcmp(fs+8,"IVFC",4))continue;
      if(fs[4]!=1&&fs[4]!=3)continue; // Compressed/indirect files need the engine.
      if(std::any_of(h.begin()+0x230,h.begin()+0x240,[](unsigned char c){return c!=0;}))continue;
      const auto layers=switchLE(fs+0x14,4);
      if(layers<2||layers>7)throw std::runtime_error("Invalid Control IVFC layers");
      const uint64_t rom=switchLE(fs+0x18+(layers-2)*24,8),length=switchLE(fs+0x20+(layers-2)*24,8);
      if(rom>end-start||length>end-start-rom||length<0x50)throw std::runtime_error("Invalid Control RomFS range");
      std::vector<unsigned char> ctr;
      if(fs[4]==3){
        const auto generation=std::max(h[0x206],h[0x220]),index=h[0x207];
        if(index>2)throw std::runtime_error("Unsupported Control key area");
        char name[64];std::snprintf(name,sizeof(name),"key_area_key_%s_%02x",index==0?"application":index==1?"ocean":"system",generation?generation-1:0);
        ctr=switchDecrypt(EVP_aes_128_ecb(),keys_.get(name,16).data(),nullptr,{h.begin()+0x320,h.begin()+0x330});
      }
      const auto read=[&](uint64_t at,size_t size){
        if(at>length||size>length-at||size>2*1024*1024)throw std::runtime_error("Control read exceeds bounds");
        const uint64_t absolute=start+rom+at,aligned=absolute&~uint64_t(15);
        if(fs[4]==1)return file_.read(entry.start+absolute,size);
        const size_t prefix=size_t(absolute-aligned);
        auto bytes=file_.read(entry.start+aligned,size+prefix);std::array<unsigned char,16> iv{};
        for(unsigned k=0;k<8;++k){iv[k]=fs[0x147-k];iv[15-k]=static_cast<unsigned char>((aligned/16)>>(k*8));}
        bytes=switchDecrypt(EVP_aes_128_ctr(),ctr.data(),iv.data(),bytes);bytes.erase(bytes.begin(),bytes.begin()+prefix);return bytes;
      };
      const auto r=read(0,0x50);
      if(switchLE(r.data(),8)!=0x50)throw std::runtime_error("Invalid Control RomFS header");
      const uint64_t table=switchLE(r.data()+0x38,8),tableSize=switchLE(r.data()+0x40,8),data=switchLE(r.data()+0x48,8);
      if(tableSize>128*1024||data>length)throw std::runtime_error("Control table exceeds bounds");
      const auto files=read(table,size_t(tableSize));std::map<std::string,std::vector<unsigned char>> out;
      for(size_t at=0;at<files.size();){
        if(files.size()-at<32)throw std::runtime_error("Truncated Control file table");
        const auto* record=files.data()+at;const auto names=switchLE(record+28,4);
        if(names>512||names>files.size()-at-32)throw std::runtime_error("Invalid Control filename");
        const std::string name(reinterpret_cast<const char*>(record+32),size_t(names));
        if(switchLE(record,4)==0&&(name=="control.nacp"||(icon&&name.rfind("icon_",0)==0&&fs::path(name).extension()==".dat"))){
          const uint64_t offset=switchLE(record+8,8),size=switchLE(record+16,8);
          if(offset>length-data||size>length-data-offset)throw std::runtime_error("Invalid Control file range");
          if(size<=1024*1024&&(name!="control.nacp"||size>=0x3070))out[name]=read(data+offset,size_t(size));
          if(out.size()>18)throw std::runtime_error("Too many Control files");
        }
        at+=(32+size_t(names)+3)&~size_t(3);
      }
      return out;
    }
    return {};
  }
  json display(const std::string& controlNca={}) const {
    json out=json::object();
    for(const auto& member:file_.contents){
      if(fs::path(member.first).extension()!=".nca")continue;
      if(!controlNca.empty()&&member.first!=controlNca)continue;
      const auto h=header(member.second);if(h[0x205]!=2)continue;
      const auto files=control(member.second,h,false);
      const auto nacp=files.find("control.nacp");
      if(nacp==files.end())continue;
      const auto& n=nacp->second;
      const auto text=[](const unsigned char* p,size_t size){const auto* end=static_cast<const unsigned char*>(std::memchr(p,0,size));std::string value(reinterpret_cast<const char*>(p),end?size_t(end-p):size);
        for(unsigned char c:value)if(c<32||c==127)return std::string{};
        try{(void)json(value).dump();}catch(...){return std::string{};}return value;};
      // Prefer an English title consistently with Botty's UI, then any available locale.
      for(unsigned language=0;language<16;++language){const auto name=text(n.data()+language*0x300,0x200);if(!name.empty()){out["name"]=name;break;}}
      out["versionLabel"]=text(n.data()+0x3060,16);out["controlTitleId"]=switchTitleId(switchLE(h.data()+0x210,8));
      break;
    }
    return out;
  }
  std::vector<unsigned char> icon(const std::string& controlNca={}) const {
    for(const auto& member:file_.contents){
      if(fs::path(member.first).extension()!=".nca")continue;
      if(!controlNca.empty()&&member.first!=controlNca)continue;
      const auto h=header(member.second);if(h[0x205]!=2)continue;
      const auto files=control(member.second,h,true);
      for(const auto* language:{"AmericanEnglish","BritishEnglish","French"}){const auto found=files.find(std::string("icon_")+language+".dat");if(found!=files.end())return found->second;}
      for(const auto& file:files)if(file.first.rfind("icon_",0)==0)return file.second;
    }
    return {};
  }
};
std::string switchLibraryPixels(const std::vector<unsigned char>& encoded);
}
