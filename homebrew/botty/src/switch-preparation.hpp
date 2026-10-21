// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "switch-metadata.hpp"
#include "switch-release.hpp"
#include "zip.hpp"
#include "storage.hpp"
namespace botty {
inline void requireSwitchJsonSize(const json& value,size_t limit,const char* message){
 if(value.dump(2).size()+1>limit)throw std::runtime_error(message);
}

inline std::vector<std::string> switchArchiveSets(const json& files){
 std::vector<std::string> out;
 for(const auto& f:files){const auto name=f.at("name").get<std::string>(),lower=asciiLower(name),ext=fs::path(lower).extension().string();
 if(ext==".zip")out.push_back(name);
 else if(ext==".rar"){std::smatch part;
 if(std::regex_search(lower,part,std::regex(R"(\.part([0-9]+)\.rar$)"))&&part[1].str().find_first_not_of('0')!=std::string::npos&&part[1].str().substr(part[1].str().find_first_not_of('0'))!="1")continue;
 out.push_back(name);
 }}
 return out;
}
inline json prepareSwitchFiles(const fs::path& root,const json& files,const fs::path& output,const fs::path& keyFile,const std::string& release,const Reporter& report,const std::function<void()>& externalCheck={}){
 if(!files.is_array()||files.empty()||files.size()>4096)throw std::runtime_error("Switch download inventory exceeds limits");
 std::map<fs::path,std::array<uint64_t,4>> sources;
 std::set<fs::path> volumes;
 const auto identity=[](const fs::path& p){struct stat s{};
 if(::lstat(p.c_str(),&s)||!S_ISREG(s.st_mode)||s.st_size<0)throw std::runtime_error("Switch source is not a regular file");
 return std::array<uint64_t,4>{uint64_t(s.st_dev),uint64_t(s.st_ino),uint64_t(s.st_size),uint64_t(fs::last_write_time(p).time_since_epoch().count())};
 };
 std::set<std::string> names;
 for(const auto& f:files){const auto name=safeRelative(f.at("name").get<std::string>());
 if(!names.insert(name.generic_string()).second)throw std::runtime_error("Duplicate Switch download members");
 const auto path=containedExisting(root,root/name);
 const auto original=identity(path);
 if(original[2]!=f.at("size").get<uint64_t>())throw std::runtime_error("Incomplete Switch download member");
 sources[path]=original;
 volumes.insert(path);
 }
 const auto check=[&]{if(externalCheck)externalCheck();
 for(const auto& e:sources)if(identity(e.first)!=e.second)throw std::runtime_error("Switch source changed while preparing "+e.first.filename().string()+". Wait for the download to finish writing, then choose Prepare again. Original files and partial output are kept.");
 };
 check();
 if(!fs::is_directory(fs::symlink_status(output.parent_path())))throw std::runtime_error("Switch staging parent must be an existing real directory");
 if(!fs::create_directory(output))throw std::runtime_error("Switch staging already exists; inspect the retained partial preparation");
 fs::permissions(output,fs::perms::owner_all);
 struct Candidate{fs::path path;
 bool loose;
 };
 std::vector<Candidate> candidates;
 std::vector<json> notes;
 std::set<fs::path> expanded;
 unsigned archives=0;
 const auto note=[&](const fs::path& p){if(fs::file_size(p)>65536)throw std::runtime_error("NFO exceeds the 64 KiB limit");
 if(notes.size()>=64)throw std::runtime_error("Too many Switch release notes");
 notes.push_back(switchReleaseNotes(readText(p,65536),release));
 };
 std::function<void(const fs::path&,unsigned)> unpack;
 unpack=[&](const fs::path& archive,unsigned depth){check();
 if(depth>3||++archives>32)throw std::runtime_error("Nested archive limit reached");
 if(!expanded.insert(archive).second)return;
 const auto folder=output/("archive-"+std::to_string(archives));
  const auto progress=[&](const Progress& p){check();
 report(p);
 };
  if(asciiLower(archive.extension().string())==".zip")extractZip(archive,folder,progress,[&]{check();return false;});
  else extractRar(archive,folder,progress,"",[&]{check();return false;},0,false,depth==0?volumes:std::set<fs::path>{});
  json extracted=json::array();
 unsigned count=0;
 for(auto it=fs::recursive_directory_iterator(folder);it!=fs::recursive_directory_iterator();++it){if(++count>65536||it.depth()>12||it->is_symlink()||(!it->is_regular_file()&&!it->is_directory()))throw std::runtime_error("Unsafe or excessive extracted Switch files");
 if(!it->is_regular_file())continue;
 const auto ext=asciiLower(it->path().extension().string());
 if(ext==".nsp"||ext==".xci")candidates.push_back({it->path(),false});
 else if(ext==".nfo")note(it->path());
 extracted.push_back({{"name",it->path().lexically_relative(folder).generic_string()}});
 }
  for(const auto& name:switchArchiveSets(extracted))unpack(folder/safeRelative(name),depth+1);
 };
 for(const auto& f:files){const auto path=root/safeRelative(f.at("name"));
 const auto ext=asciiLower(path.extension().string());
 if(ext==".nsp"||ext==".xci")candidates.push_back({path,true});
 else if(ext==".nfo")note(path);
  else if(path.filename().string().size()>11&&path.filename().string().substr(path.filename().string().size()-11)==".botty.json"){
   const auto m=json::parse(readText(path,65536));
   for(const auto& key:{"titleId","name","type","requiredUpdateLabel","requiredFirmwareLabel","versionLabel"})if(m.contains(key)&&!m.at(key).is_string())throw std::runtime_error("Switch sidecar labels must be text");
   json hint={{"announcedTitleId",m.at("titleId")},{"announcedName",m.at("name")},{"announcedType",m.at("type")}};for(const auto& key:{"requiredUpdateLabel","requiredFirmwareLabel","versionLabel"})if(m.contains(key))hint[key]=m.at(key);notes.push_back(hint);
  }
 }
 for(const auto& name:switchArchiveSets(files))unpack(root/safeRelative(name),0);
 if(candidates.empty())throw std::runtime_error("No NSP/XCI found after extraction. NSZ/XCZ, tickets and executable installers are not importable Switch games.");
 if(candidates.size()>256)throw std::runtime_error("Too many Switch containers in one download");
 uint64_t looseBytes=0;for(const auto& candidate:candidates)if(candidate.loose){const auto n=fs::file_size(candidate.path);if(n>UINT64_MAX-looseBytes)throw std::runtime_error("Switch size overflow");looseBytes+=n;}const auto space=freeBytes(output);if(space<512ULL*1024*1024||looseBytes>space-512ULL*1024*1024)throw std::runtime_error("Not enough free space to preserve downloads and import Switch content");
 SwitchKeys keys(keyFile);
 json results=json::array();
 std::set<std::string> targets;
 std::vector<json> inspected;
 for(const auto& candidate:candidates){check();inspected.push_back(inspectSwitchMetadata(candidate.path,keys,report,check));}
 for(const auto& n:notes)if(n.contains("announcedTitleId")&&!std::any_of(inspected.begin(),inspected.end(),[&](const json& m){return switchAnnouncedIdentity(n,m);}))throw std::runtime_error("NFO title ID does not match package metadata; inspect this release before importing");
 size_t candidateIndex=0;
 for(const auto& candidate:candidates){check();
 report({"Identifying Switch game, update or DLC",candidate.path.filename().string(),0,0});
 auto metadata=applySwitchReleaseNotes(inspected.at(candidateIndex++),notes,candidate.path.stem().string());
 metadata["releaseName"]=release;if(!release.empty()&&metadata.value("name",std::string{})==candidate.path.stem().string())metadata["name"]=release;
  const auto ext=asciiLower(candidate.path.extension().string());
 const auto name=metadata.at("titleId").get<std::string>()+"-v"+std::to_string(metadata.at("version").get<uint64_t>())+ext;
 if(!targets.insert(name).second)throw std::runtime_error("Duplicate title/version containers; select one release");
 metadata["filename"]=name;
 results.push_back({{"source",candidate.path.string()},{"loose",candidate.loose},{"metadata",metadata}});
 }
 check();
 fs::create_directories(output/"content/roms");
 fs::create_directories(output/"content/updates");
 json inventory=json::array();
 for(const auto& item:results){const fs::path source=item.at("source").get<std::string>();
 auto metadata=item.at("metadata");
 const std::string name=metadata.at("filename");
 const std::string relative=metadata.at("kind").get<std::string>()+"/"+name;
 const auto target=output/"content"/relative;
  if(item.at("loose").get<bool>())copyChecked(source,target,check,[&](uint64_t bytes,uint64_t total,const std::string& file){report({"Staging verified Switch content; source kept",file,bytes,total});});
 else fs::rename(source,target);
  metadata["verifiedFileSize"]=fs::file_size(target);metadata["verifiedMtime"]=fs::last_write_time(target).time_since_epoch().count();
 requireSwitchJsonSize(metadata,65536,"Switch sidecar metadata exceeds the supported size; shorten release labels");writeJson(target.string()+".botty.json",metadata);
 const auto marked=identity(target);
 inventory.push_back({{"name",relative},{"size",fs::file_size(target)},{"identity",marked},{"metadata",metadata}});
 inventory.push_back({{"name",relative+".botty.json"},{"size",fs::file_size(target.string()+".botty.json")},{"identity",identity(target.string()+".botty.json")}});
 }
 check();
 for(unsigned archive=1;archive<=archives;++archive)deleteGameDirectory(output,"archive-"+std::to_string(archive));
 json receipt={{"schema",1},{"platform","switch"},{"files",inventory}};
 requireSwitchJsonSize(json{{"schema",1},{"status","complete"},{"receipt",receipt}},1024*1024-4096,"Switch batch receipt exceeds the supported size; prepare fewer titles");
 writeJson(output/"prepared.json",receipt);
 report({"Switch formats, metadata and hashes verified","",1,1});
 return receipt;
}
// Flush both data and directory entries before publishing a completion receipt.
inline void flushSwitchPath(const fs::path& path,bool directory=true){
 const int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|(directory?O_DIRECTORY:0));
 if(fd<0)throw std::runtime_error("Cannot open Switch publication for flushing");
 const int result=::fsync(fd);::close(fd);
 if(result)throw std::runtime_error("Cannot flush Switch publication");
}
// Two directory renames with a durable commit receipt. Even a completed receipt
// is revalidated on recovery; an empty reserved target can be safely resumed.
inline json publishSwitchBatch(const fs::path& prepared,const fs::path& emulator,const std::string& id,const std::function<void()>& check={}){
 if(!std::regex_match(id,std::regex("[a-f0-9]{32}")))throw std::runtime_error("Invalid Switch batch identity");
 const auto receipt=json::parse(readText(prepared/"prepared.json",1024*1024));
 if(receipt.at("schema")!=1||receipt.at("platform")!="switch")throw std::runtime_error("Invalid Switch preparation receipt");
 fs::create_directories(emulator/"imports");containedExisting(emulator,emulator/"imports");flushSwitchPath(emulator);
 const auto record=emulator/"imports"/(id+".json");
 if(fs::exists(record)){
  const auto previous=json::parse(readText(record,1024*1024));
  if(previous.at("receipt")!=receipt)throw std::runtime_error("Existing Switch receipt differs; files preserved");
 }else{writeJson(record,{{"schema",1},{"status","pending"},{"receipt",receipt}});flushSwitchPath(record.parent_path());}
 for(const std::string kind:{"roms","updates"}){
  if(check)check();
  fs::create_directories(emulator/kind);containedExisting(emulator,emulator/kind);flushSwitchPath(emulator);
  const auto source=prepared/"content"/kind,target=emulator/kind/(".botty-batch-"+id);
  const auto verify=[&](const fs::path& folder){
   if(!fs::is_directory(fs::symlink_status(folder)))throw std::runtime_error("Switch batch is not a real directory");
   for(const auto& f:receipt.at("files")){
    const auto relative=safeRelative(f.at("name"));if(*relative.begin()!=kind)continue;
    const auto file=folder/relative.filename();struct stat st{};
    if(::lstat(file.c_str(),&st)||!S_ISREG(st.st_mode)||json(std::array<uint64_t,4>{uint64_t(st.st_dev),uint64_t(st.st_ino),uint64_t(st.st_size),uint64_t(fs::last_write_time(file).time_since_epoch().count())})!=f.at("identity"))throw std::runtime_error("Switch batch changed; publication refused and files retained");
    flushSwitchPath(file,false);
   }
   flushSwitchPath(folder);
  };
  if(fs::exists(target)){
   containedExisting(emulator,target);
   if(fs::is_directory(fs::symlink_status(source))&&fs::is_empty(target)){
    // Only an empty reservation belonging to this retained transaction is removed.
    verify(source);if(!fs::remove(target))throw std::runtime_error("Cannot resume Switch destination reservation");
   }
  }
  if(fs::exists(target)){
   verify(target);
   if(fs::exists(source)&&!fs::is_empty(source))throw std::runtime_error("Ambiguous Switch batch publication; copies retained");
  }else if(fs::is_directory(fs::symlink_status(source))){
   verify(source);flushSwitchPath(prepared/"content");flushSwitchPath(prepared);flushSwitchPath(prepared.parent_path());
   publishExclusive(source,target);verify(target);
  }else throw std::runtime_error("Switch batch directory is missing; files preserved for inspection");
  flushSwitchPath(prepared/"content");flushSwitchPath(target.parent_path());
 }
 if(check)check();
 writeJson(record,{{"schema",1},{"status","complete"},{"receipt",receipt}});flushSwitchPath(record.parent_path());
 return receipt;
}
}
