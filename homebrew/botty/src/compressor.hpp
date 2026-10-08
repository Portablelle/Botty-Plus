#pragma once
#include "core.hpp"
#include <atomic>
#include "progress.hpp"
#include "storage.hpp"
#include "compression-library.hpp"
#include <curl/curl.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <regex>
#include <sys/stat.h>

namespace botty {
// Worker copies only; this service journals activation and verifies mounted bytes.
// Interrupted source mutations remain locked for explicit recovery.
class Compressor {
  Paths paths_;
  Storage* storage_=nullptr;
  Paths disk(const std::string& id) const {if(storage_)return storage_->get(id);if(id!="internal")throw std::runtime_error("External storage unavailable");return paths_;}
  int port_=5910,shadowPort_=10101;
  bool enabled_=false;
  std::atomic<bool> skipVerification_{false};
  json records_=json::object();
  mutable std::mutex mutex_;
  json state_={{"status","idle"}};
  ExtractionEstimate estimate_;
  std::string estimateKey_;
  std::chrono::steady_clock::time_point phaseStarted_{},measuredAt_{};
  void measure() {
    const auto key=state_.value("jobId","")+state_.value("status","")+state_.value("phase","")+state_.value("unit","bytes");
    const auto now=std::chrono::steady_clock::now();measuredAt_=now;
    if(key!=estimateKey_){estimateKey_=key;estimate_=ExtractionEstimate{};phaseStarted_=now;}
    const auto status=state_.value("status","");
    const bool measurable=status=="running"||status=="verifying"||status=="deleting-original"||status=="deleting-game";
    if(measurable)estimate_.update(now,state_.value("bytes",uint64_t(0)),state_.value("total",uint64_t(0)));
    state_["rate"]=measurable?estimate_.rate:0;state_["eta"]=measurable?estimate_.eta:-1;
    state_["elapsed"]=std::chrono::duration<double>(now-phaseStarted_).count();
  }
  fs::path record() const {return paths_.root/"compressor/state.json";}
  static bool pending(const json& s) {
    const auto status=s.value("status","");
    return status=="starting"||status=="running"||status=="uncertain"||status=="waiting-close"||status=="activating"||status=="verifying"||status=="deleting-original"||status=="deleting-game";
  }
  static long long now() {return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());}
  void save() {
    measure();
    writeJson(record(),state_);
    if(state_.contains("jobId")){records_[state_.at("jobId").get<std::string>()]=state_;writeJson(paths_.root/"compressor/games.json",records_);}
  }
  json request(const std::string& path,bool post=false) const {
    auto key=readText(paths_.root/"compressor/token",128);
    while(!key.empty()&&(key.back()=='\n'||key.back()=='\r'))key.pop_back();
    if(!std::regex_match(key,std::regex("[a-f0-9]{64}")))throw std::runtime_error("Invalid compressor credentials");
    const auto url="http://127.0.0.1:"+std::to_string(port_)+path+(path.find('?')==std::string::npos?"?":"&")+"token="+key;
    CURL* raw=curl_easy_init();if(!raw)throw std::runtime_error("Cannot initialize compressor connection");
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> client(raw,curl_easy_cleanup);
    std::string body;
    curl_easy_setopt(raw,CURLOPT_URL,url.c_str());curl_easy_setopt(raw,CURLOPT_PROXY,"");
    curl_easy_setopt(raw,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(raw,CURLOPT_CONNECTTIMEOUT_MS,500L);curl_easy_setopt(raw,CURLOPT_TIMEOUT_MS,1500L);
    curl_easy_setopt(raw,CURLOPT_POST,post?1L:0L);
    if(post)curl_easy_setopt(raw,CURLOPT_POSTFIELDS,"");
    curl_easy_setopt(raw,CURLOPT_WRITEDATA,&body);
    curl_easy_setopt(raw,CURLOPT_WRITEFUNCTION,+[](char* p,size_t n,size_t m,void* opaque)->size_t {
      auto& b=*static_cast<std::string*>(opaque);if(n&&m>SIZE_MAX/n)return 0;const auto bytes=n*m;
      if(bytes>2*1024*1024-b.size())return 0;b.append(p,bytes);return bytes;
    });
    if(curl_easy_perform(raw)!=CURLE_OK)throw std::runtime_error("Compression worker unavailable; do not repeat an uncertain request");
    long status=0;curl_easy_getinfo(raw,CURLINFO_RESPONSE_CODE,&status);
    auto data=json::parse(body);
    if(status!=200||!data.value("ok",false))throw std::runtime_error(data.value("error","Compression worker rejected the request"));
    return data;
  }
  static std::string encode(const std::string& s) {
    const char* h="0123456789ABCDEF";std::string out;
    for(unsigned char c:s)if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')out+=c;else {out+='%';out+=h[c>>4];out+=h[c&15];}
    return out;
  }
  void cleanFailedCopy(const std::string& id,const std::string& title,const fs::path& source,const fs::path& output,const Paths& sourcePaths,const std::string& selected) {
    const auto directory=output.parent_path();
    if(!fs::exists(fs::symlink_status(directory)))return;
    containedExisting(output.parent_path().parent_path().parent_path(),directory);
    const fs::path temporary="."+output.filename().string()+".gc-compress.tmp";
    const std::vector<fs::path> files={temporary,temporary.string()+".vhash"};
    bool found=false;
    for(const auto& name:files)found=found||fs::exists(fs::symlink_status(directory/name));
    if(!found)return;
    const auto previous=records_.value(id,json::object());
    const auto status=previous.value("status","");
    if((status!="failed"&&status!="cancelled")||previous.value("jobId","")!=id||
       previous.value("titleId","")!=title||previous.value("source","")!=source.string()||previous.value("storage","internal")!=selected||
       !previous.value("originalKept",false)||previous.value("verified",false)||
       previous.contains("originalPath")||previous.value("originalDeletionStarted",false)||previous.value("gameDeletionStarted",false))
      throw std::runtime_error("Untracked compression files require inspection before retrying");
    if(fs::exists(fs::symlink_status(sourcePaths.root/"compressor/originals"/id)))
      throw std::runtime_error("An original backup exists; recover the Library operation before retrying");
    // Validate both names before removing either; descriptor-relative deletion
    // rejects symlinks and special files and never follows a replaced path.
    downloadedFiles(directory,files,false);
    downloadedFiles(directory,files,true);
    for(const auto& name:files)if(fs::exists(fs::symlink_status(directory/name)))
      throw std::runtime_error("Could not remove unfinished compression files");
  }
public:
  void init(const Paths& paths,int port=5910,Storage* storage=nullptr,int shadowPort=10101) {
    shadowPort_=shadowPort;storage_=storage;paths_=paths;port_=port;
    if(!fs::exists(paths.root/"compressor/enabled.json"))return;
    const auto config=json::parse(readText(paths.root/"compressor/enabled.json",4096));
    enabled_=config.value("mode","")=="library-1.2";
    if(enabled_&&fs::exists(record()))state_=json::parse(readText(record(),65536));
    if(enabled_&&fs::exists(paths_.root/"compressor/games.json"))records_=json::parse(readText(paths_.root/"compressor/games.json",4*1024*1024));
    if(enabled_&&(state_.value("status","")=="activating"||state_.value("status","")=="verifying"||state_.value("status","")=="deleting-original"||state_.value("status","")=="deleting-game")){state_["status"]="uncertain";state_["error"]="Interrupted Library operation. Original backup and compressed image require recovery.";save();}
  }
  bool enabled() const {return enabled_;}
  bool busy() const {std::lock_guard<std::mutex> g(mutex_);return enabled_&&pending(state_);}
  void requireIdle() const {if(busy())throw std::runtime_error("Compression is active or uncertain; wait before changing files");}
  void confirmNativeUpdateIdle() const {
    const auto safe=[](const json& record) {
      const auto status=record.value("status","");
      if(status!="idle"&&status!="ready"&&status!="restored"&&status!="deleted"&&status!="failed"&&status!="cancelled")return false;
      if((status=="failed"||status=="cancelled")&&(record.contains("originalPath")||record.value("originalDeletionStarted",false)||record.value("gameDeletionStarted",false)))return false;
      return true;
    };
    {
      std::lock_guard<std::mutex> g(mutex_);
      if(!enabled_)throw std::logic_error("Compression worker idle confirmation is unavailable. Check the worker installation before updating.");
      if(state_.value("status","")=="uncertain"||(!safe(state_)&&!pending(state_)))throw std::logic_error("Resolve the protected compression state before updating Botty+.");
      if(!safe(state_))throw std::runtime_error("Waiting for compression and Library activation to finish, or for uncertain state to be resolved.");
      for(const auto& record:records_)if(!safe(record))throw std::logic_error("A protected compression operation needs recovery before updating Botty+.");
    }
    const auto version=request("/api/status").value("bottyWorker","");
    if(version!="library-1.2"&&version!="library-1.3")throw std::runtime_error("Cannot confirm the installed compression worker identity.");
    const auto worker=request("/api/gc/job");
    if(!worker.contains("busy")||!worker.at("busy").is_boolean()||worker.at("busy").get<bool>())throw std::runtime_error("Waiting for confirmed compression worker idle state.");
    std::lock_guard<std::mutex> g(mutex_);
    if(state_.value("status","")=="uncertain"||(!safe(state_)&&!pending(state_)))throw std::logic_error("Resolve the protected compression state before updating Botty+.");
    if(!safe(state_))throw std::runtime_error("Waiting for compression and Library activation to finish.");
  }
  json state() const {std::lock_guard<std::mutex> g(mutex_);auto out=state_;if(pending(state_)&&phaseStarted_.time_since_epoch().count())out["elapsed"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-phaseStarted_).count();if(std::chrono::steady_clock::now()-measuredAt_>std::chrono::seconds(5)){out["rate"]=0;out["eta"]=-1;}out["supported"]=enabled_;out["deletionSupported"]=enabled_;out["busy"]=enabled_&&pending(state_);return out;}
  json game(const std::string& id) const {std::lock_guard<std::mutex> g(mutex_);return state_.value("jobId","")==id?state_:records_.value(id,json::object());}
  bool protects(const std::string& id) const {const auto s=game(id).value("status","");return !s.empty()&&s!="restored"&&s!="failed"&&s!="cancelled";}
  json requestGameDeletion(const std::string& id) {
    std::lock_guard<std::mutex> g(mutex_);
    if(!enabled_||pending(state_))throw std::runtime_error("Wait for the current file operation");
    auto rec=records_.value(id,json::object());
    if(rec.value("status","")!="ready")throw std::runtime_error("No ready compressed game available");
    state_=rec;state_["deleteGameRequested"]=true;state_["restoreRequested"]=false;state_["deleteRequested"]=false;state_["status"]="waiting-close";state_["phase"]="Preparing deletion. Follow progress here; saves and archives are kept.";save();return state_;
  }
  json requestOriginalDeletion(const std::string& id) {
    std::lock_guard<std::mutex> g(mutex_);
    if(pending(state_)&&!(state_.value("status","")=="uncertain"&&state_.value("jobId","")==id))throw std::runtime_error("Wait for the current file operation");
    auto rec=records_.value(id,json::object());
    if((rec.value("status","")!="ready"&&rec.value("status","")!="uncertain")||!rec.value("originalKept",false)||!rec.contains("originalPath")||rec.value("originalDeletionStarted",false)||rec.value("gameDeletionStarted",false))throw std::runtime_error("No original backup available for deletion");
    state_=rec;state_["deleteRequested"]=true;state_["deleteGameRequested"]=false;state_["restoreRequested"]=false;state_["status"]="waiting-close";state_["phase"]="Preparing original-copy deletion. Compressed copy and archives are kept.";save();return state_;
  }
  json requestRestore(const std::string& id) {
    std::lock_guard<std::mutex> g(mutex_);
    if(pending(state_)&&state_.value("status","")!="uncertain")throw std::runtime_error("Wait for the current file operation");
    auto rec=records_.value(id,json::object());
    if(rec.value("gameDeletionStarted",false)||rec.value("originalDeletionStarted",false)||!rec.value("originalKept",false)||!rec.contains("originalPath"))throw std::runtime_error("No original backup is available");
    state_=rec;state_["restoreRequested"]=true;state_["deleteRequested"]=false;state_["status"]="waiting-close";state_["phase"]="Close Botty+ and games to restore the original source.";save();return state_;
  }
  json start(const json& job,const std::string& selected="") {
    std::lock_guard<std::mutex> g(mutex_);
    if(!enabled_)throw std::runtime_error("Compression worker is not installed");
    if(pending(state_))throw std::runtime_error("Compression is already active or needs review");
    const auto content=job.at("content");const auto title=content.value("titleId","");const auto id=job.at("id").get<std::string>();
    if(job.value("status","")!="moved"||content.value("kind","")!="folder"||!std::regex_match(title,std::regex("PPSA[0-9]{5}"))||title=="PPSA99071"||!std::regex_match(id,std::regex("[a-f0-9]{32}")))throw std::runtime_error("Only tracked PS5 game folders in Library can be compressed");
    if(records_.contains(id)&&records_[id].value("status","")!="failed"&&records_[id].value("status","")!="cancelled"&&records_[id].value("status","")!="restored")throw std::runtime_error("This game already has a compression record");
    const auto name=safeRelative(content.at("destination").get<std::string>());if(name.has_parent_path())throw std::runtime_error("Invalid Library folder");
    const auto sourceId=job.value("storage","internal"),targetId=selected.empty()?sourceId:selected;
    const auto sourcePaths=disk(sourceId),targetPaths=disk(targetId);
    const auto source=sourcePaths.library/name;
    if(job.value("destination","")!=source.string()||!fs::is_directory(containedExisting(sourcePaths.library,source)))throw std::runtime_error("Library source does not match the tracked folder");
    const auto output=targetPaths.root/"compressor/output"/(title+".ffpfsc");
    fs::create_directories(output.parent_path());
    if(records_.contains(id)&&records_[id].value("status","")=="restored") {
      if(records_[id].value("storage","internal")!=targetId||records_[id].value("source","")!=source.string())throw std::runtime_error("Retained compressed copy uses another location. Reactivate it there before transferring it.");
      state_=records_[id];state_["restoreRequested"]=false;state_["deleteRequested"]=false;state_["status"]="waiting-close";state_["phase"]="Close Botty+ and games to select the retained compressed copy.";save();return state_;
    }
    if(fs::exists(fs::symlink_status(output))||fs::exists(fs::symlink_status(output.string()+".vhash")))throw std::runtime_error("Compressed output already exists; nothing was overwritten");
    // APR images need a separate indexing workflow; fail closed for now.
    for(const auto& e:fs::recursive_directory_iterator(source))if((e.path().filename()=="libSceAmpr.sprx"||e.path().filename()=="libSceAmpr.prx")&&(!fs::is_regular_file(source/"ampr_emu.index")||fs::file_size(source/"ampr_emu.index")==0))throw std::runtime_error("This APR game needs a valid ampr_emu.index before compression");
    // Reject links/special files, including nested modules, before invoking the worker.
    uint64_t size=0;size_t count=0;
    for(const auto& entry:fs::recursive_directory_iterator(source)) {
      if(++count>200000)throw std::runtime_error("Too many source files");
      const auto s=entry.symlink_status();
      if(fs::is_symlink(s)||(!fs::is_directory(s)&&!fs::is_regular_file(s)))throw std::runtime_error("Unsupported source entry");
      if(fs::is_regular_file(s)){const auto bytes=entry.file_size();if(bytes>UINT64_MAX-size)throw std::runtime_error("Source too large");size+=bytes;}
    }
    const auto param=json::parse(readText(containedExisting(source,source/"sce_sys/param.json"),65536));
    if(param.value("titleId","")!=title)throw std::runtime_error("Source title identity mismatch");
    const auto workerVersion=request("/api/status").value("bottyWorker","");
    if(workerVersion!="library-1.3"&&(workerVersion!="library-1.2"||sourceId!="internal"||targetId!="internal"))throw std::runtime_error("Unexpected compression worker version");
    if(request("/api/gc/job").value("busy",true))throw std::runtime_error("Compression worker is busy");
    cleanFailedCopy(id,title,source,output,sourcePaths,targetId);
    if(size>UINT64_MAX-1073741824ULL||freeBytes(targetPaths.root)<size+1073741824ULL)throw std::runtime_error("Not enough space to keep original and compressed copy");
    state_={{"storage",targetId},{"sourceStorage",sourceId},{"status","starting"},{"jobId",job.at("id")},{"source",source.string()},{"titleId",title},{"startedAt",now()},{"name",job.value("name",title)},{"phase","Starting compressed copy"},{"originalKept",true},{"bytes",0},{"total",size}};
    save(); // Persist intent BEFORE sending a request that may outlive its response.
    try {
      const auto reply=request("/api/gc/compress?titleId="+title+"&sourcePath="+encode(source.string())+"&bottyOutputRoot="+encode(targetPaths.root.string())+"&format=exfat&deletePolicy=keep",true);
      const auto id=reply.at("id").get<std::string>();
      if(!std::regex_match(id,std::regex("op-[0-9]+")))throw std::runtime_error("Unexpected worker operation identity");
      state_["operationId"]=id;state_["status"]="running";save();
    }catch(...) {state_["status"]="uncertain";state_["error"]="Request outcome unknown. Original is kept; do not retry until inspected.";save();throw;}
    return state_;
  }
  void poll() {
    std::unique_lock<std::mutex> g(mutex_);
    if(!enabled_)return;
    if(state_.value("status","")=="waiting-close") {
      const auto title=state_.at("titleId").get<std::string>();auto work=state_;g.unlock();
      Paths targetPaths=paths_,sourcePaths=paths_;try{targetPaths=disk(work.value("storage","internal"));sourcePaths=disk(work.value("sourceStorage","internal"));}catch(...){return;}
      CompressionLibrary library(targetPaths,shadowPort_,"/data/shadowmount","/user/app","/system_ex/app",&sourcePaths);
      bool onlineDelete=false;
      if(work.value("deleteGameRequested",false))try{onlineDelete=BackgroundStorage(shadowPort_).supported();}catch(const std::exception& e){g.lock();state_["error"]=e.what();save();return;}
      if(!work.value("deleteRequested",false)&&!onlineDelete&&!library.idle(title)){
        g.lock();state_["phase"]="Waiting for ShadowMount. Close Botty+ only if your installed ShadowMount requires it.";save();return;
      }
      auto checkpoint=[&]{std::lock_guard<std::mutex> guard(mutex_);state_=work;if(!work.contains("remoteJobId")){state_["bytes"]=0;state_["total"]=0;state_["unit"]="bytes";}save();};
      auto progress=[&](uint64_t bytes,uint64_t total){std::lock_guard<std::mutex> guard(mutex_);state_["bytes"]=bytes;state_["total"]=total;state_["status"]="verifying";state_["unit"]="bytes";state_["phase"]="Verifying every file through the mounted compressed image";measure();};
      auto deletionProgress=[&](uint64_t done,uint64_t total,const std::string& file){std::lock_guard<std::mutex> guard(mutex_);state_["bytes"]=done;state_["total"]=total;state_["unit"]="items";state_["file"]=file;state_["phase"]="Deleting files and folders";measure();};
      try {if(work.value("deleteGameRequested",false)){work["status"]="ready";library.removeGame(work,checkpoint,deletionProgress);}else if(work.value("restoreRequested",false))library.restore(work,checkpoint);else if(work.value("deleteRequested",false)){work["status"]="ready";library.removeOriginal(work,checkpoint,deletionProgress);}else if(work.value("verifyRequested",false))library.verifyRetained(work,checkpoint,progress,[&]{return skipVerification_.load();});else library.activate(work,checkpoint,progress);}
      catch(const std::exception& e){work["status"]="uncertain";work["error"]=e.what();work["phase"]="Compression needs recovery before other file operations. Retained files were not automatically removed.";checkpoint();}
      return;
    }
    if(state_.value("status","")=="uncertain"||!pending(state_)||!state_.contains("operationId"))return;
    try {
      const auto history=request("/api/gc/history").at("history");
      for(const auto& op:history) {
        if(op.value("id","")!=state_.at("operationId")||op.value("sourcePath","")!=state_.at("source")||op.value("createdAt",0LL)<state_.value("startedAt",0LL)-2)continue;
        const auto status=op.value("status","");
        state_["worker"]=op;
        if(status=="success"&&op.value("result","")=="copy-created-unverified") {
          state_["status"]="waiting-close";state_["phase"]="Copy created. Close Botty+ and games to mount the compressed game.";state_["output"]=op.value("outputPath","");state_["error"]="";
        }else if(status=="failed"||status=="cancelled") {
          state_["status"]=status;state_["phase"]="Compression stopped; original is kept";state_["error"]=op.value("error","");
        }else if(status=="success") {
          state_["status"]="uncertain";state_["error"]="Unexpected worker result; inspect before continuing";
        }else {
          const auto live=request("/api/gc/job");
          if(live.value("activeId","")==state_.at("operationId")) {
            state_["unit"]="bytes";state_["phase"]=live.value("phase","Working");state_["bytes"]=live.value("copiedBytes",0LL);state_["total"]=live.value("totalBytes",0LL);
          }
          state_["error"]="";
        }
        save();return;
      }
      state_["error"]="Worker operation missing; original is kept. Do not retry.";
    }catch(...) {state_["error"]="Waiting for compression worker. Original is kept.";}
    state_["rate"]=0;state_["eta"]=-1;
  }
  json relocate(const std::string& id,const std::string& selected,const std::function<void()>& check,const BackgroundStorage::Report& report={}) {
    std::unique_lock<std::mutex> g(mutex_);
    if(pending(state_))throw std::runtime_error("Wait for compression to finish");
    auto rec=records_.value(id,json::object());
    if(rec.value("status","")!="ready")throw std::runtime_error("Only ready compressed games can be transferred");
    const auto oldPaths=disk(rec.value("storage","internal")),newPaths=disk(selected);
    const auto title=rec.at("titleId").get<std::string>();
    const fs::path image=rec.at("output").get<std::string>();
    if(image!=oldPaths.root/"compressor/output"/(title+".ffpfsc"))throw std::runtime_error("Unexpected compressed path");
    containedExisting(oldPaths.root,image);
    const auto dest=newPaths.root/"compressor/output"/image.filename();fs::create_directories(dest.parent_path());
    g.unlock();CompressionLibrary library(oldPaths,shadowPort_);
    BackgroundStorage background(shadowPort_);
    if(background.supported()) {
      rec["status"]="uncertain";rec["phase"]="Moving compressed game";g.lock();state_=rec;save();g.unlock();
      const fs::path hashes=image.string()+".vhash",newHashes=dest.string()+".vhash";
      if(fs::exists(hashes))copyChecked(hashes,newHashes,check);
      background.run("move",title,image,dest,report,check);
      check();rec["storage"]=selected;rec["output"]=dest.string();rec["verified"]=false;rec["verificationSkipped"]=true;rec["status"]="ready";rec["phase"]="Compressed game moved - Not verified";g.lock();state_=rec;save();g.unlock();
      if(fs::exists(hashes))removeTransferred(hashes);
      return rec;
    }
    if(!library.idle(title))throw std::runtime_error("Close Botty+ and games before transferring this game");
    rec["status"]="uncertain";rec["phase"]="Transferring compressed game";g.lock();state_=rec;save();g.unlock();
    copyChecked(image,dest,check);copyChecked(image.string()+".vhash",dest.string()+".vhash",check);
    library.api("manual/remove",{{"path",image.string()}});
    library.api("manual/add",{{"path",dest.string()}});
    library.api("scan",{{"reset_attempts",false}});
    bool found=false;for(int n=0;n<60;++n){check();try{if(library.api("games/info",{{"title_id",title}}).value("path","")==dest.string()){found=true;break;}}catch(...){}std::this_thread::sleep_for(std::chrono::seconds(1));}
    if(!found)throw std::runtime_error("New compressed source not confirmed; both copies kept");
    check();rec["storage"]=selected;rec["output"]=dest.string();rec["verified"]=false;rec["verificationSkipped"]=true;rec["status"]="ready";rec["phase"]="Compressed game transferred - Not verified";g.lock();state_=rec;save();g.unlock();
    removeTransferred(image);removeTransferred(image.string()+".vhash");return rec;
  }
  json requestVerification(const std::string& id) {
    std::lock_guard<std::mutex> g(mutex_);
    if(pending(state_))throw std::runtime_error("Wait for the current file operation");
    const auto rec=records_.value(id,json::object());
    if(rec.value("status","")!="ready"||!rec.value("originalKept",false)||!rec.contains("originalPath"))throw std::runtime_error("Verification needs the retained original");
    state_=rec;state_["verifyRequested"]=true;state_["deleteRequested"]=false;state_["deleteGameRequested"]=false;state_["restoreRequested"]=false;
    skipVerification_=false;state_["status"]="waiting-close";state_["phase"]="Close Botty+ and games for optional verification";save();return state_;
  }
  json skipVerification(const std::string& id) {
    std::lock_guard<std::mutex> g(mutex_);
    if(state_.value("jobId","")!=id||!state_.value("verifyRequested",false)||
       (state_.value("status","")!="verifying"&&state_.value("status","")!="waiting-close"))throw std::runtime_error("No optional verification is active for this game");
    skipVerification_=true;
    return {{"ok",true},{"skipRequested",true}};
  }
  void stopWorker() {
    std::lock_guard<std::mutex> g(mutex_);
    if(pending(state_))throw std::runtime_error("Compression is active or uncertain");
    if(enabled_)request("/api/control/shutdown",true);
  }
  json cancel(const std::string& id) {
    std::lock_guard<std::mutex> g(mutex_);
    if(!pending(state_)||state_.value("jobId","")!=id||!state_.contains("operationId"))throw std::runtime_error("No identified active compression for this game");
    const auto live=request("/api/gc/job");
    if(live.value("activeId","")!=state_.at("operationId"))throw std::runtime_error("Worker operation changed; refresh before cancelling");
    request("/api/gc/job/cancel",true);return {{"ok",true}};
  }
};
}
