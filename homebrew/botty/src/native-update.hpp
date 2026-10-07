#pragma once
#include "native-transaction.hpp"
#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace botty {
inline std::string nativeDownload(const std::string& path,size_t limit,const fs::path& ca,const std::atomic<bool>* cancelled=nullptr) {
  if(!limit||limit>NativeTransaction::maxFileBytes)throw std::runtime_error("Unexpected native download bound.");
  const std::vector<std::string> services={"botty/manifest.json","botty/botty-manager.elf","botty/icon0.png","botty/ui/index.html","botty/ui/app.js","botty/ui/style.css","botty/cacert.pem","botty/game-compressor.elf","rtorrent/manifest.json","rtorrent/rtorrent.elf","rtorrent/rtorrent.rc","rtorrent/cacert.pem"};
  if(path!="botty-release.json"&&path!="botty-native/manifest.json"&&std::find(services.begin(),services.end(),path)==services.end()&&
     (path.rfind("botty-native/",0)!=0||!nativeFileAllowed(path.substr(13))))throw std::runtime_error("Unexpected installation release path.");
  if(!fs::is_regular_file(fs::symlink_status(ca)))throw std::runtime_error("Manager CA bundle is missing. Update the manager installation before retrying.");
  CURL* raw=curl_easy_init();if(!raw)throw std::runtime_error("Cannot initialize native release check.");
  std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> client(raw,curl_easy_cleanup);
  const auto url="https://raw.githubusercontent.com/portablelle/botty-plus/main/packages/"+path;
  struct Response {std::string bytes;size_t limit;} response{{},limit};
  curl_easy_setopt(raw,CURLOPT_URL,url.c_str());curl_easy_setopt(raw,CURLOPT_PROXY,"");
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(raw,CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(raw,CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
  curl_easy_setopt(raw,CURLOPT_PROTOCOLS,long(CURLPROTO_HTTPS));curl_easy_setopt(raw,CURLOPT_REDIR_PROTOCOLS,long(CURLPROTO_HTTPS));
#endif
  curl_easy_setopt(raw,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(raw,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(raw,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(raw,CURLOPT_CAINFO,ca.c_str());curl_easy_setopt(raw,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(raw,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(raw,CURLOPT_TIMEOUT,120L);
  curl_easy_setopt(raw,CURLOPT_LOW_SPEED_LIMIT,1024L);curl_easy_setopt(raw,CURLOPT_LOW_SPEED_TIME,20L);
  curl_easy_setopt(raw,CURLOPT_NOPROGRESS,0L);
  curl_easy_setopt(raw,CURLOPT_XFERINFODATA,cancelled);
  curl_easy_setopt(raw,CURLOPT_XFERINFOFUNCTION,+[](void* opaque,curl_off_t,curl_off_t,curl_off_t,curl_off_t)->int {
    return opaque&&static_cast<const std::atomic<bool>*>(opaque)->load()?1:0;
  });
  curl_easy_setopt(raw,CURLOPT_WRITEDATA,&response);
  curl_easy_setopt(raw,CURLOPT_WRITEFUNCTION,+[](char* p,size_t n,size_t m,void* opaque)->size_t {
    auto& r=*static_cast<Response*>(opaque);if(n&&m>SIZE_MAX/n)return 0;const auto size=n*m;
    if(size>r.limit-r.bytes.size())return 0;
    r.bytes.append(p,size);return size;
  });
  if(curl_easy_perform(raw)!=CURLE_OK)throw std::runtime_error("Native release download failed. Check HTTPS connectivity and the manager CA bundle.");
  long status=0;curl_easy_getinfo(raw,CURLINFO_RESPONSE_CODE,&status);
  if(status!=200)throw std::runtime_error("Native release source is unavailable. Retry the update check.");
  return response.bytes;
}

struct NativeRelease {json manifest;std::string hash,releaseHash;};
inline NativeRelease nativeRelease(const std::function<std::string(const std::string&,size_t)>& fetch) {
  const auto source=fetch("botty-release.json",256*1024);
  const auto index=json::parse(source);
  if(index.value("schema",0)!=1||!index.contains("sha256")||!index.at("sha256").is_object())throw std::runtime_error("Unexpected native release index.");
  const auto hash=index.at("sha256").at("botty-native/manifest.json").get<std::string>();
  if(!std::regex_match(hash,std::regex("[a-f0-9]{64}")))throw std::runtime_error("Invalid native manifest digest.");
  const auto bytes=fetch("botty-native/manifest.json",65536);
  if(nativeHash(bytes)!=hash)throw std::runtime_error("Native manifest verification failed. The release may be changing; retry later.");
  auto manifest=json::parse(bytes);validateNativeManifest(manifest);
  json selected={{"manifest",hash},{"files",json::object()}};
  for(const auto& file:manifest.at("files")) {
    const auto path="botty-native/"+file.at("path").get<std::string>();
    if(index.at("sha256").at(path)!=file.at("sha256"))throw std::runtime_error("Native release file digests disagree.");
    selected["files"][path]=file.at("sha256");
  }
  return {manifest,hash,nativeHash(selected.dump())};
}

class NativeUpdater {
public:
  using Fetch=std::function<std::string(const std::string&,size_t)>;
  using Gate=std::function<void()>;
  using Notify=std::function<void(const std::string&)>;
private:
  NativeTransaction& transaction_;
  fs::path requestPath_;
  Fetch fetch_;
  Gate idle_,stopped_,unmount_,rescan_;
  Notify notify_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  bool stop_=false,check_=true;
  std::atomic<bool> requested_{false};
  json state_={{"supported",true},{"status","checking"},{"installedVersion",""},{"availableVersion",""},{"message","Checking the native app release."},{"requested",false},{"closeRequired",false}};
  NativeRelease trusted_;
  json durable_;
  std::chrono::steady_clock::time_point nextCheck_{};
  void status(const std::string& value,const std::string& message) {
    std::lock_guard<std::mutex> guard(mutex_);
    state_["status"]=value;state_["message"]=message.substr(0,240);state_["requested"]=requested_.load();state_["closeRequired"]=requested_.load();
  }
  void failed(const std::string& message) {
    bool safe=false;
    bool published=false;
    try{published=!trusted_.hash.empty()&&transaction_.targetComplete(trusted_.manifest,trusted_.hash);}catch(...){}
    try{safe=durable_.contains("previous")&&transaction_.canReleaseRequest(durable_.at("previous").get<std::string>());}catch(...){}
    if(safe) {
      auto failure=durable_;failure["status"]="failed";failure["message"]=message.substr(0,240);
      try{nativeWrite(requestPath_,failure.dump()+"\n");std::lock_guard<std::mutex> guard(mutex_);durable_=failure;requested_=false;trusted_={};}
      catch(...){safe=false;}
    }
    if(safe)status("error",message+" Original app retained. Run a new check to retry.");
    else {
      if(published){std::lock_guard<std::mutex> guard(mutex_);state_["installedVersion"]=trusted_.manifest.at("version");}
      status("blocked",published?"Native files were updated, but final confirmation failed. Keep Botty+ closed. Recheck to retry the scan, or use Portal recovery. "+message:"Native update or recovery is incomplete. Keep Botty+ closed and use Portal recovery. "+message);
      std::lock_guard<std::mutex> guard(mutex_);trusted_={};
    }
    try{if(notify_)notify_(safe?"Botty+: Native update failed. Original app retained. Check updates to retry.":"Botty+: Native update needs Portal recovery. Keep Botty+ closed; retained files were not deleted.");}catch(...){}
  }
  void checkRelease() {
    status("checking","Checking the native app release.");
    auto release=nativeRelease(fetch_);
    std::string installed;
    if(!requested_)installed=transaction_.installedVersion();
    else try{installed=transaction_.installedVersion();}catch(...){}
    std::lock_guard<std::mutex> guard(mutex_);
    if(requested_&&(durable_.at("target")!=release.hash||durable_.at("releaseHash")!=release.releaseHash)){
      if(installed<durable_.at("version").get<std::string>()||!transaction_.targetComplete(release.manifest,release.hash))throw std::runtime_error("The queued release changed. Existing files were preserved; keep Botty+ closed and inspect the update request.");
      auto complete=durable_;complete["status"]="complete";complete["target"]=release.hash;complete["releaseHash"]=release.releaseHash;complete["version"]=installed;
      nativeWrite(requestPath_,complete.dump()+"\n");durable_=complete;requested_=false;
    }
    trusted_=std::move(release);state_["installedVersion"]=installed;state_["availableVersion"]=trusted_.manifest.at("version");
    state_["requested"]=requested_.load();state_["closeRequired"]=requested_.load();
    state_["status"]=requested_?"waiting":installed>=trusted_.manifest.at("version").get<std::string>()?"current":"available";
    state_["message"]=requested_?"Update queued. Close Botty+; existing file operations will finish first.":state_["status"]=="current"?"The native app is up to date.":"A native app update is available.";
  }
  void install() {
    NativeRelease release;{std::lock_guard<std::mutex> guard(mutex_);release=trusted_;}
    if(release.hash.empty())return;
    try {idle_();stopped_();}
    catch(const std::logic_error&){throw;}
    catch(const std::exception& error){status("waiting",error.what());return;}
    const auto gate=[&]{idle_();stopped_();unmount_();stopped_();};
    transaction_.recover(release.manifest,release.hash,gate);
    transaction_.preflight(release.manifest);
    status("installing","Downloading and verifying the native app. Keep Botty+ closed.");
    const bool published=transaction_.targetComplete(release.manifest,release.hash);
    if(!published)transaction_.stage(release.manifest,release.hash,[&](const std::string& path,size_t size){return fetch_("botty-native/"+path,size);});
    const auto current=nativeRelease(fetch_);
    if(current.hash!=release.hash||current.releaseHash!=release.releaseHash)throw std::runtime_error("The release changed during download. Existing files were preserved.");
    if(!published)transaction_.publish(release.manifest,release.hash,gate);
    rescan_();
    const auto version=transaction_.installedVersion();
    if(version!=release.manifest.at("version"))throw std::runtime_error("The installed native version could not be confirmed.");
    auto complete=durable_;complete["status"]="complete";nativeWrite(requestPath_,complete.dump()+"\n");
    {std::lock_guard<std::mutex> guard(mutex_);state_["installedVersion"]=version;durable_=complete;requested_=false;}
    status("complete","Native app update complete. You can reopen Botty+ manually.");
    try{if(notify_)notify_("Botty+: Native app updated to "+version+". You can reopen Botty+.");}catch(...){}
  }
  void run() {
    for(;;) {
      bool checking=false;
      {std::unique_lock<std::mutex> guard(mutex_);wake_.wait_for(guard,std::chrono::seconds(2),[&]{return stop_||check_;});if(stop_)return;checking=check_||(!requested_&&std::chrono::steady_clock::now()>=nextCheck_);check_=false;if(checking)nextCheck_=std::chrono::steady_clock::now()+std::chrono::hours(6);}
      try {if(checking)checkRelease();if(requested_)install();}
      catch(const std::exception& error){if(requested_)failed(error.what());else status("error",error.what());}
    }
  }
public:
  NativeUpdater(NativeTransaction& transaction,fs::path requestPath,Fetch fetch,Gate idle,Gate stopped,Gate unmount,Gate rescan,Notify notify={})
    : transaction_(transaction),requestPath_(std::move(requestPath)),fetch_(std::move(fetch)),idle_(std::move(idle)),stopped_(std::move(stopped)),unmount_(std::move(unmount)),rescan_(std::move(rescan)),notify_(std::move(notify)) {}
  ~NativeUpdater(){ {std::lock_guard<std::mutex> guard(mutex_);stop_=true;wake_.notify_all();}if(thread_.joinable())thread_.join();}
  void start() {
    try {
      if(fs::exists(fs::symlink_status(requestPath_))) {
        durable_=json::parse(nativeRead(requestPath_,65536));
        if(durable_.value("schema",0)!=1||!std::regex_match(durable_.value("target",""),std::regex("[a-f0-9]{64}"))||!std::regex_match(durable_.value("releaseHash",""),std::regex("[a-f0-9]{64}"))||!std::regex_match(durable_.value("previous",""),std::regex("[a-f0-9]{64}"))||!std::regex_match(durable_.value("version",""),std::regex("[0-9]{2}\\.[0-9]{3}\\.[0-9]{3}"))||(durable_.value("status","")!="queued"&&durable_.value("status","")!="complete"&&durable_.value("status","")!="failed"))throw std::runtime_error("Native update request is damaged. Existing files were preserved.");
        requested_=durable_.at("status")=="queued";
      }
      if(transaction_.pendingJournal()&&!requested_)throw std::runtime_error("Pending native publication requires Portal recovery before starting new file operations.");
    }catch(const std::exception& error){requested_=true;check_=false;nextCheck_=std::chrono::steady_clock::time_point::max();status("blocked",error.what());}
    thread_=std::thread([this]{run();});
  }
  bool requested() const {return requested_.load();}
  void requireAdmission() const {if(requested())throw std::runtime_error("Native app update queued. Wait for it to finish before starting another file operation.");}
  json state() const {std::lock_guard<std::mutex> guard(mutex_);return state_;}
  void recheck() {std::lock_guard<std::mutex> guard(mutex_);check_=true;wake_.notify_all();}
  json request() {
    std::lock_guard<std::mutex> guard(mutex_);
    if(requested_) {
      if(durable_.value("status","")!="queued")throw std::runtime_error("Native update state needs inspection before retrying.");
      return {{"apiVersion",1},{"status","queued"},{"version",durable_.at("version")}};
    }
    if(state_.at("status")!="available"||trusted_.hash.empty())throw std::runtime_error("Run an update check and wait for a trusted newer native release.");
    const auto installed=transaction_.installedVersion(),version=trusted_.manifest.at("version").get<std::string>();
    if(version<=installed)throw std::runtime_error("The native app is already current. Downgrade refused.");
    try{transaction_.preflight(trusted_.manifest);}catch(const std::exception& error){state_["status"]="blocked";state_["message"]=std::string(error.what()).substr(0,240);throw;}
    const json queued={{"schema",1},{"status","queued"},{"target",trusted_.hash},{"releaseHash",trusted_.releaseHash},{"version",version},{"previous",transaction_.installedFingerprint()}};
    nativeWrite(requestPath_,queued.dump()+"\n");
    durable_=queued;requested_=true;state_["status"]="waiting";state_["requested"]=true;state_["closeRequired"]=true;state_["message"]="Update queued. Close Botty+; existing file operations will finish first.";wake_.notify_all();
    return {{"apiVersion",1},{"status","queued"},{"version",version}};
  }
};
}
