// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.hpp"
#include "native-transaction.hpp"
#include <fstream>
#include <map>
#include <regex>

namespace botty {
// Eden owns metadata, keys, profiles and saves. Botty only inventories content.
class Emulation {
  fs::path data_, install_;
public:
  explicit Emulation(fs::path data="/data/botty/emulation/switch",fs::path install="/data/homebrew/PPSA99071")
      :data_(std::move(data)),install_(std::move(install)){}
  fs::path root() const {
    const auto config=data_/"config/prosperoeden.json";
    const auto document=fs::exists(config)?json::parse(readText(containedExisting(data_,config),1024*1024)):json::object();
    const fs::path selected=document.value("game_files",data_.string());
    if(!selected.is_absolute()||selected==selected.root_path()||selected.lexically_normal()!=selected)
      throw std::runtime_error("Invalid ProsperoEden game files folder");
    fs::path walked;
    for(const auto& part:selected){walked/=part;if(fs::is_symlink(fs::symlink_status(walked)))throw std::runtime_error("Symbolic emulator folders are not supported");}
    return selected;
  }
  static bool supported(const fs::path& path) {
    const auto ext=asciiLower(path.extension().string());return ext==".nsp"||ext==".xci";
  }
  static std::array<uint64_t,4> sourceIdentity(const fs::path& path) {
    struct stat info{};
    if(::lstat(path.c_str(),&info)||!S_ISREG(info.st_mode))throw std::runtime_error("Emulator source is no longer a regular file");
    return {{uint64_t(info.st_dev),uint64_t(info.st_ino),uint64_t(info.st_size),uint64_t(fs::last_write_time(path).time_since_epoch().count())}};
  }
  static void validate(const fs::path& path) {
    if(!supported(path)||!fs::is_regular_file(fs::symlink_status(path)))throw std::runtime_error("Select a regular NSP or XCI file");
    std::ifstream file(path,std::ios::binary);char header[4]{};
    const bool xci=asciiLower(path.extension().string())==".xci";
    if(xci)file.seekg(0x100);
    file.read(header,4);
    if(!file||std::string(header,4)!=(xci?"HEAD":"PFS0"))throw std::runtime_error("Invalid NSP/XCI header; file kept unchanged");
  }
  static json entry(const fs::path& base,const fs::path& file,const std::string& kind) {
    const auto checked=containedExisting(base,file);validate(checked);
    json result={{"id",nativeSha256(kind+":"+checked.string())},{"name",file.stem().string()},
            {"path",checked.string()},{"kind",kind},{"size",fs::file_size(checked)}};
    const auto receipt=fs::path(checked.string()+".botty.json");
    if(fs::exists(receipt)){
      const auto metadata=json::parse(readText(containedExisting(base,receipt),65536));
      if(kind!="download"&&(metadata.value("verified",false)!=true||metadata.value("kind",std::string{})!=kind||metadata.at("verifiedFileSize")!=fs::file_size(checked)||metadata.at("verifiedMtime")!=fs::last_write_time(checked).time_since_epoch().count()))throw std::runtime_error("Verified Switch file changed; prepare it again");
      for(const auto& key:{"name","type","baseTitleId","versionLabel","requiredUpdateLabel","requiredFirmwareLabel"})if(metadata.contains(key)&&!metadata.at(key).is_string())throw std::runtime_error("Invalid Switch metadata text");
      for(const auto& key:{"version","requiredApplicationVersion"})if(metadata.contains(key)&&!metadata.at(key).is_number_unsigned())throw std::runtime_error("Invalid Switch metadata version");
      if(kind!="download"||metadata.contains("titles")){
      if(!metadata.at("titles").is_array())throw std::runtime_error("Invalid Switch metadata titles");
      for(const auto& title:metadata.at("titles")){
        if(!title.is_object())throw std::runtime_error("Invalid Switch title metadata");
        for(const auto& key:{"type","baseTitleId"})if(!title.at(key).is_string())throw std::runtime_error("Invalid Switch title identity");
        if(title.contains("version")&&!title.at("version").is_number_unsigned())throw std::runtime_error("Invalid Switch title version");
      }
      }
      result["metadata"]=metadata;result["name"]=metadata.value("name",file.stem().string());
    }
    return result;
  }
  json state() const {
    json out={{"supported",true},{"installed",false},{"entries",json::array()},{"error",""}};
    try {
      out["installed"]=fs::is_regular_file(fs::symlink_status(install_/"emulation/eboot.bin")) &&
          fs::is_regular_file(fs::symlink_status(install_/"emulation-runtime.json"));
      const auto base=root();out["root"]=base.string();unsigned visited=0;std::map<std::string,bool> batches;
      for(const auto& kind:{"roms","updates"}){
        const auto folder=base/kind;if(!fs::exists(folder))continue;
        containedExisting(base,folder);
        for(auto it=fs::recursive_directory_iterator(folder);it!=fs::recursive_directory_iterator();++it){
          if(++visited>4096){out["error"]="Library scan limit reached; showing the first files";return out;}
          if(it.depth()>=8)it.disable_recursion_pending();
          if(it->is_symlink()){it.disable_recursion_pending();continue;}
          if(!it->is_regular_file()||!supported(it->path()))continue;
          if(out["entries"].size()>=256){out["error"]="Library display limit reached (256 files)";return out;}
          const auto relative=it->path().lexically_relative(folder);const auto first=relative.begin()->string();
          if(first.rfind(".botty-batch-",0)==0){const auto id=first.substr(13);if(!std::regex_match(id,std::regex("[a-f0-9]{32}")))continue;if(!batches.count(id)){bool complete=false;try{complete=json::parse(readText(containedExisting(base,base/"imports"/(id+".json")),1024*1024)).at("status")=="complete";}catch(...){}batches[id]=complete;}if(!batches[id])continue;}
          try{out["entries"].push_back(entry(base,it->path(),kind));}
          catch(const std::exception&){out["error"]="Some NSP/XCI files could not be read; they were left unchanged";}
        }
      }
    }catch(const std::exception& e){out["error"]=e.what();}
    std::map<std::string,uint64_t> games,updates;
    for(const auto& item:out["entries"])if(item.contains("metadata"))for(const auto& m:item.at("metadata").at("titles")){const auto base=m.value("baseTitleId",std::string{});if(m.value("type",std::string{})=="game")games[base]=std::max(games[base],m.value("version",uint64_t(0)));else if(m.value("type",std::string{})=="update")updates[base]=std::max(updates[base],m.value("version",uint64_t(0)));}
    for(auto& item:out["entries"])if(item.contains("metadata")){const auto& m=item.at("metadata");const auto type=m.value("type",std::string{}),base=m.value("baseTitleId",std::string{});std::string phase=type=="game"?"Game":type=="update"?"Game update":type=="unlocker"?"DLC unlocker":"DLC";
      phase+=" | "+(m.contains("versionLabel")?"v"+m.at("versionLabel").get<std::string>():"Version "+std::to_string(m.value("version",uint64_t(0))));
      if(type!="game"){
        if(!games.count(base)){item["dependencyStatus"]="base-game-missing";phase+=" | Add base game "+base+" first";}
        else if(m.value("requiredApplicationVersion",uint64_t(0))>std::max(games[base],updates[base])){item["dependencyStatus"]="update-required";phase+=" | Required game update is missing";}
        else{item["dependencyStatus"]="ready";phase+=" | Base game found";}
        if(m.contains("requiredUpdateLabel"))phase+=" | Requires update v"+m.at("requiredUpdateLabel").get<std::string>();
      }else if(updates.count(base))phase+=" | Update present";
      if(m.contains("requiredFirmwareLabel"))phase+=" | Firmware "+m.at("requiredFirmwareLabel").get<std::string>()+" required";
      item["phase"]=phase;
    }
    return out;
  }
  std::string launchPath(const std::string& id) const {
    const auto snapshot=state();
    if(!snapshot.at("installed").get<bool>())throw std::runtime_error("Install the Botty+ package with its embedded Switch engine");
    const auto base=root();
    if(!fs::exists(base/"keys/prod.keys")||fs::file_size(containedExisting(base,base/"keys/prod.keys"))==0)
      throw std::runtime_error("Provision your private Switch keys from Portal first");
    if(!fs::is_directory(base/"firmware")||fs::is_empty(containedExisting(base,base/"firmware")))
      throw std::runtime_error("Provision your private Switch firmware from Portal first");
    if(!fs::exists(base/"private-runtime.json"))throw std::runtime_error("Private Switch provisioning is incomplete; finish it in Portal");
    const auto item=resolve(id);
    if(item.at("kind")!="roms")throw std::runtime_error("Only games in the emulator library can be launched");
    const auto path=item.at("path").get<std::string>();
    if(path.size()>=4096)throw std::runtime_error("Game path is too long");
    return path;
  }
  json resolve(const std::string& id) const {
    const auto snapshot=state();
    for(const auto& item:snapshot.at("entries"))if(item.at("id")==id)return item;
    throw std::runtime_error("Game no longer available; refresh Emulation");
  }
};
}
