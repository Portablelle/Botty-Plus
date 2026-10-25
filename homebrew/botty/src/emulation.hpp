// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.hpp"
#include "native-transaction.hpp"
#include "switch-library.hpp"
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>

namespace botty {
// Eden owns metadata, keys, profiles and saves. Botty only inventories content.
class Emulation {
  fs::path data_, install_;
  struct Cached {std::array<uint64_t,4> identity{},coverIdentity{};json data;std::string pixels,id,artworkId,artworkName,kind;};
  std::shared_ptr<std::mutex> displayLock_=std::make_shared<std::mutex>();
  mutable std::map<std::string,Cached> display_;
  mutable std::string keysIdentity_;
  static json selectEntry(const json& inventory,const std::string& id){
    for(const auto& item:inventory.at("entries"))if(item.at("id")==id)return item;
    throw std::runtime_error("Game no longer available; refresh Emulation");
  }
  fs::path coverPath(const json& item,const fs::path& base) const {
    if(item.at("kind")!="roms")return {};
    const auto relative=fs::path(item.at("path").get<std::string>()).lexically_relative(base/"roms").generic_string();
    uint32_t hash=2166136261u;for(unsigned char c:relative)hash=(hash^c)*16777619u;
    char name[16];std::snprintf(name,sizeof(name),"%08x.tga",hash);
    return data_/"covers"/name;
  }
  struct ModInventory {json entries=json::array();bool known=true;unsigned cheatFiles=0;bool cheatsEnabled=true;};
  ModInventory discoverMods(const fs::path& base,const fs::path& game,const std::string& baseId,const json& config,bool settingsKnown) const {
    ModInventory result;result.known=settingsKnown;
    try {
      json settings=json::object();
      if(config.contains("games")&&config.at("games").contains(baseId))settings=config.at("games").at(baseId);
      const bool enabled=settings.value("mods",true);const auto off=settings.value("mods_off",json::array());
      result.cheatsEnabled=settings.value("cheats",true);
      unsigned visited=0;std::set<std::string> names,cheats;
      const auto visit=[&](){if(++visited>1024)throw std::runtime_error("Mod inventory limit reached");};
      const auto list=[&](const fs::path& directory){
        std::vector<fs::directory_entry> entries;
        if(!fs::exists(directory))return entries;
        for(const auto& entry:fs::directory_iterator(containedExisting(base,directory))){
          visit();if(!entry.is_symlink())entries.push_back(entry);
        }
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.path()<b.path();});
        return entries;
      };
      const auto cheatFolder=[&](const fs::path& folder,const std::string& name){
        const auto files=list(folder);
        for(const auto& file:files)if(file.is_regular_file()&&asciiLower(file.path().extension().string())==".txt")
          cheats.insert(name+"/"+asciiLower(file.path().filename().string()));
        return !files.empty();
      };
      const auto addMods=[&](const fs::path& folder){
        for(const auto& mod:list(folder)){
          const auto name=mod.path().filename().string();if(name=="Cheats folder"||names.count(name))continue;
          bool present=false,hasCheats=false;
          if(mod.is_directory())for(const auto& part:list(mod.path())){
            if(!part.is_directory())continue;
            const auto kind=asciiLower(part.path().filename().string());
            if(kind=="cheats"){hasCheats=cheatFolder(part.path(),name);continue;}
            if(kind!="exefs"&&kind!="romfs"&&kind!="romfs_ext"&&kind!="romfslite")continue;
            for(const auto& file:list(part.path())){
              const auto filename=file.path().filename().string(),ext=file.path().extension().string();
              if(kind!="exefs"||ext==".ips"||ext==".pchtxt"||filename=="main"||filename=="main.npdm"||filename=="rtld"||filename=="sdk"||std::regex_match(filename,std::regex("subsdk[0-9]")))present=true;
            }
          }
          else if(mod.is_regular_file()&&name.rfind("cheat_",0)==0){cheats.insert(name);hasCheats=true;}
          if(!present&&!hasCheats)continue;
          names.insert(name);
          // Cheat-only collections belong to the engine's separate Cheats panel.
          if(!present)continue;
          if(result.entries.size()>=64)throw std::runtime_error("Mod display limit reached");
          bool selected=true;for(const auto& disabled:off)if(disabled==name)selected=false;
          result.entries.push_back({{"name",name},{"enabled",enabled&&selected}});
        }
      };
      // The engine gives a same-name mod in the game's own folder precedence.
      const auto own=game.parent_path();
      if(own!=base/"roms")for(const auto& part:list(own))if(part.is_directory()){
        const auto kind=asciiLower(part.path().filename().string());
        if(kind=="mods")addMods(part.path());
        if(kind=="cheats")cheatFolder(part.path(),"Cheats folder");
      }
      for(const auto& title:list(base/"mods"))
        if(title.is_directory()&&asciiLower(title.path().filename().string())==asciiLower(baseId))addMods(title.path());
      for(const auto& title:list(base/"cheats"))
        if(title.is_directory()&&asciiLower(title.path().filename().string())==asciiLower(baseId))cheatFolder(title.path(),"Cheats folder");
      result.cheatFiles=cheats.size();
    }catch(...){result.known=false;}
    return result;
  }
  void enrich(json& out,const fs::path& base) const {
    std::string keysIdentity;
    const auto keyFile=base/"keys/prod.keys";
    try{keysIdentity=json(sourceIdentity(containedExisting(base,keyFile))).dump();}catch(...){}
    if(keysIdentity!=keysIdentity_){display_.clear();keysIdentity_=keysIdentity;}
    SwitchKeys keys(keysIdentity.empty()?fs::path{}:keyFile);
    std::set<std::string> current;
    for(auto& item:out["entries"]){
      const auto path=item.at("path").get<std::string>();current.insert(path);
      const auto identity=sourceIdentity(path);auto found=display_.find(path);
      if(found==display_.end()||found->second.identity!=identity){
        Cached cached;cached.identity=identity;cached.data=json::object();
        try{cached.data["inventory"]=inspectSwitchDisplayMetadata(path,keys);}catch(...){cached.data["status"]="Content identity unavailable";}
        try{const auto controlNca=cached.data.value("inventory",json::object()).value("controlNca",std::string{});
          const auto display=SwitchLibraryFile(path,keys).display(controlNca);for(auto it=display.begin();it!=display.end();++it)cached.data[it.key()]=it.value();
        }catch(...){cached.data["status"]="Game details unavailable";}
        if(sourceIdentity(path)!=identity){item["library"]=json::object();continue;}
        found=display_.insert_or_assign(path,std::move(cached)).first;
      }
      const auto& d=found->second.data;
      std::array<uint64_t,4> coverIdentity{};
      try{const auto cover=coverPath(item,base);if(!cover.empty()&&fs::exists(cover))coverIdentity=sourceIdentity(containedExisting(data_,cover));}catch(...){}
      if(found->second.coverIdentity!=coverIdentity){found->second.coverIdentity=coverIdentity;found->second.pixels.clear();}
      item["artworkId"]=nativeSha256(path+json(identity).dump()+keysIdentity_+json(coverIdentity).dump());
      found->second.id=item.at("id");found->second.kind=item.at("kind");found->second.artworkId=item.at("artworkId");
      json library=item.contains("metadata")?item.at("metadata"):d.value("inventory",json::object());
      if(d.contains("name")){item["name"]=d.at("name");library["name"]=d.at("name");}
      if(d.contains("versionLabel")&&!d.at("versionLabel").get<std::string>().empty())library["versionLabel"]=d.at("versionLabel");
      item["library"]=library;
      if(d.contains("status"))item["metadataStatus"]=d.at("status");
    }
    for(auto it=display_.begin();it!=display_.end();)if(!current.count(it->first))it=display_.erase(it);else ++it;
    json config=json::object();bool settingsKnown=true;
    try{const auto p=data_/"config/prosperoeden.json";if(fs::exists(p))config=json::parse(readText(containedExisting(data_,p),1024*1024));}catch(...){settingsKnown=false;}
    std::map<std::string,ModInventory> modsByGame;
    for(const auto& item:out["entries"]){const auto& m=item.at("library");
      if(m.value("type",std::string{})!="game")continue;
      const auto baseId=m.value("baseTitleId",std::string{});
      const auto path=item.at("path").get<std::string>();
      modsByGame.emplace(path,discoverMods(base,path,baseId,config,settingsKnown));
    }
    project(out,modsByGame);
    // Receipts and base-game associations can provide a name without a NACP.
    for(const auto& item:out["entries"]){
      const auto found=display_.find(item.at("path").get<std::string>());
      if(found!=display_.end())found->second.artworkName=item.at("name").get<std::string>();
    }
  }
  static void project(json& out,const std::map<std::string,ModInventory>& modsByGame) {
    const auto version=[](const json& m){const auto label=m.value("versionLabel",std::string{});return label.empty()?"revision "+std::to_string(m.value("version",uint64_t(0))):"v"+label;};
    struct Related {
      std::string name,updateVersion;
      bool hasGame=false,hasUpdate=false;
      uint64_t gameRevision=0,updateRevision=0;
      json updates=json::array(),dlc=json::array();
      std::set<std::string> seenDlc;
    };
    std::map<std::string,Related> related;
    // Build each title's relationships once, including bundled content.
    for(const auto& item:out["entries"]){const auto& m=item.at("library");for(const auto& title:m.value("titles",json::array())){
      auto& r=related[title.value("baseTitleId",std::string{})];
      const auto type=title.value("type",std::string{});const auto revision=title.value("version",uint64_t(0));
      if(type=="game"){r.hasGame=true;r.gameRevision=std::max(r.gameRevision,revision);r.name=item.at("name");}
      else if(type=="update"){
        json update=title;update["file"]=fs::path(item.at("path").get<std::string>()).filename().string();
        // A game's NACP describes its base, not a bundled update's display version.
        if(m.value("type",std::string{})=="update"&&m.contains("versionLabel"))update["versionLabel"]=m.at("versionLabel");
        r.updates.push_back(std::move(update));
        if(!r.hasUpdate||revision>r.updateRevision){r.updateRevision=revision;r.updateVersion=m.value("type",std::string{})=="update"?version(m):"revision "+std::to_string(revision);}
        r.hasUpdate=true;
      }else if(type=="dlc"&&r.seenDlc.insert(title.value("titleId",std::string{})).second){
        json entry=title;entry["file"]=fs::path(item.at("path").get<std::string>()).filename().string();entry["unlocker"]=m.value("type",std::string{})=="unlocker";r.dlc.push_back(std::move(entry));
      }
    }}
    for(auto& item:out["entries"]){
      const auto& m=item.at("library");const auto baseId=m.value("baseTitleId",std::string{}),type=m.value("type",std::string{});
      item["titleId"]=baseId;
      if(type.empty()){item["phase"]="Details unavailable | "+fs::path(item.at("path").get<std::string>()).filename().string();item["details"]="Game identity and version could not be read.\nFile: "+fs::path(item.at("path").get<std::string>()).filename().string();continue;}
      const auto& r=related[baseId];
      if(type!="game"){
        if(r.hasGame)item["name"]=r.name;
        std::string phase=(type=="update"?"Update ":type=="unlocker"?"DLC unlocker ":"DLC ")+version(m);
        if(!r.hasGame){item["dependencyStatus"]="base-game-missing";phase+=" | Base game missing";}
        else if(m.value("requiredApplicationVersion",uint64_t(0))>std::max(r.gameRevision,r.updateRevision)){item["dependencyStatus"]="update-required";phase+=" | Required update missing";}
        else {item["dependencyStatus"]="ready";phase+=" | Base game found";}
        item["phase"]=phase;item["details"]=phase+"\nTitle ID: "+m.value("titleId",baseId)+"\nFile: "+fs::path(item.at("path").get<std::string>()).filename().string();continue;
      }
      const auto& additions=r.updates;const auto& dlc=r.dlc;
      const auto& inventory=modsByGame.at(item.at("path").get<std::string>());const auto& mods=inventory.entries;const bool modsKnown=inventory.known;
      unsigned on=0;for(const auto& mod:mods)if(mod.at("enabled")==true)++on;
      std::string phase="Base "+version(m),updateVersion="None detected";
      std::string detail="Title ID: "+baseId+"\nBase: "+version(m);
      if(r.hasUpdate){
        const auto& updateLabel=r.updateVersion;
        updateVersion=updateLabel;phase+=" | Update "+updateLabel;detail+="\nUpdate present: "+updateLabel;
      }else detail+="\nUpdate: none detected";
      phase+=" | "+std::to_string(dlc.size())+" DLC";
      phase+=modsKnown?(mods.empty()?" | No mods":" | Mods "+std::to_string(on)+"/"+std::to_string(mods.size())+" on"):" | Mods unknown";
      detail+="\nDLC: "+std::to_string(dlc.size())+" detected";
      for(const auto& addon:dlc)if(detail.size()<900)detail+="\n  "+addon.at("titleId").get<std::string>()+(addon.at("unlocker")==true?" (unlocker)":"");
      detail+="\nMods: "+(modsKnown?std::to_string(on)+"/"+std::to_string(mods.size())+" switched on":std::string("unavailable"));
      detail+="\nCheat files: "+(modsKnown?std::to_string(inventory.cheatFiles):std::string("unavailable"))+" | All cheats: "+(inventory.cheatsEnabled?"on":"off");
      for(const auto& mod:mods)if(detail.size()<1500)detail+="\n  "+mod.at("name").get<std::string>()+(mod.at("enabled")==true?" (on)":" (off)");
      detail+="\nFile: "+fs::path(item.at("path").get<std::string>()).filename().string();
      item["phase"]=phase;item["details"]=detail;item["updates"]=additions;item["dlc"]=dlc;item["mods"]=mods;item["modsKnown"]=modsKnown;
      item["cheatFileCount"]=inventory.cheatFiles;item["cheatsEnabled"]=inventory.cheatsEnabled;
      item["baseVersion"]=version(m);item["updateVersion"]=updateVersion;
      item["dlcCount"]=dlc.size();item["modCount"]=mods.size();item["modsOn"]=on;
      std::string modSummary;unsigned shown=0;
      for(const auto& mod:mods){if(shown++==3)break;if(!modSummary.empty())modSummary+='\n';modSummary+=mod.at("name").get<std::string>()+(mod.at("enabled")==true?" (on)":" (off)");}
      if(mods.size()>3)modSummary+="\n+ "+std::to_string(mods.size()-3)+" more in Game configuration";
      item["modSummary"]=modSummary;
    }
    std::stable_sort(out["entries"].begin(),out["entries"].end(),[](const json& a,const json& b){return asciiLower(a.at("name").get<std::string>())<asciiLower(b.at("name").get<std::string>());});
  }
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
  json inventory() const {
    json out={{"supported",true},{"installed",false},{"entries",json::array()},{"error",""}};
    try {
      out["installed"]=fs::is_regular_file(fs::symlink_status(install_/"emulation/eboot.bin")) &&
          fs::is_regular_file(fs::symlink_status(install_/"emulation-runtime.json"));
      const auto base=root();out["root"]=base.string();unsigned visited=0;std::map<std::string,bool> batches;
      for(const auto& kind:{"roms","updates"}){
        if(visited>4096||out["entries"].size()>=256)break;
        const auto folder=base/kind;if(!fs::exists(folder))continue;
        containedExisting(base,folder);
        for(auto it=fs::recursive_directory_iterator(folder);it!=fs::recursive_directory_iterator();++it){
          if(++visited>4096){out["error"]="Library scan limit reached; showing the first files";break;}
          if(it.depth()>=8)it.disable_recursion_pending();
          if(it->is_symlink()){it.disable_recursion_pending();continue;}
          if(!it->is_regular_file()||!supported(it->path()))continue;
          if(out["entries"].size()>=256){out["error"]="Library display limit reached (256 files)";break;}
          const auto relative=it->path().lexically_relative(folder);const auto first=relative.begin()->string();
          if(first.rfind(".botty-batch-",0)==0){const auto id=first.substr(13);if(!std::regex_match(id,std::regex("[a-f0-9]{32}")))continue;if(!batches.count(id)){bool complete=false;try{complete=json::parse(readText(containedExisting(base,base/"imports"/(id+".json")),1024*1024)).at("status")=="complete";}catch(...){}batches[id]=complete;}if(!batches[id])continue;}
          try{out["entries"].push_back(entry(base,it->path(),kind));}
          catch(const std::exception&){out["error"]="Some NSP/XCI files could not be read; they were left unchanged";}
        }
      }
    }catch(const std::exception& e){out["error"]=e.what();}
    return out;
  }
  json state() const {
    std::lock_guard<std::mutex> guard(*displayLock_);
    auto out=inventory();
    try{enrich(out,root());}catch(...){out["error"]="Some game details are unavailable; files were left unchanged";}
    return out;
  }
  std::string artworkTitle(const std::string& id) const {
    std::lock_guard<std::mutex> guard(*displayLock_);
    const auto found=std::find_if(display_.begin(),display_.end(),[&](const auto& entry){return entry.second.id==id||entry.second.artworkId==id;});
    if(found==display_.end())return {};
    try{containedExisting(root(),found->first);if(sourceIdentity(found->first)!=found->second.identity)return {};}catch(...){return {};}
    return found->second.artworkName;
  }
  std::string artwork(const std::string& id) const {
    std::lock_guard<std::mutex> guard(*displayLock_);
    auto found=std::find_if(display_.begin(),display_.end(),[&](const auto& entry){return entry.second.id==id||entry.second.artworkId==id;});
    if(found==display_.end())return {};
    const auto path=found->first;fs::path base;
    try{base=root();containedExisting(base,path);if(sourceIdentity(path)!=found->second.identity)return {};
      std::string keysIdentity;const auto key=base/"keys/prod.keys";
      if(fs::exists(key))keysIdentity=json(sourceIdentity(containedExisting(base,key))).dump();
      if(keysIdentity!=keysIdentity_)return {};
    }catch(...){return {};}
    const json item={{"kind",found->second.kind},{"path",path}};
    fs::path cover;std::array<uint64_t,4> coverIdentity{};
    if(item.at("kind")=="roms")try{
      const auto candidate=coverPath(item,root());
      if(fs::exists(candidate)){cover=containedExisting(data_,candidate);coverIdentity=sourceIdentity(cover);}
    }catch(...){}
    if(found->second.coverIdentity!=coverIdentity){found->second.pixels.clear();found->second.coverIdentity=coverIdentity;}
    if(found->second.pixels.empty()){
      std::string pixels;
      if(!cover.empty())try{const auto bytes=readText(cover,2*1024*1024);pixels=switchLibraryPixels({bytes.begin(),bytes.end()});}catch(...){}
      try{SwitchKeys keys(keysIdentity_.empty()?fs::path{}:containedExisting(root(),root()/"keys/prod.keys"));
        const auto controlNca=found->second.data.value("inventory",json::object()).value("controlNca",std::string{});
        if(pixels.empty())pixels=switchLibraryPixels(SwitchLibraryFile(path,keys).icon(controlNca));
        if(sourceIdentity(path)!=found->second.identity)return {};
        // Keep at most 24 decoded covers (~2.7 MiB); names/versions stay cached.
        unsigned count=0;for(auto& cached:display_)if(!cached.second.pixels.empty()&&++count>=24)cached.second.pixels.clear();
        found->second.pixels=std::move(pixels);
      }catch(...){return {};}
    }
    return found->second.pixels;
  }
  std::string launchPath(const std::string& id) const {
    const auto snapshot=inventory();
    if(!snapshot.at("installed").get<bool>())throw std::runtime_error("Install the Botty+ package with its embedded Switch engine");
    const auto base=root();
    if(!fs::exists(base/"keys/prod.keys")||fs::file_size(containedExisting(base,base/"keys/prod.keys"))==0)
      throw std::runtime_error("Provision your private Switch keys from Portal first");
    if(!fs::is_directory(base/"firmware")||fs::is_empty(containedExisting(base,base/"firmware")))
      throw std::runtime_error("Provision your private Switch firmware from Portal first");
    if(!fs::exists(base/"private-runtime.json"))throw std::runtime_error("Private Switch provisioning is incomplete; finish it in Portal");
    const auto item=selectEntry(snapshot,id);
    if(item.at("kind")!="roms")throw std::runtime_error("Only games in the emulator library can be launched");
    const auto path=item.at("path").get<std::string>();
    if(path.size()>=4096)throw std::runtime_error("Game path is too long");
    return path;
  }
  json resolve(const std::string& id) const {
    return selectEntry(inventory(),id);
  }
};
}
