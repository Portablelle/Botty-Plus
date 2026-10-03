#pragma once
#include "core.hpp"
#include <regex>
#include <map>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__PS5__) || defined(__APPLE__)
#include <sys/mount.h>
#endif
namespace botty {
// Metadata stays on internal storage. Only payload directories follow the volume.
class Storage {
  Paths internal_;
  std::vector<fs::path> testMounts_;
  mutable std::mutex mutex_;
  std::map<std::string,fs::path> mounts_;
  static bool mounted(const fs::path& p) {
#ifdef __PS5__
    struct statfs s{};
    if(statfs(p.c_str(),&s)||p!=s.f_mntonname||(s.f_flags&MNT_RDONLY))return false;
    const std::string type=s.f_fstypename;
    return type=="exfatfs"||type=="exfat";
#else
    return fs::is_directory(p)&&!fs::is_symlink(fs::symlink_status(p));
#endif
  }
  static std::string identity(const fs::path& p,bool create) {
    const auto file=p/".botty-volume.json";
    if(!fs::exists(fs::symlink_status(file))&&create){
      const auto data=json{{"id","external-"+randomId()}}.dump();
      int fd=open(file.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
      if(fd>=0){const auto n=write(fd,data.data(),data.size());const int rc=fsync(fd);close(fd);if(n!=ssize_t(data.size())||rc)throw std::runtime_error("Cannot identify external storage");}
    }
    containedExisting(p,file);
    const auto id=json::parse(readText(file,256)).at("id").get<std::string>();
    if(!std::regex_match(id,std::regex("external-[a-f0-9]{32}")))throw std::runtime_error("Invalid external volume identity");
    return id;
  }
public:
  void init(const Paths& p,const std::vector<fs::path>& mounts={}){internal_=p;testMounts_=mounts;}
  json list() {
    std::lock_guard<std::mutex> guard(mutex_);
    json out=json::array({{{"id","internal"},{"label","Internal SSD"},{"available",true},{"freeBytes",freeBytes(internal_.root)}}});
    std::vector<fs::path> candidates=testMounts_;
#ifdef __PS5__
    for(int i=0;i<8;++i)candidates.emplace_back("/mnt/usb"+std::to_string(i));
#endif
    std::map<std::string,fs::path> found;
    for(const auto& p:candidates)try{
      if(!mounted(p)||access(p.c_str(),W_OK))continue;
      const auto id=identity(p,true);
      if(found.count(id))throw std::runtime_error("Duplicate external disk identity");
      found[id]=p;
      out.push_back({{"id",id},{"label","External SSD ("+p.filename().string()+")"},{"available",true},{"freeBytes",freeBytes(p)}});
    }catch(...){/* Unavailable/unrecognized media is never an internal fallback. */}
    for(const auto& item:found)mounts_[item.first]=item.second;
    for(const auto& item:mounts_)if(!found.count(item.first))out.push_back({{"id",item.first},{"label","External SSD (disconnected)"},{"available",false},{"freeBytes",0}});
    return out;
  }
  Paths get(const std::string& id,bool create=true) const {
    if(id=="internal"){
      if(create)for(const auto& dir:{internal_.complete,internal_.extracted,internal_.library,internal_.root/"compressor/output",internal_.root/"compressor/originals"}){
        auto current=dir==internal_.library?dir.parent_path():internal_.root;const auto base=current;if(fs::is_symlink(fs::symlink_status(base)))throw std::runtime_error("Internal storage root is a symbolic link");for(const auto& part:dir.lexically_relative(base)){current/=part;if(fs::is_symlink(fs::symlink_status(current)))throw std::runtime_error("Internal storage contains a symbolic link");if(!fs::exists(current))fs::create_directory(current);}
      }
      return internal_;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    auto it=mounts_.find(id);
    if(it==mounts_.end()||!mounted(it->second)||identity(it->second,false)!=id)
      throw std::runtime_error("Selected external disk is disconnected or changed. Reconnect the same disk.");
    const auto mount=it->second;
    Paths p(mount/"botty",mount/"homebrew");p.jobs=internal_.jobs;
    for(const auto& dir:{p.root,p.complete,p.extracted,p.library,p.root/"compressor/output",p.root/"compressor/originals"}){
      if(create){
        // Check every ancestor before mkdir; a link must never redirect payloads.
        auto current=mount;
        for(const auto& part:dir.lexically_relative(mount)){current/=part;if(fs::is_symlink(fs::symlink_status(current)))throw std::runtime_error("External storage contains a symbolic link");if(!fs::exists(current))fs::create_directory(current);}
      }
      if(fs::exists(dir))containedExisting(mount,dir);
    }
    return p;
  }
  std::string forDownload(const fs::path& directory) const {
    if(directory.lexically_normal()==internal_.complete.lexically_normal())return "internal";
    std::vector<std::string> ids;{std::lock_guard<std::mutex> g(mutex_);for(const auto& e:mounts_)ids.push_back(e.first);}
    for(const auto& id:ids)try{if(directory.lexically_normal()==get(id,false).complete.lexically_normal())return id;}catch(...){}
    throw std::runtime_error("Torrent is not on an available Botty disk");
  }
};
// Copy to private staging, flush and compare every byte before publication.
// Callers journal before copying and commit metadata before removing the source.
void copyChecked(const fs::path& source,const fs::path& destination,const std::function<void()>& check={},const DeleteProgress& progress={});
void publishExclusive(const fs::path& source,const fs::path& destination);
void removeTransferred(const fs::path& source);
}
