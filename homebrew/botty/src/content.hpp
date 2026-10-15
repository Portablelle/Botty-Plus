#pragma once
#include "core.hpp"
#include <regex>
namespace botty {
inline bool packageName(const std::string& name) {
  const auto lower=asciiLower(name),ext=fs::path(lower).extension().string();
  return ext==".pkg"||ext==".fpkg"||std::regex_search(lower,std::regex("(^|[^a-z0-9])fpkg([^a-z0-9]|$)"));
}
inline bool packageTorrent(const json& torrent) {
  if(packageName(torrent.value("name",std::string{})))return true;
  for(const auto& f:torrent.value("files",json::array()))if(packageName(f.at("name")))return true;
  return false;
}
inline const char* packagePolicyMessage(){return "PKG/FPKG downloads are not supported on PS5. Choose Server for conversion to exFAT.";}
inline std::vector<std::string> archiveSets(const json& torrent) {
  std::vector<std::string> result;
  for(const auto& f:torrent.at("files")){
    const std::string name=f.at("name");if(asciiLower(fs::path(name).extension().string())!=".rar")continue;
    std::smatch part;
    if(std::regex_search(name,part,std::regex("\\.part([0-9]+)\\.rar$",std::regex::icase))&&(part[1].str().find_first_not_of('0')==std::string::npos||part[1].str().substr(part[1].str().find_first_not_of('0'))!="1"))continue;
    result.push_back(name);
  }
  return result;
}
// Announcements are hints, not executable instructions or proof of a format.
inline std::string releaseFormatHint(const std::string& name,const std::string& nfo={}) {
  if(packageName(name))return "FPKG announced; Server required";
  const auto notes=asciiLower(nfo);
  if(notes.find("file/folder format")!=std::string::npos||notes.find("folder dump")!=std::string::npos)return "Folder announced in NFO; content not yet confirmed";
  const auto lower=asciiLower(name);
  if(lower.find("exfat")!=std::string::npos)return "exFAT announced; content not yet confirmed";
  return "Format will be confirmed from the files";
}
// Classify only members belonging to this torrent, never adjacent downloads.
json classifyDownload(const fs::path& root,const json& files,const std::string& marker={});
}
