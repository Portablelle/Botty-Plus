// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.hpp"
#include <regex>
namespace botty {
// Scene descriptions are declarations, never proof of a container or commands
// to execute. Match their bounded fields against verified CNMT identities.
inline json switchReleaseNotes(std::string text,const std::string& fallback={}){
 if(text.size()>65536)text.resize(65536);
 text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return c<32&&c!='\n'&&c!='\r'&&c!='\t';}),text.end());
 json out=json::object();
 std::smatch match;
 if(std::regex_search(text,match,std::regex(R"(\bTitle\s*:\s*([^\r\n|]{1,200}))",std::regex::icase)))out["announcedName"]=match[1].str();
 const auto title=out.value("announcedName",fallback);
 if(std::regex_search(title,std::regex(R"(\bDLC[ ._-]+Unlocker\b)",std::regex::icase)))out["announcedType"]="unlocker";
 else if(std::regex_search(title,std::regex(R"(\bDLC\b)",std::regex::icase)))out["announcedType"]="dlc";
 else if(std::regex_search(title,std::regex(R"(\bUpdate\b)",std::regex::icase)))out["announcedType"]="update";
 if(std::regex_search(text,match,std::regex(R"(Title\s*ID\s*:\s*([0-9a-fx]{16}))",std::regex::icase))){auto id=match[1].str();
 std::transform(id.begin(),id.end(),id.begin(),[](unsigned char c){return std::toupper(c);});
 out["announcedTitleId"]=id;
 }
 std::string plain=text;
 std::replace(plain.begin(),plain.end(),'|',' ');
 plain=std::regex_replace(plain,std::regex(R"(\s+)")," ");
 if(std::regex_search(plain,match,std::regex(R"(Update\s+v([0-9]+(?:\.[0-9]+){1,3})\s+is\s+required)",std::regex::icase)))out["requiredUpdateLabel"]=match[1].str();
 if(std::regex_search(plain,match,std::regex(R"(Firmware\s+v?([0-9]+(?:\.[0-9]+){1,3}).{0,120}\brequired)",std::regex::icase)))out["requiredFirmwareLabel"]=match[1].str();
 if(std::regex_search(title,match,std::regex(R"(\bUpdate\s+v([0-9]+(?:\.[0-9]+){1,3}))",std::regex::icase)))out["versionLabel"]=match[1].str();
 return out;
}
inline bool switchAnnouncedIdentity(const json& notes,const json& metadata){
 if(!notes.contains("announcedTitleId"))return false;
 const auto expected=notes.at("announcedTitleId").get<std::string>();
 for(const auto& title:metadata.at("titles")){const auto actual=title.at("titleId").get<std::string>();
 bool same=expected.size()==actual.size();
 for(size_t i=0;same&&i<expected.size();++i)same=expected[i]=='X'||expected[i]==actual[i];
 if(same)return true;
 }
 return false;
}
inline json applySwitchReleaseNotes(json metadata,const std::vector<json>& notes,const std::string& fallback){
 json selected=switchReleaseNotes("",fallback);
 bool matched=false;
 for(const auto& n:notes)if(switchAnnouncedIdentity(n,metadata)){selected=n;
 matched=true;
 break;
 }
 if(!matched&&notes.size()==1){if(!notes[0].contains("announcedTitleId"))selected=notes[0];
 }
 const auto type=metadata.at("type").get<std::string>(),declared=selected.value("announcedType",std::string{});
 if(!declared.empty()&&declared!="unlocker"&&declared!=type)throw std::runtime_error("NFO content type differs from package metadata; inspect this release before importing");
 if(declared=="unlocker"){if(type!="dlc")throw std::runtime_error("Announced DLC unlocker is not DLC metadata; package kept for inspection");
 metadata["type"]="unlocker";
 metadata["metadataType"]="dlc";
 }
 for(const auto& key:{"requiredUpdateLabel","requiredFirmwareLabel","versionLabel"})if(selected.contains(key))metadata[key]=selected.at(key);
 metadata["name"]=selected.value("announcedName",fallback);
 metadata["notesMatchedByTitleId"]=matched;
 if(metadata.at("type")=="unlocker")metadata["notice"]="DLC unlocker: requires the base game and the content/update it unlocks. It is not a complete game.";
 return metadata;
}
}
