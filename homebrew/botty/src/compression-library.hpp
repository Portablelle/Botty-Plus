#pragma once
#include "core.hpp"
#include "httplib.h"
#include "notification.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <map>
#include <thread>
#include <regex>
#include <fcntl.h>
#include <unistd.h>
namespace botty {
// Exact streaming comparison through the mounted PS5 filesystem. Never follow links.
inline std::map<std::string,uint64_t> compressionFiles(const fs::path& root) {
  if(!fs::is_directory(root)||fs::is_symlink(fs::symlink_status(root)))throw std::runtime_error("Invalid compression source directory");
  std::map<std::string,uint64_t> out;size_t count=0;
  for(const auto& e:fs::recursive_directory_iterator(root)) {
    if(++count>200000)throw std::runtime_error("Too many source entries");
    const auto s=e.symlink_status();
    if(fs::is_symlink(s)||(!fs::is_directory(s)&&!fs::is_regular_file(s)))throw std::runtime_error("Unsupported source entry");
    if(fs::is_regular_file(s))out.emplace(e.path().lexically_relative(root).generic_string(),e.file_size());
  }
  if(out.empty())throw std::runtime_error("Empty game folder");
  return out;
}
inline void compareCompression(const fs::path& original,const fs::path& mounted,const std::function<void(uint64_t,uint64_t)>& progress={}) {
  const auto files=compressionFiles(original);if(files!=compressionFiles(mounted))throw std::runtime_error("Compressed file names or sizes differ from original");
  uint64_t total=0,done=0;for(const auto& e:files){if(e.second>UINT64_MAX-total)throw std::runtime_error("Source too large");total+=e.second;}
  for(const auto& e:files){
    std::ifstream a(containedExisting(original,original/e.first),std::ios::binary),b(containedExisting(mounted,mounted/e.first),std::ios::binary);
    if(!a||!b)throw std::runtime_error("Cannot read game for verification");
    std::vector<char> x(65536),y(65536);uint64_t bytes=0;
    do {a.read(x.data(),x.size());b.read(y.data(),y.size());auto n=a.gcount();if(n!=b.gcount()||memcmp(x.data(),y.data(),size_t(n)))throw std::runtime_error("Compressed content differs from original");bytes+=uint64_t(n);done+=uint64_t(n);if(progress)progress(done,total);}while(a.gcount());
    if(a.bad()||b.bad()||bytes!=e.second)throw std::runtime_error("Game changed or could not be read during verification");
  }
  if(files!=compressionFiles(original)||files!=compressionFiles(mounted))throw std::runtime_error("Game changed during verification");
}
class CompressionLibrary {
  Paths paths_,source_;fs::path shadow_,apps_,runtime_;int port_;
  static void replaceText(const fs::path& p,const std::string& text) {
    fs::create_directories(p.parent_path());const auto tmp=p.string()+".botty-tmp";
    int fd=open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)throw std::runtime_error("Cannot save compression mount settings");
    size_t off=0;while(off<text.size()){auto n=write(fd,text.data()+off,text.size()-off);if(n<=0){close(fd);throw std::runtime_error("Cannot write compression mount settings");}off+=size_t(n);}int rc=fsync(fd);close(fd);if(rc||rename(tmp.c_str(),p.c_str()))throw std::runtime_error("Cannot publish compression mount settings");
  }
  void appendLine(const fs::path& p,const std::string& line) {auto text=fs::exists(p)?readText(p):"";std::istringstream in(text);std::string row;while(std::getline(in,row))if(row==line)return;replaceText(p,text+"\n"+line+"\n");}
public:
  explicit CompressionLibrary(const Paths& p,int port=10101,fs::path shadow="/data/shadowmount",fs::path apps="/user/app",fs::path runtime="/system_ex/app",const Paths* source=nullptr) : paths_(p),source_(source?*source:p),shadow_(std::move(shadow)),apps_(std::move(apps)),runtime_(std::move(runtime)),port_(port) {}
  json api(const std::string& route,const json& data) {
    httplib::Client client("127.0.0.1",port_);client.set_connection_timeout(1);client.set_read_timeout(30);client.set_write_timeout(5);
    auto r=client.Post("/api/v1/"+route,data.dump(),"application/json");
    if(!r)throw std::runtime_error("ShadowMount unavailable; original is kept");
    auto out=json::parse(r->body);if(r->status!=200||out.value("status",-1)!=0)throw std::runtime_error(out.value("error",std::string("Close Botty+ and running games to finish compression")));
    return out;
  }
  bool idle(const std::string& title) {try{api("games/unmount",{{"title_id",title}});return true;}catch(...){return false;}}
  void activate(json& rec,const std::function<void()>& save,const std::function<void(uint64_t,uint64_t)>& progress,const std::function<void(const std::string&)>& notify=notifySystem) {
    const auto title=rec.at("titleId").get<std::string>(),id=rec.at("jobId").get<std::string>();
    if(!std::regex_match(title,std::regex("PPSA[0-9]{5}"))||title=="PPSA99071"||!std::regex_match(id,std::regex("[a-f0-9]{32}")))throw std::runtime_error("Invalid compression identity");
    const fs::path source=rec.at("source").get<std::string>(),image=rec.at("output").get<std::string>();
    if(image!=paths_.root/"compressor/output"/(title+".ffpfsc")||!fs::is_regular_file(containedExisting(paths_.root/"compressor/output",image)))throw std::runtime_error("Unexpected compressed output");
    containedExisting(source_.library,source);compressionFiles(source);
    const auto info=api("games/info",{{"title_id",title}});if(info.value("path","")!=source.string()||info.value("source_type","")!="folder")throw std::runtime_error("ShadowMount source changed; original is kept");
    const auto original=source_.root/"compressor/originals"/id;
    if(fs::exists(fs::symlink_status(original)))throw std::runtime_error("Original backup destination already exists");
    fs::create_directories(original.parent_path());
    rec["originalPath"]=original.string();rec["oldMountLink"]=readText(apps_/title/"mount.lnk",4096);
    rec["oldImageLink"]=fs::exists(apps_/title/"mount_img.lnk")?readText(apps_/title/"mount_img.lnk",4096):"";
    rec["status"]="activating";save(); // Crash journal precedes every source mutation.
    fs::rename(source,original);
    fs::remove(apps_/title/"mount.lnk");fs::remove(apps_/title/"mount_img.lnk");
    appendLine(shadow_/"config.ini","image_ro="+title+".exfat");
    appendLine(shadow_/"autotune.ini","image_sector="+title+".ffpfsc:65536");
    api("manual/add",{{"path",image.string()}});
    bool found=false;
    for(int attempt=0;attempt<60;++attempt){
      try{const auto current=api("games/info",{{"title_id",title}});if(current.value("path","")==image.string()&&current.value("image_backed",false)){found=true;break;}}catch(...){}
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if(!found)throw std::runtime_error("Compressed source scan not confirmed; original backup is kept");
    api("games/mount",{{"title_id",title},{"mode","ro"}});
    rec["status"]="verifying";save();
    compareCompression(original,runtime_/title,progress);
    api("games/unmount",{{"title_id",title}});
    rec["status"]="ready";rec["phase"]="Compressed game ready. Test the game before deleting the original.";rec["originalKept"]=true;rec["verified"]=true;rec["error"]="";save();
    // Notify only after verification, runtime release and the durable ready checkpoint.
    // A failed notification must not turn a successfully verified copy into recovery.
    try {notify("Botty+: Verification complete ("+title+"). You can reopen Botty+. Original kept.");}catch(...) {}
  }
  void restore(json& rec,const std::function<void()>& save) {
    const auto title=rec.at("titleId").get<std::string>(),id=rec.at("jobId").get<std::string>();
    const fs::path original=rec.at("originalPath").get<std::string>(),source=rec.at("source").get<std::string>();
    if(!std::regex_match(id,std::regex("[a-f0-9]{32}"))||!std::regex_match(title,std::regex("PPSA[0-9]{5}"))||title=="PPSA99071"||original!=source_.root/"compressor/originals"/id||source.parent_path()!=source_.library)throw std::runtime_error("Invalid original recovery path");
    containedExisting(original.parent_path(),original);compressionFiles(original);
    if(fs::exists(fs::symlink_status(source)))throw std::runtime_error("Original destination exists; refusing overwrite");
    rec["status"]="activating";save();
    api("manual/remove",{{"path",rec.at("output")}});
    fs::rename(original,source);
    replaceText(apps_/title/"mount.lnk",rec.at("oldMountLink").get<std::string>());
    const auto oldImage=rec.value("oldImageLink","");if(oldImage.empty())fs::remove(apps_/title/"mount_img.lnk");else replaceText(apps_/title/"mount_img.lnk",oldImage);
    api("scan",{{"reset_attempts",false}});
    bool found=false;for(int attempt=0;attempt<60;++attempt){try{if(api("games/info",{{"title_id",title}}).value("path","")==source.string()){found=true;break;}}catch(...){}std::this_thread::sleep_for(std::chrono::seconds(1));}
    if(!found)throw std::runtime_error("Original files restored; ShadowMount scan still needs recovery");
    rec["status"]="restored";rec["verified"]=false;rec["originalKept"]=true;rec["phase"]="Original game restored. Compressed copy retained for inspection.";rec["error"]="";save();
  }
  void removeGame(json& rec,const std::function<void()>& save,const DeleteProgress& deletionProgress={}) {
    const auto title=rec.at("titleId").get<std::string>(),id=rec.at("jobId").get<std::string>();
    if(rec.value("status","")!="ready"||!rec.value("verified",false)||!std::regex_match(id,std::regex("[a-f0-9]{32}"))||!std::regex_match(title,std::regex("PPSA[0-9]{5}"))||title=="PPSA99071")throw std::runtime_error("No verified compressed game available for deletion");
    const fs::path image=rec.at("output").get<std::string>(),original=source_.root/"compressor/originals"/id;
    if(image!=paths_.root/"compressor/output"/(title+".ffpfsc")||!fs::is_regular_file(containedExisting(image.parent_path(),image)))throw std::runtime_error("Invalid compressed image path");
    const auto hashes=fs::path(image.string()+".vhash");
    if(fs::exists(fs::symlink_status(hashes))&&!fs::is_regular_file(containedExisting(image.parent_path(),hashes)))throw std::runtime_error("Invalid compressed verification file");
    const fs::path source=rec.at("source").get<std::string>();
    if(source.parent_path()!=source_.library||fs::exists(fs::symlink_status(source)))throw std::runtime_error("Unexpected Library source; deletion refused");
    const bool kept=rec.value("originalKept",false);
    if(kept){
      if(rec.value("originalPath","")!=original.string())throw std::runtime_error("Invalid original backup path");
      containedExisting(original.parent_path(),original);compressionFiles(original);
      if(json::parse(readText(original/"sce_sys/param.json",65536)).value("titleId","")!=title)throw std::runtime_error("Original title mismatch");
    }else if(fs::exists(fs::symlink_status(original)))throw std::runtime_error("Unexpected original copy; inspect before deleting");
    const auto info=api("games/info",{{"title_id",title}});
    if(info.value("path","")!=image.string()||!info.value("image_backed",false))throw std::runtime_error("Compressed source changed; deletion refused");
    const auto app=apps_/title;
    if(fs::exists(fs::symlink_status(app)))containedExisting(apps_,app);
    for(const auto& name:{"mount.lnk","mount_img.lnk"})if(fs::exists(fs::symlink_status(app/name)))containedExisting(app,app/name);
    if(fs::exists(app/"mount_img.lnk")){std::istringstream input(readText(app/"mount_img.lnk",8192));std::string first;std::getline(input,first);if(first!=image.string())throw std::runtime_error("Image mount link changed; deletion refused");}
    api("games/unmount",{{"title_id",title}}); // Recheck immediately before any mutation.
    rec["status"]="deleting-game";rec["gameDeletionStarted"]=true;rec["phase"]="Deleting compressed game files; saves and archives are kept.";save();
    api("manual/remove",{{"path",image.string()}});
    if(kept){deleteGameDirectory(original.parent_path(),original.filename(),deletionProgress);if(fs::exists(fs::symlink_status(original)))throw std::runtime_error("Original deletion incomplete");}
    downloadedFiles(image.parent_path(),{image.filename()},true,deletionProgress);if(fs::exists(fs::symlink_status(image)))throw std::runtime_error("Compressed image deletion incomplete");
    downloadedFiles(image.parent_path(),{hashes.filename()},true,deletionProgress);if(fs::exists(fs::symlink_status(hashes)))throw std::runtime_error("Verification file deletion incomplete");
    if(fs::exists(app))downloadedFiles(app,{"mount.lnk","mount_img.lnk"},true);
    api("scan",{{"reset_attempts",false}});
    rec["status"]="deleted";rec["verified"]=false;rec["originalKept"]=false;rec["phase"]="Game deleted. Saves, torrents and archives kept.";rec["error"]="";save();
  }
  void removeOriginal(json& rec,const std::function<void()>& save,const DeleteProgress& deletionProgress={}) {
    const auto title=rec.at("titleId").get<std::string>(),id=rec.at("jobId").get<std::string>();
    if(rec.value("status","")!="ready"||!rec.value("originalKept",false))throw std::runtime_error("No original copy available for deletion");
    const fs::path original=rec.at("originalPath").get<std::string>();
    if(!std::regex_match(id,std::regex("[a-f0-9]{32}"))||!std::regex_match(title,std::regex("PPSA[0-9]{5}"))||title=="PPSA99071"||original!=source_.root/"compressor/originals"/id)throw std::runtime_error("Invalid original backup path");
    containedExisting(source_.root/"compressor/originals",original);
    const fs::path image=rec.at("output").get<std::string>();
    if(image!=paths_.root/"compressor/output"/(title+".ffpfsc")||!fs::is_regular_file(containedExisting(image.parent_path(),image)))throw std::runtime_error("Compressed copy is missing or its path changed");
    const auto info=api("games/info",{{"title_id",title}});if(info.value("path","")!=rec.at("output").get<std::string>()||!info.value("image_backed",false))throw std::runtime_error("Compressed source is not selected");
    api("games/unmount",{{"title_id",title}});
    rec["status"]="deleting-original";rec["originalDeletionStarted"]=true;rec["originalKept"]=false;save();
    compressionFiles(original);deleteGameDirectory(original.parent_path(),original.filename(),deletionProgress);if(fs::exists(fs::symlink_status(original)))throw std::runtime_error("Original deletion incomplete");
    rec["status"]="ready";rec["originalKept"]=false;rec["phase"]="Original copy deleted. Compressed copy and archives kept.";save();
  }
};
}
