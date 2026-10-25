// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "emulation.hpp"
#include "../build/forwarder-embedded.hpp"
#include <zlib.h>

namespace botty {
// Only our own forwarders are recognised. Game files and app registration are
// owned by Eden and ShadowMount respectively; neither is modified here.
class SwitchHome {
  fs::path home_,app_,metadata_;
  static bool exists(const fs::path& p) {return fs::symlink_status(p).type()!=fs::file_type::not_found;}
  static void directory(const fs::path& p) {
    if(!fs::is_directory(fs::symlink_status(p)))throw std::runtime_error("Home-screen folder is unavailable or symbolic");
    fs::path walked;for(const auto& part:p){walked/=part;if(fs::is_symlink(fs::symlink_status(walked)))throw std::runtime_error("Symbolic home-screen folders are not supported");}
  }
  static std::string inflate(const unsigned char* bytes,size_t size,unsigned long expected,const char* hash) {
    std::string out(expected,'\0');auto length=expected;
    if(uncompress(reinterpret_cast<Bytef*>(out.data()),&length,bytes,size)!=Z_OK||length!=expected||nativeSha256(out)!=hash)
      throw std::runtime_error("Bundled forwarder template is damaged");
    return out;
  }
  static void write(const fs::path& file,const std::string& bytes,mode_t mode=0644) {
    const int fd=::open(file.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,mode);
    if(fd<0)throw std::runtime_error("Cannot create home-screen shortcut file");
    size_t at=0;bool ok=true;
    while(at<bytes.size()){const auto n=::write(fd,bytes.data()+at,bytes.size()-at);if(n<=0){ok=false;break;}at+=size_t(n);}
    if(::fchmod(fd,mode)||::fsync(fd))ok=false;
    if(::close(fd))ok=false;
    if(!ok)throw std::runtime_error("Cannot save home-screen shortcut file");
  }
  static void bigEndian(std::string& out,uint32_t n) {for(int shift=24;shift>=0;shift-=8)out+=char(n>>shift);}
  static void chunk(std::string& out,const std::string& kind,const std::string& data) {
    bigEndian(out,uint32_t(data.size()));out+=kind;out+=data;
    auto crc=crc32(0,reinterpret_cast<const Bytef*>(kind.data()),4);
    crc=crc32(crc,reinterpret_cast<const Bytef*>(data.data()),data.size());bigEndian(out,uint32_t(crc));
  }
public:
  explicit SwitchHome(fs::path home="/data/homebrew",fs::path app="/data/homebrew/PPSA99071",fs::path metadata="/system_data/priv/appmeta")
    :home_(std::move(home)),app_(std::move(app)),metadata_(std::move(metadata)){}
  static bool validEntry(const std::string& id) {return std::regex_match(id,std::regex("[a-f0-9]{64}"));}
  // A 512-square PNG, with the existing portrait fitted without distortion.
  static std::string icon(const std::string& pixels) {
    if(pixels.size()!=160*240*3)return {};
    std::string rows;rows.reserve(512*(1+512*3));
    for(unsigned y=0;y<512;++y){rows+='\0';for(unsigned x=0;x<512;++x)for(unsigned c=0;c<3;++c)
      rows+=x>=85&&x<426?pixels[((y*240/512)*160+(x-85)*160/341)*3+c]:char(12);}
    uLongf length=compressBound(rows.size());std::string compressed(length,'\0');
    if(compress2(reinterpret_cast<Bytef*>(compressed.data()),&length,reinterpret_cast<const Bytef*>(rows.data()),rows.size(),6)!=Z_OK)
      throw std::runtime_error("Cannot encode game icon");
    compressed.resize(length);std::string out("\x89PNG\r\n\x1a\n",8),header;
    bigEndian(header,512);bigEndian(header,512);header.append("\x08\x02\0\0\0",5);
    chunk(out,"IHDR",header);chunk(out,"IDAT",compressed);chunk(out,"IEND",{});return out;
  }
  std::map<std::string,std::string> titles() const {
    std::map<std::string,std::string> out;
    if(!exists(home_))return out;
    directory(home_);unsigned visited=0;
    for(const auto& item:fs::directory_iterator(home_)){
      if(++visited>4096)throw std::runtime_error("Home-screen scan limit reached");
      const auto id=item.path().filename().string();
      if(!std::regex_match(id,std::regex("PPSA99[2-8][0-9]{2}"))||!fs::is_directory(item.symlink_status()))continue;
      try{const auto config=json::parse(readText(containedExisting(home_,item.path()/"forwarder.json"),16384));
        const auto entry=config.value("bottyEntry",std::string{});
        if(validEntry(entry)&&config.value("format","")=="ps5-forwarder/1"&&config.value("target","")=="PPSA99071"&&
           config.at("args")==json::array({"--botty-home-game",entry}))out.emplace(entry,id);
      }catch(...){}
    }
    return out;
  }
  void enrich(json& state) const {
    state["homeScreenSupported"]=true;
    try{const auto known=titles();
      for(auto& item:state["entries"])if(item.at("kind")=="roms"){
        const auto found=known.find(item.at("id").get<std::string>());item["homeTitleId"]=found==known.end()?"":found->second;
      }
    }catch(...){state["homeScreenSupported"]=false;}
  }
  json add(Emulation& emulator,const std::string& entry) const {
    if(!validEntry(entry))throw std::runtime_error("Invalid Switch game identity");
    // Resolve and validate again: never accept a path, executable or title ID from clients.
    (void)emulator.launchPath(entry);
    directory(home_);directory(app_);
    const auto known=titles();const auto prior=known.find(entry);
    if(prior!=known.end())return {{"ok",true},{"titleId",prior->second},{"existing",true}};
    const auto state=emulator.state();json selected;
    for(const auto& item:state.at("entries"))if(item.at("id")==entry)selected=item;
    if(selected.is_null()||selected.at("kind")!="roms")throw std::runtime_error("This Switch game is no longer available");
    auto picture=icon(emulator.artwork(entry));
    if(picture.empty())picture=readText(containedExisting(app_,app_/"sce_sys/icon0.png"),2*1024*1024);
    const auto program=inflate(forwarderProgram,sizeof(forwarderProgram),forwarderProgramSize,forwarderProgramHash);
    const auto runtime=inflate(forwarderRuntime,sizeof(forwarderRuntime),forwarderRuntimeSize,forwarderRuntimeHash);
    std::string id;
    // Leave RetroArch's range and upstream's reserved Porpoise ID alone. Existing
    // folders and registered metadata, including stale registrations, reserve IDs.
    for(unsigned number=99270;number<=99899;++number){
      const auto candidate="PPSA"+std::to_string(number);if(number==99764)continue;
      if(!exists(home_/candidate)&&!exists(metadata_/candidate)&&!exists("/user/appmeta"/fs::path(candidate))){id=candidate;break;}
    }
    if(id.empty())throw std::runtime_error("No unused home-screen shortcut identity remains");
    const auto stage=home_/(".botty-home-"+randomId());
    if(!fs::create_directory(stage))throw std::runtime_error("Cannot stage home-screen shortcut");
    try{
      fs::permissions(stage,fs::perms(0755));fs::create_directory(stage/"sce_sys");fs::permissions(stage/"sce_sys",fs::perms(0755));
      fs::create_directory(stage/"sce_module");fs::permissions(stage/"sce_module",fs::perms(0755));
      write(stage/"eboot.bin",program,0755);write(stage/"sce_module/libc.prx",runtime,0755);
      json config={{"format","ps5-forwarder/1"},{"target","PPSA99071"},{"args",json::array({"--botty-home-game",entry})},{"creator","Botty+"},{"bottyEntry",entry}};
      write(stage/"forwarder.json",config.dump(2)+"\n");
      auto param=json::parse(forwarderParam);param["titleId"]=id;param["conceptId"]=id.substr(4);param["contentId"]="UP9000-"+id+"_00-PS5FORWARDER0000";
      param["localizedParameters"]["en-US"]["titleName"]=selected.at("name");
      write(stage/"sce_sys/param.json",param.dump(2)+"\n");write(stage/"sce_sys/icon0.png",picture);
      // Claim the otherwise unused ID with mkdir (exclusive). The complete tree
      // replaces only this empty reservation, never another application's tree.
      if(exists(metadata_/id)||exists("/user/appmeta"/fs::path(id))||!fs::create_directory(home_/id))
        throw std::runtime_error("Shortcut identity was claimed; try again");
      fs::rename(stage,home_/id);
    }catch(...){fs::remove_all(stage);throw;}
    return {{"ok",true},{"titleId",id},{"existing",false}};
  }
};
}
