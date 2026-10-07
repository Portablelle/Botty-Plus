#pragma once
#include "native-update.hpp"
#include <set>

namespace botty {
inline json installationRecord(const fs::path& path,size_t limit) {
  try{return json::parse(nativeRead(path,limit));}
  catch(const std::exception&){throw std::runtime_error("Private installation state is unreadable. Retain files and use Portal recovery.");}
}
inline const std::vector<std::string>& serviceFiles(const std::string& package) {
  static const std::vector<std::string> manager={"botty-manager.elf","icon0.png","ui/index.html","ui/app.js","ui/style.css","cacert.pem","game-compressor.elf"};
  static const std::vector<std::string> engine={"rtorrent.elf","rtorrent.rc","cacert.pem"};
  if(package=="botty")return manager;
  if(package=="rtorrent")return engine;
  throw std::runtime_error("Unexpected installation package.");
}
inline bool installationPathAllowed(const std::string& path) {
  if(path=="botty-release.json"||path=="botty-native/manifest.json"||path=="botty/manifest.json"||path=="rtorrent/manifest.json")return true;
  const auto slash=path.find('/');if(slash==std::string::npos)return false;
  const auto package=path.substr(0,slash),file=path.substr(slash+1);
  if(package=="botty-native")return nativeFileAllowed(file);
  if(package!="botty"&&package!="rtorrent")return false;
  const auto& allowed=serviceFiles(package);return std::find(allowed.begin(),allowed.end(),file)!=allowed.end();
}
inline std::array<unsigned long,4> serviceVersion(const std::string& value,bool engine=false) {
  std::smatch match;
  const std::regex pattern(engine?"([0-9]+)\\.([0-9]+)\\.([0-9]+)-botty([0-9]+)":"([0-9]+)\\.([0-9]+)\\.([0-9]+)");
  if(value.size()>64||!std::regex_match(value,match,pattern))throw std::runtime_error("Invalid service version.");
  std::array<unsigned long,4> parts{};
  for(size_t i=1;i<match.size();++i){parts[i-1]=std::stoul(match[i]);if(parts[i-1]>999999)throw std::runtime_error("Invalid service version.");}
  return parts;
}
inline void validateServiceManifest(const json& manifest,const std::string& package) {
  if(manifest.at("schema")!=1||!manifest.at("files").is_array()||manifest.at("files").size()!=serviceFiles(package).size())throw std::runtime_error("Invalid service manifest.");
  serviceVersion(manifest.at("id").get<std::string>(),package=="rtorrent");
  std::set<std::string> seen;
  for(const auto& file:manifest.at("files")) {
    const auto path=file.at("path").get<std::string>();const auto& allowed=serviceFiles(package);
    if(std::find(allowed.begin(),allowed.end(),path)==allowed.end()||!seen.insert(path).second||!file.at("size").is_number_integer()||file.at("size").get<int64_t>()<1||file.at("size").get<int64_t>()>int64_t(NativeTransaction::maxFileBytes)||!std::regex_match(file.at("sha256").get<std::string>(),std::regex("[a-f0-9]{64}")))throw std::runtime_error("Invalid service manifest file.");
    if(path=="botty-manager.elf"&&file.at("size").get<size_t>()>=16*1024*1024)throw std::runtime_error("Manager exceeds the Portal read bound.");
  }
}
struct InstallationRelease {
  NativeRelease native;
  json manager,engine;
  std::string hash;
  json record() const {return {{"native",native.manifest},{"nativeHash",native.hash},{"manager",manager},{"engine",engine},{"hash",hash}};}
};
inline void validateInstallationCompatibility(const json& native,const json& manager,const json& engine) {
  validateNativeManifest(native);validateServiceManifest(manager,"botty");validateServiceManifest(engine,"rtorrent");
  const auto& requires=native.at("requires");
  if(requires.at("apiVersion")!=1||manager.at("apiVersion")!=1||manager.at("workerApi")!="library-1.3"||manager.at("updaterVersion")!="1.0.0"||serviceVersion(requires.at("manager"))>serviceVersion(manager.at("id"))||serviceVersion(requires.at("worker"))>serviceVersion(manager.at("workerVersion"))||serviceVersion(requires.at("rtorrent"),true)>serviceVersion(engine.at("id"),true))throw std::runtime_error("Installation components are incompatible with this updater API.");
}
inline InstallationRelease installationRelease(const NativeUpdater::Fetch& fetch) {
  const auto index=json::parse(fetch("botty-release.json",256*1024));
  if(index.at("schema")!=1||!index.at("sha256").is_object())throw std::runtime_error("Invalid installation release index.");
  json selected=json::object();std::array<json,3> manifests;std::string nativeDigest;
  const std::array<std::string,3> packages={"botty-native","botty","rtorrent"};
  for(size_t i=0;i<packages.size();++i) {
    const auto path=packages[i]+"/manifest.json",bytes=fetch(path,65536),digest=index.at("sha256").at(path).get<std::string>();
    if(!std::regex_match(digest,std::regex("[a-f0-9]{64}"))||nativeHash(bytes)!=digest)throw std::runtime_error("Installation manifest verification failed; retry after the release settles.");
    manifests[i]=json::parse(bytes);
    if(i==0){validateNativeManifest(manifests[i]);nativeDigest=digest;}else validateServiceManifest(manifests[i],packages[i]);
    selected[path]=digest;
    for(const auto& file:manifests[i].at("files")) {
      const auto source=packages[i]+"/"+file.at("path").get<std::string>();
      if(index.at("sha256").at(source)!=file.at("sha256"))throw std::runtime_error("Installation release digests disagree.");
      selected[source]=file.at("sha256");
    }
  }
  validateInstallationCompatibility(manifests[0],manifests[1],manifests[2]);
  const auto digest=nativeHash(selected.dump());
  return {{manifests[0],nativeDigest,digest},manifests[1],manifests[2],digest};
}
inline void verifyServiceTree(const fs::path& root,const json& manifest,const std::string& package) {
  validateServiceManifest(manifest,package);
  for(const auto& file:manifest.at("files")) {
    const auto bytes=nativeRead(root/file.at("path").get<std::string>(),NativeTransaction::maxFileBytes);
    if(bytes.size()!=file.at("size").get<size_t>()||nativeHash(bytes)!=file.at("sha256"))throw std::runtime_error("Staged service verification failed.");
  }
  for(const auto& entry:fs::recursive_directory_iterator(root)) {
    const auto relative=entry.path().lexically_relative(root).generic_string();const auto status=entry.symlink_status();
    if(fs::is_symlink(status)||(!fs::is_directory(status)&&!fs::is_regular_file(status)))throw std::runtime_error("Unsafe service tree.");
    if(fs::is_directory(status)){if(package!="botty"||relative!="ui")throw std::runtime_error("Unexpected service directory.");}
    else if(!installationPathAllowed(package+"/"+relative))throw std::runtime_error("Unexpected service file.");
  }
}
inline void stageService(const fs::path& root,const json& manifest,const std::string& package,const NativeUpdater::Fetch& fetch) {
  validateServiceManifest(manifest,package);
  if(fs::exists(fs::symlink_status(root))){verifyServiceTree(root,manifest,package);return;}
  for(const auto& file:manifest.at("files")) {
    const auto path=file.at("path").get<std::string>(),bytes=fetch(package+"/"+path,file.at("size").get<size_t>());
    if(bytes.size()!=file.at("size").get<size_t>()||nativeHash(bytes)!=file.at("sha256"))throw std::runtime_error("Service download verification failed.");
    nativeWrite(root/path,bytes,true);
    if(::chmod((root/path).c_str(),path.find(".elf")!=std::string::npos?0755:0644))throw std::runtime_error("Cannot set service permissions.");
  }
  verifyServiceTree(root,manifest,package);
}
}
