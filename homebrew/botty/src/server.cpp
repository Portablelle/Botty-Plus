#include "core.hpp"
#include "storage.hpp"
#include "progress.hpp"
#include "operations.hpp"
#include "search.hpp"
#include "rtorrent.hpp"
#include "remote.hpp"
#include "rest-mode.hpp"
#include "rest-companion.hpp"
#include "rest-network.hpp"
#define CPPHTTPLIB_THREAD_POOL_COUNT 3
#include "httplib.h"
#include "compressor.hpp"
#include "native-update.hpp"
#include "native-process.hpp"
#include "emulation.hpp"
#include "switch-home.hpp"
#include "switch-preparation.hpp"
#include "switch-source-settlement.hpp"
#include "installation-update.hpp"
#include "service-updater-embedded.hpp"
#include <chrono>
#include <atomic>
#include <csignal>
#include <iostream>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
using namespace botty;
#ifndef BOTTY_UI
#define BOTTY_UI "/data/botty/manager/1.10.5/ui"
#endif
#ifdef __PS5__
// Run before C++ globals so loader/initialization failures leave a useful boundary.
__attribute__((constructor(101))) static void startupLog() {
  int fd=open("/data/botty/manager/startup.log",O_WRONLY|O_CREAT|O_TRUNC,0600);
  if(fd>=0) {dup2(fd,STDERR_FILENO);if(fd!=STDERR_FILENO)close(fd);}
  const char message[]="Botty loader entered; initializing C++ runtime\n";
  write(STDERR_FILENO,message,sizeof(message)-1);
}

#endif
namespace {
Search search,explore;
Emulation emulation;
SwitchHome switchHome;
Emulation legacyEmulation("/data/prosperoeden","/data/homebrew/PPSA99008");
Compressor compressor;
std::unique_ptr<NativeTransaction> nativeTransaction;
std::unique_ptr<InstallationUpdater> nativeUpdater;
Operations operations;
Operations transfers;
std::mutex lock;
json jobs=json::array(); bool extracting=false;
std::atomic<bool> retiring{false};
std::atomic<bool> httpReady{false};
class ServerThreads {
  httplib::Server& server_;
  std::atomic<bool> stopping_{false};
  std::mutex stopMutex_;
  std::thread ready_,stop_;
public:
  explicit ServerThreads(httplib::Server& server):server_(server){}
  ~ServerThreads(){join();}
  void watchReady(){ready_=std::thread([this]{while(!stopping_){httpReady=server_.is_accepting();std::this_thread::sleep_for(std::chrono::milliseconds(20));}});}
  void stop(){std::lock_guard<std::mutex> guard(stopMutex_);if(stop_.joinable())throw std::runtime_error("Manager stop is already queued.");stop_=std::thread([this]{std::this_thread::sleep_for(std::chrono::milliseconds(250));server_.stop();});}
  void join(){stopping_=true;if(ready_.joinable())ready_.join();if(stop_.joinable())stop_.join();}
};
std::atomic<bool> cancelExtraction{false}; std::string activeJob;
Paths paths;
Storage storage;
std::unique_ptr<RemoteDownloads> remote;
std::string remoteError;
std::vector<fs::path> testMounts;
std::atomic<bool> transferring{false},registeringLibrary{false};
void requireLibraryIdle(){if(registeringLibrary)throw std::runtime_error("Library registration is running; retry shortly");}
json transferState={{"status","idle"}};
std::string uiDir=BOTTY_UI;
std::string token;
int rpcPort=5001, port=8088, compressorPort=5910, shadowPort=10101;

std::string connectionUrl() {
  ifaddrs* interfaces=nullptr;
  if(getifaddrs(&interfaces)!=0)return "";
  std::string result;
  for(auto* iface=interfaces;iface;iface=iface->ifa_next) {
    if(!iface->ifa_addr || iface->ifa_addr->sa_family!=AF_INET ||
       !(iface->ifa_flags&IFF_UP) || (iface->ifa_flags&IFF_LOOPBACK))continue;
    char address[INET_ADDRSTRLEN]{};
    const auto* addr=reinterpret_cast<const sockaddr_in*>(iface->ifa_addr);
    if(!inet_ntop(AF_INET,&addr->sin_addr,address,sizeof(address)))continue;
    // Advertise the authenticated Botty web interface on the private LAN.
    if(std::string(address).rfind("192.168.",0)==0) {result="http://"+std::string(address)+":8088";break;}
  }
  freeifaddrs(interfaces);
  return result;
}
Rtorrent& localBackend(){static Rtorrent backend(paths,rpcPort);return backend;}
std::string selectedDownloadEngine(const json& arguments){
  const auto engine=arguments.value("engine",std::string("rtorrent"));
  if(engine!="rtorrent"&&engine!="qbittorrent")throw std::runtime_error("Choose rTorrent on PS5 or qBittorrent on the dedicated server");
  if(engine=="qbittorrent"&&!remote)throw std::runtime_error(remoteError.empty()?"Dedicated server is not configured":remoteError);
  return engine;
}
json rpc(const std::string& method, const json& arguments=json::object()) {
  auto& backend=localBackend();
  if(method=="torrent-get"&&remote){
    json entries=json::array();try{entries=backend.request(method,arguments).at("torrents");}catch(...){}
    const auto remoteRows=remote->list();json combined=json::array();
    for(const auto& r:remoteRows)if(!r.value("remoteCandidate",false))combined.push_back(r);
    for(const auto& r:entries)combined.push_back(r);
    for(const auto& r:remoteRows)if(r.value("remoteCandidate",false))combined.push_back(r);
    return {{"torrents",combined}};
  }
  if(method=="torrent-add"){
    const auto engine=selectedDownloadEngine(arguments);
    if(engine=="qbittorrent"){
      if(!remote)throw std::runtime_error(remoteError.empty()?"Dedicated server is not configured":remoteError);
      return remote->add(Rtorrent::inputHash(arguments),arguments);
    }
    auto local=arguments;local.erase("engine");local.erase("platform");return backend.request(method,local);
  }
  if(remote&&arguments.contains("ids")){
    bool any=false,all=true;for(const auto& id:arguments.at("ids")){const bool owned=remote->contains(id);any|=owned;all&=owned;}
    if(any){if(!all)throw std::runtime_error("Split local and remote torrent operations");return remote->action(method,arguments);}
  }
  return backend.request(method,arguments);
}
json torrents() {
  return rpc("torrent-get",{{"fields",{"id","hashString","name","status","percentDone","leftUntilDone","totalSize","sizeWhenDone","eta","downloadDir","rateDownload","rateUpload","error","errorString","files","peersConnected","peersSendingToUs","peersGettingFromUs"}}}).at("torrents");
}
json getTorrent(int id) {
  for(auto& item:torrents())if(item.at("id")==id)return item;
  throw std::runtime_error("Torrent no longer exists");
}
void publishJob(json job, bool checkpoint=true) {
  // Only this extraction worker publishes its active job. Persist before making
  // terminal states visible, without holding the lock used by HTTP readers.
  if(checkpoint)writeJson(paths.jobs/(job.at("id").get<std::string>()+".json"),job);
  std::lock_guard<std::mutex> guard(lock);
  for(auto& old:jobs)if(old.at("id")==job.at("id")){old=job;return;}
  jobs.push_back(job);
}
json findJob(const std::string& id) {
  if(!std::regex_match(id,std::regex("[a-f0-9]{32}")))throw std::runtime_error("Invalid job ID");
  for(const auto& job:jobs)if(job.at("id")==id)return job;
  throw std::runtime_error("Job not found");
}
// One coordinator owns admission, work, terminal state and restart recovery.
class SwitchPreparationCoordinator {
  SwitchSourceSettlement settlement_;
  static void completed(json& job,const json& receipt,const fs::path& root,const std::string& phase){
    job["status"]="moved";job["publicationPending"]=false;job["libraryStatus"]="emulation";
    job["destination"]=root.string();job["content"]["files"]=receipt.at("files");job["error"]="";job["phase"]=phase;
  }
public:
  bool publishedIntact(const json& job,const fs::path& root) const {
    try{
      if(job.value("emulationRoot",root.string())!=root.string())return false;
      const std::string id=job.at("id");
      const auto record=json::parse(readText(containedExisting(root,root/"imports"/(id+".json")),1024*1024));
      if(record.at("status")!="complete"||record.at("receipt").at("files")!=job.at("content").at("files"))return false;
      for(const auto& file:record.at("receipt").at("files")){
        const auto relative=safeRelative(file.at("name"));const auto kind=relative.begin()->string();
        if(kind!="roms"&&kind!="updates")return false;
        const auto path=containedExisting(root,root/kind/(".botty-batch-"+id)/relative.filename());
        if(json(Emulation::sourceIdentity(path))!=file.at("identity"))return false;
        if(Emulation::supported(path))Emulation::entry(root,path,kind);
      }
      return true;
    }catch(const std::exception&){return false;}
  }
  bool retained(const json& job) const {
    if(job.value("publicationPending",false))return true;
    const fs::path root=job.value("emulationRoot",emulation.root().string());
    const auto id=job.at("id").get<std::string>();
    return fs::exists(root/"imports"/(id+".json"))||fs::exists(root/".botty-preparation"/(id+".working")/"prepared.json");
  }
  void removePartial(const json& job) const {
    if(retained(job))throw std::runtime_error("Switch publication needs recovery; restart Botty before retrying or removing this preparation");
    const auto root=emulation.root();
    if(job.value("emulationRoot",root.string())!=root.string())throw std::runtime_error("Reconnect the original emulator folder before removing partial files");
    const auto parent=root/".botty-preparation";const auto name=job.at("id").get<std::string>()+".working";
    if(fs::exists(parent/name))deleteGameDirectory(parent,name);
  }
  json start(const json& request,bool automatic=true);
  void recover(json& job){
        const std::string id=job.at("id");
        const auto before=job;
        try{
          const auto root=emulation.root();
          if(job.value("emulationRoot",root.string())!=root.string())throw std::runtime_error("Reconnect the original emulator folder to resume Switch publication");
          const auto stage=root/".botty-preparation"/(id+".working");
          const auto record=root/"imports"/(id+".json");
          const bool retained=fs::exists(stage/"prepared.json")||fs::exists(record)||job.value("publicationPending",false);
          if(retained){
            job["publicationPending"]=true;
            const auto receipt=publishSwitchBatch(stage,root,id);
            completed(job,receipt,root,"Verified Switch batch recovered in Emulation");
          }else if(job.value("status","")=="extracting"||job.value("status","")=="moving"){
            job["status"]="interrupted";job["error"]="Switch preparation interrupted; sources and partial output retained";
          }
        }catch(const std::exception& error){
          job["status"]="interrupted";job["error"]=error.what();job["phase"]="Switch publication retained; reconnect its disk and restart Botty to resume";
        }
        job["extractionRate"]=0;job["eta"]=-1;job["elapsed"]=0;
        if(job!=before)writeJson(paths.jobs/(job.at("id").get<std::string>()+".json"),job);
  }
};
SwitchPreparationCoordinator switchPreparation;
void recoverJobs() {
  for(const auto& entry:fs::directory_iterator(paths.jobs)) {
    if(!entry.is_regular_file() || entry.path().extension()!=".json")continue;
    try {
      auto job=json::parse(readText(entry.path()));
      const std::string id=job.at("id");
      if(!std::regex_match(id,std::regex("[a-f0-9]{32}")) || entry.path().stem()!=id)continue;
      if(job.value("platform",std::string{})=="switch"&&!job.value("dismissed",false)&&job.value("status","")!="moved"){
        switchPreparation.recover(job);jobs.push_back(job);continue;
      }
      const bool retainedBeforePublication=job.value("status","")=="move-error"&&job.value("adoptedImport",false)&&!job.contains("destination");
      if(job.value("status","")=="extracting" || job.value("status","")=="moving" || retainedBeforePublication) {
        const bool preparing=job.value("status","")=="extracting"||retainedBeforePublication,wasAutomatic=job.value("automatic",false);
        job["extractionRate"]=0;job["eta"]=-1;job["elapsed"]=0;job["status"]="interrupted";job["error"]="Previous session ended during this operation. Extract the same archive again to verify and resume supported partial output.";
        if(preparing&&job.value("adoptedImport",false)){job["status"]="ready";job["automatic"]=false;job["error"]="Imported files are protected. Reconnect their disk and choose Move to Library to resume.";job["phase"]="Imported preparation needs resuming";}
        if(preparing&&job.value("platform",std::string("ps5"))!="switch")try{
          const auto disk=storage.get(job.value("storage","internal"),false);
          const auto final=disk.extracted/id;
          if(fs::is_directory(final)&&!fs::is_symlink(fs::symlink_status(final))){
            const auto content=classify(final);if(content.value("kind","")!="unsupported"){job["content"]=content;job["status"]="ready";job["error"]="";job["phase"]="Prepared output recovered; waiting for Library publication";job["automatic"]=wasAutomatic;}
          }
        }catch(...){}
        writeJson(entry.path(),job);
      }
      jobs.push_back(job);
    }catch(...){/* Do not overwrite unknown or damaged state. */}
  }
}
void resumeAdoptedOutput(const Paths& disk,const json& job,const std::function<void()>& check={}){
  if(!job.value("adoptedImport",false)||job.value("status","")!="ready"||job.contains("destination"))return;
  const auto final=disk.extracted/job.at("id").get<std::string>();
  if(!fs::exists(fs::symlink_status(final))||(fs::is_directory(fs::symlink_status(final))&&fs::is_empty(final))){
    if(!remote)throw std::runtime_error("Reconnect the dedicated server to resume this imported preparation");
    remote->adopt(job.at("torrentId"),final,job.at("id"),[&]{if(check)check();},true);
  }
  if(fs::exists(fs::symlink_status(final/remoteOwnerFile)))fs::remove(containedExisting(final,final/remoteOwnerFile));
}
struct PreparationBusy:std::runtime_error {using std::runtime_error::runtime_error;};
struct PreparationPlan {
  enum class Kind { rar, loose };
  Kind kind;fs::path source;std::string password;
  bool loose()const{return kind==Kind::loose;}
};
void requirePreparationAdmission(){
  if(retiring)throw PreparationBusy("Botty is shutting down");
  try{nativeUpdater->requireAdmission();compressor.requireIdle();requireLibraryIdle();}catch(const std::runtime_error& e){throw PreparationBusy(e.what());}
  if(registeringLibrary||transferring||extracting)throw PreparationBusy("Wait for the active file operation");
}
// All formats share durable admission, worker ownership, progress and cleanup.
// Only source validation, preparation/publication and error recovery vary.
template<class Work,class Failure>
json startPreparationWorker(json job,Work work,Failure failure){
  writeJson(paths.jobs/(job.at("id").get<std::string>()+".json"),job);
  const int directory=::open(paths.jobs.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
  if(directory<0)throw std::runtime_error("Cannot open preparation journal directory");
  const int flushed=::fsync(directory);::close(directory);
  if(flushed)throw std::runtime_error("Cannot persist preparation journal directory");
  bool replaced=false;for(auto& old:jobs)if(old.at("id")==job.at("id")){old=job;replaced=true;break;}
  if(!replaced)jobs.push_back(job);
  cancelExtraction=false;activeJob=job.at("id");extracting=true;
  try{std::thread([job,work=std::move(work),failure=std::move(failure)]()mutable{
    const auto began=std::chrono::steady_clock::now();ProgressSchedule schedule;ExtractionEstimate estimate;std::string phase;
    const Reporter report=[&](const Progress& p){
      const auto now=std::chrono::steady_clock::now();const auto decision=schedule.update(now,p.phase);if(!decision.publish)return;
      if(phase!=p.phase){phase=p.phase;estimate=ExtractionEstimate{};}if(p.total)estimate.update(now,p.bytes,p.total);
      job["phase"]=p.phase;job["file"]=p.file;job["bytes"]=p.bytes;job["total"]=p.total;
      job["elapsed"]=std::chrono::duration<double>(now-began).count();job["extractionRate"]=estimate.rate;job["eta"]=estimate.eta;
      publishJob(job,decision.checkpoint);
    };
    try{work(job,report);}catch(const std::exception& error){failure(job,error);}
    job["elapsed"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
    job["extractionRate"]=0;job["eta"]=job.value("status",std::string{})=="ready"?0:-1;
    try{publishJob(job);}catch(const std::exception& error){std::cerr<<"Preparation checkpoint failed: "<<error.what()<<'\n';}
    std::lock_guard<std::mutex> guard(lock);extracting=false;activeJob.clear();
  }).detach();}catch(...){
    extracting=false;activeJob.clear();job["status"]="failed";job["error"]="Cannot start preparation worker; sources and partial output retained";
    try{writeJson(paths.jobs/(job.at("id").get<std::string>()+".json"),job);}catch(...){}
    for(auto& old:jobs)if(old.at("id")==job.at("id")){old=job;break;}
    throw;
  }
  return job;
}
json startExtraction(const json& request, bool automatic=false) {
  const int id=request.at("id").get<int>();
  const std::string name=request.value("archive",std::string{});PreparationPlan plan{name.empty()?PreparationPlan::Kind::loose:PreparationPlan::Kind::rar,{},request.value("password",std::string{})};
  if(plan.password.size()>1024)throw std::runtime_error("Password too long");
  std::lock_guard<std::mutex> guard(lock);
  requirePreparationAdmission();
  const auto torrent=getTorrent(id);
  if(torrent.at("leftUntilDone").get<uint64_t>()!=0 || torrent.value("error",0)!=0 || torrent.value("status",0)==1 || torrent.value("status",0)==2)throw PreparationBusy("Wait until the torrent is complete and error-free");
  const auto sourceStorage=torrent.value("remote",false)?remote->published(id).at("storage").get<std::string>():storage.forDownload(torrent.at("downloadDir").get<std::string>());
  const auto targetStorage=request.value("storage",sourceStorage);
  const auto sourcePaths=storage.get(sourceStorage),selectedPaths=storage.get(targetStorage);
  bool found=false;
  for(const auto& file:torrent.at("files")) {
    // Require all files, including unselected volumes, to be downloaded.
    if(file.at("bytesCompleted").get<uint64_t>()!=file.at("length").get<uint64_t>())throw std::runtime_error("Some torrent files are missing or incomplete");
    if(file.at("name")==name)found=true;
  }
  if(plan.loose()){
    if(!torrent.value("remote",false)&&packageTorrent(torrent))throw std::runtime_error(packagePolicyMessage());
    const auto content=classifyDownload(sourcePaths.complete,torrent.at("files"),torrent.value("remote",false)?remoteOwnerFile:std::string{});
    if(content.value("kind","")=="unsupported")throw std::runtime_error(content.at("reason").get<std::string>());
    plan.source=containedExisting(sourcePaths.complete,sourcePaths.complete/fs::path(content.at("source").get<std::string>()));
  }else{
    if(!found || asciiLower(fs::path(name).extension().string())!=".rar")throw std::runtime_error("Select the first .rar volume from this torrent");
    std::smatch part;if(std::regex_search(name,part,std::regex("\\.part([0-9]+)\\.rar$",std::regex::icase))&&std::stoul(part[1])!=1)throw std::runtime_error("Select part1.rar, not a continuation volume");
    plan.source=containedExisting(sourcePaths.complete,sourcePaths.complete/safeRelative(name));
    if(!fs::is_regular_file(plan.source))throw std::runtime_error("Archive is not a regular file");
  }
  // Check duplicate active or completed jobs; archives are never deleted by extraction.
  for(const auto& job:jobs)if(job.value("hash","")==torrent.at("hashString") && job.value("archive","")==name &&
      (job.value("status","")=="ready" || job.value("status","")=="moved"))throw std::runtime_error("This archive was already extracted");
  json job={{"id",randomId()},{"name",torrent.at("name")},{"hash",torrent.at("hashString")},{"archive",name},{"preparation",plan.loose()?"loose":"rar"},{"remote",torrent.value("remote",false)},{"adoptedImport",plan.loose()&&torrent.value("remote",false)&&sourceStorage==targetStorage},
    {"storage",targetStorage},{"archiveStorage",sourceStorage},{"automatic",automatic},{"status","extracting"},{"phase","Starting"},{"bytes",0},{"total",0},{"error",""}};
  bool resuming=false;
  for(const auto& old:jobs) {
    const auto status=old.value("status","");
    if(old.value("hash","")!=torrent.at("hashString")||old.value("archive","")!=name||
        (status!="interrupted"&&status!="cancelled"&&status!="failed")||old.value("dismissed",false))continue;
    const auto oldPaths=storage.get(old.value("storage","internal"));
    const auto stage=oldPaths.extracted/(old.at("id").get<std::string>()+".working");
    if(!fs::exists(fs::symlink_status(stage)))continue;
    if(resuming)throw std::runtime_error("Multiple partial extractions exist; choose which partial output to keep before retrying");
    if(old.value("storage","internal")!=targetStorage)throw std::runtime_error("Resume on the original disk, or remove the partial extraction first");
    containedExisting(oldPaths.extracted,stage);if(plan.loose())throw std::runtime_error("Remove the interrupted preparation before trying again; downloads are kept");job=old;resuming=true;
  }
  job["torrentId"]=id;job["status"]="extracting";job["phase"]=resuming?"Verifying existing files for resume":"Starting";
  job["bytes"]=0;job["total"]=0;job["error"]="";job["extractionRate"]=0;job["eta"]=-1;
  return startPreparationWorker(job,
    [plan,resuming,sourceStorage,targetStorage,sourcePaths,selectedPaths,torrent](json& job,const Reporter& progress){
      const auto check=[&]{if(storage.get(sourceStorage,false).root!=sourcePaths.root || storage.get(targetStorage,false).root!=selectedPaths.root)throw std::runtime_error("Disk mount changed during extraction");};
      const Reporter report=[&](const Progress& p){check();progress(p);};
      const auto stage=selectedPaths.extracted/(job.at("id").get<std::string>()+".working");
      const auto final=selectedPaths.extracted/job.at("id").get<std::string>();
        if(plan.loose()){
          if(torrent.value("remote",false)&&sourceStorage==targetStorage){
            remote->adopt(torrent.at("id"),final,job.at("id"),[&]{check();if(cancelExtraction||retiring)throw std::runtime_error("Preparation cancelled");});
            if(fs::exists(fs::symlink_status(final/remoteOwnerFile)))fs::remove(containedExisting(final,final/remoteOwnerFile));
            report({"Preparing downloaded game","",0,0});
          }else{
            if(!fs::create_directory(stage))throw std::runtime_error("Preparation staging already exists");
            const auto name=fs::is_directory(plan.source)?fs::path("game"):plan.source.filename();
            copyChecked(plan.source,stage/name,[&]{check();if(cancelExtraction||retiring)throw std::runtime_error("Preparation cancelled");},[&](uint64_t bytes,uint64_t total,const std::string& file){report({"Preparing game; torrent kept for seeding",file,bytes,total});});
            publishExclusive(stage,final);
          }
        }
        else extractRar(plan.source,stage,report,plan.password,[]{return cancelExtraction.load();},0,resuming);
        if(cancelExtraction||retiring)throw std::runtime_error("Preparation cancelled");
        check();if(!plan.loose())fs::rename(stage,final);job["content"]=classify(final);job["status"]="ready";job["phase"]="Extraction verified";
        if(job.value("automatic",false)||plan.loose()){
          if(cancelExtraction||retiring)throw std::runtime_error("Preparation cancelled");
          publishJob(job);
          try{job=movePrepared(selectedPaths,job,nullptr,[&]{check();if(cancelExtraction||retiring)throw std::runtime_error("Preparation cancelled");});}
          catch(const std::exception& failure){job=json::parse(readText(paths.jobs/(job.at("id").get<std::string>()+".json")));job["error"]=failure.what();job["phase"]="Needs attention";}
        }
    },
    [plan,torrent,sourceStorage,targetStorage,selectedPaths](json& job,const std::exception& error){
      const auto final=selectedPaths.extracted/job.at("id").get<std::string>();
        job["status"]=cancelExtraction.load()?"cancelled":"failed";job["error"]=error.what();
        if(cancelExtraction.load())job["phase"]="Cancelled; partial files kept";
        std::error_code finalError;const auto finalStatus=fs::symlink_status(final,finalError);
        const bool adopted=plan.loose()&&torrent.value("remote",false)&&sourceStorage==targetStorage&&remote->adoptedBy(torrent.at("id"),job.at("id"));
        if(!adopted)job["adoptedImport"]=false;
        if(adopted||cancelExtraction.load()){
          // Adoption may already have moved the only PS5 copy. Never expose it
          // as disposable failed output, even when retirement or I/O failed.
          if(adopted){job["status"]=job.contains("destination")?"move-error":"ready";job["automatic"]=false;job["phase"]="Imported files kept; reconnect the disk and move to Library to finish";}
          if(fs::is_directory(finalStatus)&&!fs::is_symlink(finalStatus))try{
            job["content"]=classify(final);job["automatic"]=false;job["error"]=std::string(error.what())+"; prepared files kept";
            if(job["content"].value("kind","")!="unsupported"){job["status"]="ready";job["phase"]="Move to Library to finish";}
          }catch(...){}
        }
    });
}
// Switch jobs share PS5 admission/progress/recovery but publish into Eden, never
// into ShadowMount's PS5 Library. Every container is identified before import.
json emulationState(const json& downloads);
bool switchTorrent(const json& torrent){
 if(torrent.value("emulationContent",false)||torrent.value("platform",std::string{})=="switch")return true;
 for(const auto& file:torrent.value("files",json::array()))if(Emulation::supported(fs::path(file.at("name").get<std::string>())))return true;
 const auto hash=torrent.value("hashString",std::string{});if(!std::regex_match(hash,std::regex("[a-fA-F0-9]{40}")))return false;
 try{const auto file=paths.root/"automatic"/(torrent.value("remote",false)?"remote-"+std::to_string(torrent.at("id").get<int>())+".json":hash+".json");return json::parse(readText(file,16384)).value("platform",std::string{})=="switch";}catch(...){return false;}
}
json SwitchPreparationCoordinator::start(const json& request,bool automatic){
 std::lock_guard<std::mutex> guard(lock);requirePreparationAdmission();
 struct Source {
  fs::path root,mountRoot;json files=json::array(),torrentId=nullptr;
  std::string storageId,hash,name;bool remote=false;
 } source;
 json torrent;
 if(request.contains("entry")){
  const auto snapshot=emulationState(torrents());json selected;
  for(const auto& item:snapshot.at("entries"))if(item.at("id")==request.at("entry")&&item.at("kind")=="download"){selected=item;break;}
  if(selected.is_null())throw std::runtime_error("Switch file is no longer available");
  if(selected.contains("torrentId"))torrent=getTorrent(selected.at("torrentId"));
  else{
   const fs::path file=selected.at("path").get<std::string>();source.root=file.parent_path();
   source.hash=nativeSha256(file.string()).substr(0,40);source.name=selected.at("name");
   source.files.push_back({{"name",file.filename().string()},{"size",selected.at("size")}});
  }
 }else torrent=getTorrent(request.at("id"));
 if(!torrent.is_null()){
  if(torrent.value("leftUntilDone",1ULL)!=0||torrent.value("error",0)||torrent.value("status",0)==1||torrent.value("status",0)==2)throw PreparationBusy("Wait for the complete, verified download");
  source.remote=torrent.value("remote",false);source.torrentId=torrent.at("id");source.hash=torrent.at("hashString");source.name=torrent.at("name");
  source.storageId=source.remote?remote->published(torrent.at("id")).at("storage").get<std::string>():storage.forDownload(torrent.at("downloadDir").get<std::string>());
  const auto disk=storage.get(source.storageId);source.root=disk.complete;source.mountRoot=disk.root;
  for(const auto& f:torrent.at("files")){if(f.at("bytesCompleted")!=f.at("length"))throw std::runtime_error("Some Switch archive volumes or files are incomplete");source.files.push_back({{"name",f.at("name")},{"size",f.at("length")}});}
 }
 const auto directoryIdentity=[](const fs::path& path){struct stat st{};if(::lstat(path.c_str(),&st)||!S_ISDIR(st.st_mode))throw std::runtime_error("Switch source folder is unavailable");return std::array<uint64_t,2>{uint64_t(st.st_dev),uint64_t(st.st_ino)};};
 const auto sourceIdentity=directoryIdentity(source.root);
 json fingerprint=json::array();
 for(const auto& f:source.files){const auto file=containedExisting(source.root,source.root/safeRelative(f.at("name").get<std::string>()));fingerprint.push_back({{"name",f.at("name")},{"identity",Emulation::sourceIdentity(file)}});}
 if(source.torrentId.is_null())source.hash=nativeSha256(source.root.string()+":"+fingerprint.dump()).substr(0,40);
 if(!settlement_.observe(source.root.string()+":"+source.hash,fingerprint))throw PreparationBusy("Finalizing Switch download. Wait a few seconds; automatic preparation will retry. For a manual import, choose Prepare again.");
 const auto root=emulation.root();
 for(const auto& old:jobs)if(old.value("platform",std::string{})=="switch"&&old.value("hash",std::string{})==source.hash){
  if(old.value("status",std::string{})=="moved"){if(publishedIntact(old,root))throw std::runtime_error("This Switch release is already imported in Emulation");continue;}
  if(!old.value("dismissed",false)&&retained(old))throw std::runtime_error("Switch publication needs recovery; restart Botty to resume its retained transaction before retrying");
 }
 fs::create_directories(root/".botty-preparation");containedExisting(root,root/".botty-preparation");
 json job={{"id",randomId()},{"hash",source.hash},{"name",source.name},{"sourceRoot",source.root.string()},{"sourceKind",source.torrentId.is_null()?"existing":"download"},{"platform","switch"},{"emulationContent",true},{"emulationRoot",root.string()},{"remote",source.remote},{"automatic",automatic},{"preparation","switch"},{"status","extracting"},{"phase","Checking Switch download"},{"error",""},{"bytes",0},{"total",0},{"content",{{"kind","switch"}}}};
 if(!source.torrentId.is_null())job["torrentId"]=source.torrentId;
 if(!source.storageId.empty())job["storage"]=source.storageId;
 return startPreparationWorker(job,
  [source,sourceIdentity,directoryIdentity,root](json& job,const Reporter& progress){
   const std::string id=job.at("id");const auto stage=root/".botty-preparation"/(id+".working");
   const auto check=[&]{if(retiring||cancelExtraction)throw std::runtime_error("Switch preparation cancelled; source files kept");if(emulation.root()!=root||directoryIdentity(source.root)!=sourceIdentity||(!source.storageId.empty()&&storage.get(source.storageId,false).root!=source.mountRoot))throw std::runtime_error("Switch source or destination disk changed");};
   const Reporter report=[&](const Progress& p){check();progress(p);};
   const auto receipt=prepareSwitchFiles(source.root,source.files,stage,root/"keys/prod.keys",job.at("name"),report,check);
   check();job["publicationPending"]=true;publishJob(job);publishSwitchBatch(stage,root,id,check);
   completed(job,receipt,root,"Ready in Emulation; game/update/DLC identified");
  },
  [](json& job,const std::exception& error){
   job["status"]=job.value("publicationPending",false)?"interrupted":cancelExtraction?"cancelled":"failed";
   job["error"]=error.what();job["phase"]=job.value("publicationPending",false)?"Switch publication retained; restart Botty to resume":"Switch preparation needs attention; sources kept";
  });
}

// Only completed, verified download members may be copied into Eden.
json emulationState(const json& downloads) {
  auto state=emulation.state();
  switchHome.enrich(state);
  const auto legacy=legacyEmulation.state();
  for(auto item:legacy.at("entries")){
    if(state["entries"].size()>=256)break;
    bool listed=false;for(const auto& known:state["entries"])if(known.at("path")==item.at("path"))listed=true;
    if(listed)continue;
    item["sourceKind"]=item.at("kind");item["source"]="existing-ps5";item["kind"]="download";
    item["id"]=nativeSha256("download:"+item.at("path").get<std::string>());
    state["entries"].push_back(std::move(item));
  }
  for(const auto& torrent:downloads) {
    if(torrent.value("leftUntilDone",uint64_t(1))!=0||torrent.value("error",0)!=0||torrent.value("remoteCandidate",false)||torrent.value("status",0)==1||torrent.value("status",0)==2)continue;
    try {
      const auto base=storage.get(storage.forDownload(torrent.at("downloadDir").get<std::string>())).complete;
      for(const auto& file:torrent.at("files")) {
        if(state["entries"].size()>=256)return state;
        const auto name=safeRelative(file.at("name").get<std::string>());
        if(!Emulation::supported(name)||file.value("bytesCompleted",uint64_t(0))!=file.at("length").get<uint64_t>())continue;
        try{auto item=Emulation::entry(base,base/name,"download");if(item.at("size")!=file.at("length"))continue;item["torrentId"]=torrent.at("id");state["entries"].push_back(item);}catch(...){}
      }
    }catch(...){}
  }
  return state;
}
// Shared durable lifecycle for file transfers. Callers hold admission's mutex;
// work reports whether a failed move has an uncertain, non-replayable outcome.
template<class Work>
void startTrackedTransfer(json initial,const std::string& name,Work work,bool notify=false) {
  const auto id=initial.at("id").is_string()?initial.at("id").get<std::string>():std::to_string(initial.at("id").get<int>());
  transfers.start(id,name,"transfer");transferState=std::move(initial);
  try{writeJson(paths.root/"transfer.json",transferState);}catch(...){transfers.finish(false,"Cannot save transfer state");throw;}
  transferring=true;
  try{std::thread([work=std::move(work),notify]()mutable{
    bool uncertain=false;
    try{
      const auto phase=work(uncertain);
      {std::lock_guard<std::mutex> guard(lock);transferState["status"]="complete";transferState["phase"]=phase;writeJson(paths.root/"transfer.json",transferState);transferring=false;transfers.finish(true);}
      if(notify)try{notifySystem("Botty+: Move completed. You can open Botty+.");}catch(...){}
    }catch(const std::exception& error){
      std::lock_guard<std::mutex> guard(lock);transferState["status"]=uncertain?"uncertain":"failed";
      if(!uncertain)transferring=false;
      transferState["error"]=error.what();transferState["phase"]="Transfer stopped; retained copies need inspection";
      try{writeJson(paths.root/"transfer.json",transferState);}catch(...){}
      transfers.finish(false,error.what());
    }
  }).detach();}catch(...){
    transferring=false;transferState["status"]="failed";transferState["error"]="Cannot start transfer worker";
    try{writeJson(paths.root/"transfer.json",transferState);}catch(...){}
    transfers.finish(false,"Cannot start transfer worker");throw;
  }
}
json importEmulation(const json& body) {
 const auto kind=body.value("kind",std::string("auto"));if(kind!="auto")throw std::runtime_error("Switch content type is detected automatically; refresh the client and choose Analyze and import");
 return switchPreparation.start({{"entry",body.at("id")}},true);
}
// Only explicitly requested Prowlarr downloads enter this durable queue.
fs::path automaticTaskPath(const std::string& hash,int id,bool isRemote){return paths.root/"automatic"/(isRemote?"remote-"+std::to_string(id)+".json":hash+".json");}
json queueDownload(const std::string& method,const json& arguments,const std::string& selected="internal",bool automatic=true,const std::string& engine="rtorrent",const std::string& platform="ps5"){
  if(platform!="ps5"&&platform!="switch")throw std::runtime_error("Choose PS5 or Switch preparation");
  std::lock_guard<std::mutex> guard(lock);if(transferring||retiring)throw std::runtime_error("Wait for the storage transfer to finish");
  nativeUpdater->requireAdmission();
  const auto selectedPaths=storage.get(selected);
  auto args=arguments;args["download-dir"]=selectedPaths.complete.string();args["storage"]=selected;args["engine"]=engine;args["automatic"]=automatic;args["platform"]=platform;
  auto result=rpc(method,args);
  auto torrent=result.contains("torrent-added")?result.at("torrent-added"):result.at("torrent-duplicate");
  auto hash=torrent.at("hashString").get<std::string>();
  if(!std::regex_match(hash,std::regex("[a-fA-F0-9]{40}")))throw std::runtime_error("Invalid torrent confirmation");
  const bool isRemote=remote&&remote->contains(torrent.at("id"));auto file=automaticTaskPath(hash,torrent.at("id"),isRemote);
  if(!result.contains("torrent-duplicate"))writeJson(file,{{"torrentId",torrent.at("id")},{"remote",isRemote},{"hash",hash},{"status",automatic?"waiting":"download-only"},{"storage",selected},{"platform",platform}});
  return result;
}
void automaticDownloads(){
  while(!retiring){
    std::this_thread::sleep_for(backgroundPollInterval());
    try{
      auto entries=torrents();
      try{localBackend().enforcePackagePolicy(entries);}catch(...){}
      {std::lock_guard<std::mutex> guard(lock);if(retiring||registeringLibrary||nativeUpdater->requested()||transferring||extracting||compressor.busy())continue;}
      for(const auto& file:fs::directory_iterator(paths.root/"automatic")){
        if(file.path().extension()!=".json")continue;
        auto task=json::parse(readText(file.path()));if(task.value("status","")!="waiting")continue;
        int sourceId=task.value("torrentId",-1);
        if(sourceId<0)for(const auto& t:entries)if(t.at("hashString")==task.at("hash")&&t.value("remote",false)==task.value("remote",false)&&!t.value("remoteCandidate",false)){sourceId=t.at("id");break;}
        bool existing=false;
        {std::lock_guard<std::mutex> guard(lock);for(const auto& job:jobs)if(job.value("automatic",false)&&job.at("hash")==task.at("hash")&&
          (job.contains("torrentId")?job.at("torrentId")==sourceId:!task.value("remote",false)))existing=true;}
        if(existing){task["status"]="tracked";writeJson(file.path(),task);continue;}
        for(const auto& torrent:entries){
          if(torrent.at("id")!=sourceId||torrent.at("hashString")!=task.at("hash")||torrent.value("leftUntilDone",1ULL)!=0||torrent.value("error",0)!=0||torrent.value("status",0)==1||torrent.value("status",0)==2)continue;
          const auto archives=archiveSets(torrent);
          const bool switchContent=task.value("platform",std::string("ps5"))=="switch"||switchTorrent(torrent);
          if(switchContent)task["platform"]="switch";
          if(archives.size()>1&&!switchContent){
            json job={{"id",randomId()},{"torrentId",sourceId},{"hash",task.at("hash")},{"remote",torrent.value("remote",false)},{"name",torrent.at("name")},{"automatic",true},{"status","failed"},{"phase","Needs attention"},{"error","Multiple RAR sets found. Choose the archive to prepare."}};
            publishJob(job);task["status"]="attention";writeJson(file.path(),task);break;
          }
          try{const auto job=switchContent?switchPreparation.start({{"id",torrent.at("id")}},true):startExtraction({{"id",torrent.at("id")},{"archive",archives.empty()?std::string{}:archives[0]},{"storage",task.value("storage",std::string("internal"))}},true);task["job"]=job.at("id");task["status"]="tracked";}
          catch(const PreparationBusy&){continue;}
          catch(const std::exception& e){json job={{"id",randomId()},{"torrentId",sourceId},{"hash",task.at("hash")},{"name",torrent.at("name")},{"automatic",true},{"status","failed"},{"phase","Needs attention"},{"error",e.what()}};publishJob(job);task["status"]="attention";}
          writeJson(file.path(),task);break;
        }
        {std::lock_guard<std::mutex> guard(lock);if(extracting)break;}
      }
    }catch(...){/* Service/rTorrent temporarily unavailable: retry without changing downloads. */}
  }
}
json inspectedLocalTorrents(bool allowEmptyOffline=true) {
  try{return localBackend().request("torrent-get",json::object()).at("torrents");}
  catch(...){
    if(!allowEmptyOffline)throw std::runtime_error("Local torrents cannot be inspected; files kept");
    const auto sessions=paths.root/"rtorrent/state/session";
    if(fs::exists(fs::symlink_status(sessions))){
      if(fs::is_symlink(fs::symlink_status(sessions))||!fs::is_directory(sessions))throw std::runtime_error("Local torrent sessions cannot be inspected; reconnect rTorrent before changing files");
      for(const auto& e:fs::directory_iterator(sessions))if(e.path().filename().string().find(".torrent")!=std::string::npos)throw std::runtime_error("Local rTorrent is unavailable with saved torrents; reconnect it to check shared files before changing files");
    }
    return json::array();
  }
}
std::set<fs::path> downloadReferences(const json& torrent,const std::vector<fs::path>& extra={}){
  if(torrent.value("remote",false))return remote->references(torrent);
  std::vector<fs::path> roots{fs::weakly_canonical(fs::path(torrent.at("downloadDir").get<std::string>()))};roots.insert(roots.end(),extra.begin(),extra.end());
  std::set<fs::path> result;for(const auto& f:torrent.at("files"))for(const auto& suffix:{"", ".part"})for(const auto& root:roots)result.insert(root/fs::path(safeRelative(f.at("name").get<std::string>()).string()+suffix));
  return result;
}
void requireUnsharedReferences(const std::set<fs::path>& selected,const json& references,int ignoredId,const std::vector<fs::path>& extra,const std::string& message){
  for(const auto& other:references)if(other.at("id")!=ignoredId)for(const auto& member:downloadReferences(other,extra))if(selected.count(member))throw std::runtime_error(message);
}
json startTransfer(const json& body) {
  std::lock_guard<std::mutex> guard(lock);
  nativeUpdater->requireAdmission();
  compressor.requireIdle();requireLibraryIdle();if(registeringLibrary||extracting||transferring)throw std::runtime_error("Wait for the current file operation");
  const auto kind=body.value("kind","job");
  if(kind!="torrent"&&kind!="job"&&kind!="publish")throw std::runtime_error("Invalid transfer kind");
  const auto selected=body.at("storage").get<std::string>();
  const auto destination=storage.get(selected);
  json job,torrent,ownership;
  std::string sourceId;
  if(kind=="torrent"){
    torrent=getTorrent(body.at("id").get<int>());if(torrent.value("remote",false)&&torrent.value("remotePhase","")!="ready")throw std::runtime_error("Finish the remote import before moving it");if(torrent.value("remote",false)){ownership=remote->published(torrent.at("id"));sourceId=ownership.at("storage").get<std::string>();}else sourceId=storage.forDownload(torrent.at("downloadDir").get<std::string>());
    if(torrent.value("status",0)==1||torrent.value("status",0)==2)throw std::runtime_error("Wait for torrent verification");
  }else{
    job=findJob(body.at("id").get<std::string>());
    if(job.value("platform",std::string{})=="switch")throw std::runtime_error("Switch content is managed in Emulation; PS5 Library transfers are unavailable for these imports");
    sourceId=job.value("storage","internal");
    const auto compressed=compressor.game(job.at("id").get<std::string>());
    if(compressed.value("status","")=="ready")sourceId=compressed.value("storage","internal");
    else if(compressor.protects(job.at("id").get<std::string>()))throw std::runtime_error("Resolve compression before transferring");
    if(job.value("status","")!="ready"&&job.value("status","")!="moved")throw std::runtime_error("Only completed content can be transferred");
    if(kind=="publish"&&job.value("status","")!="ready")throw std::runtime_error("Extraction is not ready");
  }
  const auto source=storage.get(sourceId);
  if(kind!="publish"&&selected==sourceId)throw std::runtime_error("Choose a different disk");
  startTrackedTransfer({{"status","running"},{"kind",kind},{"id",body.at("id")},{"storage",selected},{"sourceStorage",sourceId},{"phase","Copying; source kept until completion"}},kind=="torrent"?torrent.value("name","Torrent"):job.value("name","Game"),[kind,selected,sourceId,source,destination,job,torrent,ownership](bool& changed)mutable{
      const auto check=[&]{if(storage.get(sourceId,false).root!=source.root||storage.get(selected,false).root!=destination.root)throw std::runtime_error("Disk mount changed during transfer");if(!ownership.empty())RemoteDownloads::checkPublishedIdentity(source.complete/ownership.at("hash").get<std::string>(),ownership.at("identity"));};
      ExtractionEstimate estimate;uint64_t previousTotal=0;
      const auto copyProgress=[&](uint64_t bytes,uint64_t total,const std::string& file){if(total!=previousTotal){estimate=ExtractionEstimate{};previousTotal=total;}estimate.update(std::chrono::steady_clock::now(),bytes,total);transfers.progress({{"bytes",bytes},{"total",total},{"file",file},{"unit","bytes"},{"rate",estimate.rate},{"eta",estimate.eta},{"phase","Copying files; source kept until completion"}});};
      const auto remoteReport=[&](const json& task){
        auto progress=backgroundProgress(task);
        if(task.value("state","")=="transferring"){
          estimate.update(std::chrono::steady_clock::now(),progress.at("bytes").get<uint64_t>(),progress.at("total").get<uint64_t>());progress["eta"]=estimate.eta;
        }else estimate=ExtractionEstimate{};
        transfers.progress(progress);std::lock_guard<std::mutex> g(lock);transferState.update(progress);writeJson(paths.root/"transfer.json",transferState);
      };
      const auto waitForGame=[&](const std::string& title){
        CompressionLibrary library(source,shadowPort);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(5);
        while(!library.idle(title)){
          transfers.phase("Waiting for ShadowMount. This version may require closing Botty+.");
          if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("ShadowMount stayed busy or unavailable. Source kept; update ShadowMount to move games inside Botty+.");
          check();{std::lock_guard<std::mutex> g(lock);transferState["phase"]="Close Botty+ and games to transfer Library content";}
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      };
      if(kind=="torrent"){
        const auto hash=torrent.at("hashString").get<std::string>();
        rpc("torrent-close",{{"ids",{torrent.at("id")}}});
        if(getTorrent(torrent.at("id")).value("status",-1)!=0)throw std::runtime_error("Torrent did not stop; no files moved");
        std::vector<fs::path> members;
        uint64_t total=0;
        for(const auto& f:torrent.at("files")){
          const auto name=safeRelative(f.at("name").get<std::string>());
          if(!fs::exists(source.complete/name)){if(torrent.value("remote",false))throw std::runtime_error("Remote import member missing; source kept");continue;}
          containedExisting(source.complete,source.complete/name);
          if(fs::exists(fs::symlink_status(destination.complete/name)))throw std::runtime_error("Torrent destination already exists");
          const auto bytes=fs::file_size(source.complete/name);if(torrent.value("remote",false)&&bytes!=f.at("length").get<uint64_t>())throw std::runtime_error("Remote import member size changed; source kept");if(bytes>UINT64_MAX-total)throw std::runtime_error("Torrent is too large");total+=bytes;members.push_back(name);
        }
        if(total>UINT64_MAX-536870912ULL||freeBytes(destination.complete)<total+536870912ULL)throw std::runtime_error("Not enough destination space");
        // Inspect local sessions before copying and again before source cleanup.
        const auto requireUnshared=[&]{
          auto references=inspectedLocalTorrents(torrent.value("remote",false));if(remote)for(const auto& r:remote->list())references.push_back(r);
          std::set<fs::path> selectedPaths;for(const auto& member:members)selectedPaths.insert(source.complete/member);
          requireUnsharedReferences(selectedPaths,references,torrent.at("id"),{},"Torrent files are shared with another torrent");
        };
        requireUnshared();
        for(const auto& name:members){check();auto parent=destination.complete;for(const auto& part:name.parent_path()){parent/=part;if(fs::is_symlink(fs::symlink_status(parent)))throw std::runtime_error("Unsafe transfer destination");fs::create_directories(parent);}copyChecked(source.complete/name,destination.complete/name,check,copyProgress);changed=true;}
        check();rpc("torrent-set-location",{{"ids",{torrent.at("id")}},{"location",destination.complete.string()},{"storage",selected}});
        if(getTorrent(torrent.at("id")).at("downloadDir")!=destination.complete.string())throw std::runtime_error("Torrent location was not confirmed; both copies kept");
        const auto queue=automaticTaskPath(hash,torrent.at("id"),torrent.value("remote",false));
        if(fs::exists(queue)){auto task=json::parse(readText(queue));task["storage"]=selected;writeJson(queue,task);}
        requireUnshared();for(const auto& name:members){check();downloadedFiles(source.complete,{name},true);}
        if(torrent.value("remote",false)){check();removeEmptyRemoteRoot(source.complete,source.complete/hash);}
        // Remain paused: verification/resume is explicit and never races cleanup.
      }else if(kind=="publish"){
        resumeAdoptedOutput(source,job,check);changed=true;job["storage"]=selected;job=movePrepared(source,job,&destination,check);publishJob(job);
      }else if(compressor.game(job.at("id").get<std::string>()).value("status","")=="ready"){
        if(!BackgroundStorage(shadowPort).supported())waitForGame(job.at("content").value("titleId",""));
        changed=true;compressor.relocate(job.at("id").get<std::string>(),selected,check,remoteReport);
      }else{
        const auto id=job.at("id").get<std::string>();const bool moved=job.value("status","")=="moved";
        const auto name=moved?safeRelative(job.at("content").at("destination").get<std::string>()):fs::path(id);
        if(name.has_parent_path())throw std::runtime_error("Invalid tracked transfer path");
        const auto from=(moved?source.library:source.extracted)/name,to=(moved?destination.library:destination.extracted)/name;
        if(moved&&job.value("destination","")!=from.string())throw std::runtime_error("Tracked library location changed");
        containedExisting(moved?source.library:source.extracted,from);
        if(moved){
          CompressionLibrary library(source,shadowPort);
          library.api("manual/add",{{"path",from.string()}});library.api("scan",{{"reset_attempts",false}});
          std::string title=job.at("content").value("titleId","");
          if(title.empty()){
            for(int attempt=0;attempt<60&&title.empty();++attempt){
              check();const auto catalog=library.api("games",{{"include_size",false}});for(const auto& game:catalog.at("games"))if(game.value("path","")==from.string())title=game.value("title_id","");
              if(title.empty())std::this_thread::sleep_for(std::chrono::seconds(1));
            }
          }
          if(!std::regex_match(title,std::regex("(PPSA|CUSA)[0-9]{5}"))||title=="PPSA99071")throw std::runtime_error("ShadowMount must identify this game before transfer");
          job["content"]["titleId"]=title;
          BackgroundStorage background(shadowPort);
          if(background.supported()) {
            changed=true;
            try{background.run("move",title,from,to,remoteReport,check);}catch(const BackgroundRejected&){changed=false;throw;}
            check();job["storage"]=selected;job["destination"]=to.string();publishJob(job);
            return std::string("Move completed. Ready in Library.");
          }
          waitForGame(title);
        }
        copyChecked(from,to,check,copyProgress);changed=true;
        auto cleanup=from;
        if(moved){
          prepareLibraryPermissions(to);
          const auto backup=source.root/"transfers"/id;fs::create_directories(backup.parent_path());
          if(fs::exists(fs::symlink_status(backup)))throw std::runtime_error("Transfer backup already exists");
          {std::lock_guard<std::mutex> g(lock);transferState["backup"]=backup.string();transferState["destination"]=to.string();writeJson(paths.root/"transfer.json",transferState);}
          check();fs::rename(from,backup);cleanup=backup;
          CompressionLibrary library(source,shadowPort);library.api("manual/remove",{{"path",from.string()}});library.api("manual/add",{{"path",to.string()}});library.api("scan",{{"reset_attempts",false}});
          bool found=false;for(int n=0;n<60;++n){check();try{if(library.api("games/info",{{"title_id",job.at("content").value("titleId","")}}).value("path","")==to.string()){found=true;break;}}catch(...){}std::this_thread::sleep_for(std::chrono::seconds(1));}
          if(!found)throw std::runtime_error("New Library source not confirmed; copy and backup kept");
        }
        check();job["storage"]=selected;if(moved)job["destination"]=to.string();publishJob(job);removeTransferred(cleanup);
      }
      return std::string("Transfer complete; sizes checked");
  },true);return transferState;
}

void reply(httplib::Response& response,const json& body,int status=200){response.status=status;response.set_content(body.dump(),"application/json");}
}
int main(int argc,char** argv) {
#ifdef __PS5__
  (void)argc;(void)argv;
#endif
  const char* stage="entering main";
  try {
    std::cerr<<"Botty main entered\n";
#ifndef __PS5__
    // Native development/test mode is explicitly isolated by command-line paths.
    for(int i=1;i<argc;i++) {
      std::string arg=argv[i];if(i+1>=argc)throw std::runtime_error("Missing argument");
      if(arg=="--root"){auto root=fs::path(argv[++i]);paths=Paths(root,root/"test-library");}
      else if(arg=="--external")testMounts.emplace_back(argv[++i]);
      else if(arg=="--shadow-port")shadowPort=std::stoi(argv[++i]);
      else if(arg=="--compressor-port")compressorPort=std::stoi(argv[++i]);
      else if(arg=="--ui")uiDir=argv[++i];else if(arg=="--port")port=std::stoi(argv[++i]);else if(arg=="--rpc-port")rpcPort=std::stoi(argv[++i]);else throw std::runtime_error("Unknown argument");
    }
    if(paths.root=="/data/botty")throw std::runtime_error("Native testing requires --root");
    emulation=Emulation(paths.root/"eden",paths.root/"eden-app");
    switchHome=SwitchHome(paths.root/"homebrew",paths.root/"eden-app",paths.root/"appmeta");
    legacyEmulation=Emulation(paths.root/"legacy-eden",paths.root/"legacy-eden-app");
#endif
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("HTTPS initialization failed");
    umask(0077);signal(SIGPIPE,SIG_IGN);
    stage="creating working directories";
    fs::create_directories(paths.root/"automatic");fs::create_directories(paths.jobs);fs::create_directories(paths.extracted);fs::create_directories(paths.complete);
    stage="locking the manager";
    const auto lockPath=paths.root/"manager.lock";
    const int fd=open(lockPath.c_str(),O_WRONLY|O_CREAT|O_NOFOLLOW,0600);
    if(fd<0 || flock(fd,LOCK_EX|LOCK_NB))throw std::runtime_error("Botty is already running or its lock is unavailable");
    stage="loading saved jobs";
    storage.init(paths,testMounts);storage.list();
    token=randomId();recoverJobs();compressor.init(paths,compressorPort,&storage,shadowPort);
    if(fs::exists(paths.root/"transfer.json")){transferState=json::parse(readText(paths.root/"transfer.json"));if(transferState.value("status","")=="running"||transferState.value("status","")=="uncertain"){transferState["status"]="uncertain";transferState["error"]="Interrupted file operation. Check retained files and the ShadowMount job before retrying.";transferring=true;auto& monitor=transferState.value("kind","")=="deletion"?operations:transfers;monitor.start(transferState.value("id",std::string("interrupted")),"Interrupted file operation",transferState.value("kind","")=="deletion"?"deletion":"transfer");monitor.progress(transferState);monitor.finish(false,transferState.at("error"));}}
    writeJson(paths.root/"manager-process.json",{{"pid",getpid()},{"version","1.10.5"}});
    NativeTransaction::Config nativeConfig;
#ifndef __PS5__
    nativeConfig.nativeRoot=paths.root/"native-test/PPSA99071";
    nativeConfig.stateRoot=paths.root/"native";
    for(size_t i=0;i<nativeConfig.metadataRoots.size();++i)nativeConfig.metadataRoots[i]=paths.root/"native-test"/("metadata-"+std::to_string(i));
#endif
    nativeTransaction=std::make_unique<NativeTransaction>(nativeConfig);
    InstallationUpdater::Config updaterConfig;
    updaterConfig.root=paths.root;
    updaterConfig.managerVersion="1.10.5";
    updaterConfig.download=[](const std::string& file,size_t limit,const std::atomic<bool>& cancelled){return nativeDownload(file,limit,fs::path(uiDir).parent_path()/"cacert.pem",&cancelled);};
    updaterConfig.inspect=[]{installationAwaitHttpReady(httpReady,retiring);static Rtorrent engine(paths,rpcPort);return inspectInstallation(paths.root,engine);};
    updaterConfig.owns= [](const json& handoff){return installationHandoffOwned(paths.root,handoff,installationBootId());};
    updaterConfig.idle=[]{
        {std::lock_guard<std::mutex> guard(lock);if(retiring||registeringLibrary||extracting||transferring||(remote&&remote->busy()))throw std::runtime_error("Waiting for extraction, transfer or deletion to finish.");search.requireInstallationIdle();explore.requireInstallationIdle();}
        compressor.confirmNativeUpdateIdle();
        std::lock_guard<std::mutex> guard(lock);if(retiring||registeringLibrary||extracting||transferring||(remote&&remote->busy())||compressor.busy())throw std::runtime_error("Waiting for all file operations to finish.");
      };
    updaterConfig.stopped=[]{confirmNativeProcessesStopped();};
    updaterConfig.launch=[](const json&){launchEmbeddedServiceUpdater();};
    nativeUpdater=std::make_unique<InstallationUpdater>(*nativeTransaction,std::move(updaterConfig));
    if(fs::exists(paths.root/"remote.json"))try{
      remote=std::make_unique<RemoteDownloads>(paths,[](const std::string& id,const fs::path& directory){if(storage.get(id,false).complete!=directory)throw std::runtime_error("Remote destination storage changed");},[]{return !retiring&&!transferring&&nativeUpdater&&!nativeUpdater->requested();},[](const fs::path& stage,const fs::path& target,const json& inventory){
        std::set<fs::path> selected;
        for(const auto& f:inventory)for(const auto& root:{stage,target})selected.insert(root/safeRelative(f.at("name").get<std::string>()));
        requireUnsharedReferences(selected,inspectedLocalTorrents(),-1,{},"Remote import files are shared with a local torrent; pause and relocate that torrent before importing");
      });
    }catch(const std::exception& e){remoteError=e.what();}
    nativeUpdater->start();
    if(remote)remote->start();
    stage="creating HTTP server";
    RestCompanionAuthorization restAuthorization(paths.root);
    auto restDiagnostics=std::make_shared<RestModeDiagnostics>(paths.root);
    RestModeKeeper restMode(currentRestModeSupported(),requestRestMode,[restDiagnostics](const json& state){restDiagnostics->observe(state);});
    auto restState=[&]{auto state=restMode.state();state["companion"]=restDiagnostics->companion();return state;};
    httplib::Server server;server.set_payload_max_length(2*1024*1024);
    ServerThreads serverThreads(server);
    server.set_read_timeout(5);server.set_write_timeout(10);
    const auto origin="http://127.0.0.1:"+std::to_string(port);
    server.set_pre_routing_handler([&](const httplib::Request& req,httplib::Response& res){
      res.set_header("Cache-Control","no-store");res.set_header("X-Content-Type-Options","nosniff");
      res.set_header("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'");
      const auto host=req.get_header_value("Host"), source=req.get_header_value("Origin");
      const bool local=req.remote_addr=="127.0.0.1" && host=="127.0.0.1:"+std::to_string(port);
      const auto lan=connectionUrl();
      const bool remote=!lan.empty() && "http://"+host==lan && req.remote_addr.rfind("192.168.",0)==0;
      if((!local&&!remote) || (!source.empty() && source!=(local?origin:lan))) {
        reply(res,{{"error","Only the local Botty application can use this service"}},403);return httplib::Server::HandlerResponse::Handled;
      }
      if(!local) {
        const auto credentials=json::parse(readText(paths.root/"rtorrent/state/botty-credentials.json",4096));
        const auto expected="Basic "+httplib::detail::base64_encode(credentials.at("username").get<std::string>()+":"+credentials.at("password").get<std::string>());
        if(req.get_header_value("Authorization")!=expected){res.set_header("WWW-Authenticate","Basic realm=\"Botty\"");reply(res,{{"error","Authentication required"}},401);return httplib::Server::HandlerResponse::Handled;}
      }
      if(req.path=="/api/installation/retire"||req.path=="/api/installation/status") {
        const auto method=req.path=="/api/installation/retire"?"POST":"GET";
        if(!local||req.method!=method||!source.empty()){reply(res,{{"error","Private loopback updater endpoint only"}},403);return httplib::Server::HandlerResponse::Handled;}
        return httplib::Server::HandlerResponse::Unhandled;
      }
      if(req.path.rfind("/api/",0)==0 && req.path!="/api/bootstrap" && req.get_header_value("X-Botty-Token")!=token) {
        reply(res,{{"error","Reload Botty to reconnect"}},403);return httplib::Server::HandlerResponse::Handled;
      }
      if(retiring&&req.method=="POST"){reply(res,{{"error","Botty is shutting down"}},503);return httplib::Server::HandlerResponse::Handled;}
      return httplib::Server::HandlerResponse::Unhandled;
    });
    // Installed native clients require the original flat health contract.
    // Rest-mode details remain available in /api/rest-mode and /api/state.
    server.Get("/health",[](const auto&,auto& res){reply(res,{{"app","Botty"},{"version","1.10.5"},{"titleId","BTTY00001"},{"apiVersion",1}});});
    server.Get("/api/installation/status",[](const auto&,auto& res){
      const auto update=nativeUpdater->state();
      reply(res,{{"app","Botty"},{"version","1.10.5"},{"titleId","BTTY00001"},{"apiVersion",1},{"updaterVersion","1.0.0"},{"pid",getpid()},{"updateQueued",nativeUpdater->requested()},{"updateTransaction",update.value("transaction","")},{"updaterThreadFinished",nativeUpdater->finished()}});
    });
    server.Post("/api/installation/retire",[&](const auto& req,auto& res){
      const auto body=json::parse(req.body);
      if(!body.is_object()||body.size()!=1||!body.contains("transaction")||!installationRetireAuthorized(paths.root,body.at("transaction").template get<std::string>(),req.get_header_value("X-Botty-Update-Token")))throw std::runtime_error("Durable updater takeover authorization failed.");
      if(!nativeUpdater->finished())throw std::runtime_error("Updater handoff thread has not returned.");
      compressor.confirmNativeUpdateIdle();
      {std::lock_guard<std::mutex> guard(lock);if(retiring||registeringLibrary||transferring||extracting||(remote&&remote->busy())||compressor.busy())throw std::runtime_error("Existing file operations must finish before manager retirement.");search.requireInstallationIdle();explore.requireInstallationIdle();retiring=true;}
      reply(res,{{"status","retiring"},{"pid",getpid()},{"transaction",body.at("transaction")}});
      serverThreads.stop();
    });
    server.Post("/api/native-update",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);if(!body.is_object()||!body.empty())throw std::runtime_error("Native update accepts an empty object only.");
      std::lock_guard<std::mutex> guard(lock);if(retiring)throw std::runtime_error("Botty is shutting down");reply(res,nativeUpdater->request(),202);
    });
    server.Post("/api/native-update/check",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);if(!body.is_object()||!body.empty())throw std::runtime_error("Native update check accepts an empty object only.");
      nativeUpdater->recheck();reply(res,json::object(),202);
    });
    server.Get("/api/rest-mode",[&restState](const auto&,auto& res){reply(res,restState());});
    server.Get("/api/bootstrap",[](const auto&,auto& res){reply(res,{{"token",token},{"apiVersion",1}});});
    // Explicit local, token-authenticated disclosure for the console UI only.
    server.Get("/api/connections",[](const auto&,auto& res){
      (void)rpc("session-get"); // Do not display unverified credentials as usable.
      const auto credentials=json::parse(readText(paths.root/"rtorrent/state/botty-credentials.json",4096));
      reply(res,{{"apiVersion",1},{"url",connectionUrl()},
        {"username",credentials.at("username")},{"password",credentials.at("password")}});
    });
    server.Get("/api/storage",[](const auto&,auto& res){reply(res,{{"storage",storage.list()}});});
    server.Get("/api/processing",[](const auto&,auto& res){
      auto tasks=json::array();const auto deletion=operations.state();
      if(deletion.value("status","")!="idle")tasks.push_back(deletion);
      const auto transfer=transfers.state();if(transfer.value("status","")!="idle")tasks.push_back(transfer);
      auto compression=compressor.state();
      if(compression.contains("jobId")){
        compression["id"]=compression.at("jobId");compression["name"]=compression.value("name",compression.value("titleId","Game"));
        compression["kind"]=(compression.value("deleteRequested",false)||compression.value("deleteGameRequested",false))?"deletion":"compression";
        bool duplicate=false;for(const auto& task:tasks)if(task.value("id","")==compression.value("id",""))duplicate=true;
        if(!duplicate)tasks.push_back(compression);
      }
      reply(res,{{"tasks",tasks}});
    });
    server.Get("/api/state",[&restState](const auto&,auto& res){
      json result={{"storage",storage.list()},{"storageSupported",true},{"search",search.state()},{"searchSupported",true},{"extractionControls",true},{"libraryDeletionSupported",true},{"freeBytes",freeBytes(paths.root)},{"library",paths.library.string()}};
      try{result["torrents"]=torrents();result["transmissionReady"]=true;result["torrentEngine"]=remote?"qbittorrent+rtorrent":"rtorrent";}catch(const std::exception& error){result["torrents"]=json::array();result["transmissionReady"]=false;result["error"]=error.what();}
      {std::lock_guard<std::mutex> guard(lock);result["jobs"]=jobs;result["extracting"]=extracting||transferring;result["transfer"]=transferState;}
      for(auto& torrent:result["torrents"])try{torrent["storage"]=storage.forDownload(torrent.at("downloadDir").get<std::string>());}catch(...){torrent["storage"]="unavailable";}
      result["preparationSupported"]=true;
      for(auto& torrent:result["torrents"]){
        if(switchTorrent(torrent))torrent["emulationContent"]=true;
        for(const auto& f:torrent.value("files",json::array()))if(Emulation::supported(fs::path(f.at("name").get<std::string>())))torrent["emulationContent"]=true;
        bool loose=false;for(const auto& f:torrent.value("files",json::array())){const fs::path name=f.at("name").get<std::string>();const auto ext=asciiLower(name.extension().string());loose|=ext==".exfat"||ext==".ffpkg"||ext==".ffpfs"||ext==".ffpfsc"||(name.filename()=="param.json"&&name.parent_path().filename()=="sce_sys");}
        torrent["looseContent"]=loose&&archiveSets(torrent).empty()&&(!packageTorrent(torrent)||torrent.value("remote",false));
        std::string notes;
        if(torrent.value("leftUntilDone",uint64_t(1))==0&&!torrent.value("adopted",false))try{
          const auto selected=storage.get(torrent.value("storage",std::string("internal")),false);
          for(const auto& f:torrent.value("files",json::array()))if(asciiLower(fs::path(f.at("name").get<std::string>()).extension().string())==".nfo"){
            const auto member=containedExisting(selected.complete,selected.complete/safeRelative(f.at("name")));
            notes=readText(member,65536);break;
          }
        }catch(...){}
        torrent["formatHint"]=releaseFormatHint(torrent.value("name",std::string{}),notes);
      }
      result["emulation"]=emulationState(result["torrents"]);
      result["remoteDownloads"]=remote?remote->state():json{{"enabled",false},{"error",remoteError}};
      result["restMode"]=restState();
      explore.registerCatalogArtwork(result["torrents"],result["jobs"]);
      result["compression"]=compressor.state();
      result["nativeUpdate"]=nativeUpdater->state();
      for(auto& job:result["jobs"]){const auto c=compressor.game(job.value("id",""));job["compression"]=c;if(c.value("status","")=="ready"){job["destination"]=c.at("output");job["storage"]=c.value("storage","internal");job["content"]["kind"]="compressed";}}
      result["catalogArtworkSupported"]=true;
      std::set<std::string> owned;
      for(const auto& item:result["torrents"])if(!item.value("remoteCandidate",false))owned.insert(Search::gameKey(item.value("name","")));
      for(const auto& item:result["jobs"])if(item.value("status","")=="ready"||item.value("status","")=="moved"||item.value("status","")=="extracting")owned.insert(Search::gameKey(item.value("name","")));
      // Read only native metadata: never recursively walk large game content.
      try{unsigned count=0;for(const auto& item:fs::directory_iterator(paths.library)){
        if(++count>2048)break;const auto param=item.path()/"sce_sys/param.json";
        try{auto data=json::parse(readText(containedExisting(paths.library,param),65536));auto labels=data.at("localizedParameters");auto language=labels.value("defaultLanguage","en-US");owned.insert(Search::gameKey(labels.at(language).at("titleName").get<std::string>()));}catch(...){}
      }}catch(...){}
      result["torrentRemovalSupported"]=true;result["exploreSupported"]=true;result["explore"]=explore.state(owned);
      reply(res,result);
    });
    server.Get("/api/explore/artwork",[](const auto& req,auto& res){std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();const auto kind=req.has_param("kind")?req.get_param_value("kind"):"cover";auto data=explore.artwork(paths,req.get_param_value("id"),false,kind);if(data=="pending"){res.status=202;res.set_content("Pending","text/plain");}else if(data.empty()){res.status=404;res.set_content("Unavailable","text/plain");}else res.set_content(data,"application/octet-stream");});
    // Resolve only known local entries or search IDs, never client-supplied URLs/titles.
    server.Get("/api/artwork",[](const auto& req,auto& res){
      std::unique_lock<std::mutex> guard(lock);nativeUpdater->requireAdmission();
      const auto id=req.get_param_value("id");
      const auto kind=req.has_param("kind")?req.get_param_value("kind"):"cover";
      if(kind!="cover"&&kind!="hero")throw std::runtime_error("Invalid artwork kind");
      std::string data;
      if(id.rfind("m:",0)==0){
        guard.unlock();auto title=emulation.artworkTitle(id.substr(2));if(title.empty())title=legacyEmulation.artworkTitle(id.substr(2));
        if(!title.empty())data=search.artworkTitle(paths,title,kind);
      }
      else data=id.rfind("s:",0)==0?search.artwork(paths,id.substr(2),false,kind):explore.artwork(paths,id,true,kind);
      if(data=="pending"){res.status=202;res.set_content("Pending","text/plain");}
      else if(data.empty()){res.status=404;res.set_content("Unavailable","text/plain");}
      else res.set_content(data,"application/octet-stream");
    });
    server.Post("/api/explore",[](const auto& req,auto& res){std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();const auto sort=json::parse(req.body).at("sort").template get<std::string>();if(sort.empty())throw std::runtime_error("Choose an Explore sort");explore.start(paths,"PS5",sort,json::parse(req.body).value("refresh",false));reply(res,{{"ok",true}},202);});
    server.Post("/api/explore/add",[](const auto& req,auto& res){std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();const auto body=json::parse(req.body);const auto selected=body.value("storage","internal");storage.get(selected);const bool automatic=body.value("automatic",true);const auto engine=selectedDownloadEngine(body);const auto platform=body.value("platform",std::string("ps5"));explore.add(paths,body.at("id").template get<std::string>(),[selected,automatic,engine,platform](const std::string& method,const json& args){return queueDownload(method,args,selected,automatic,engine,platform);});reply(res,{{"ok",true}},202);});
    server.Post("/api/emulation/import",[](const auto& req,auto& res){reply(res,importEmulation(json::parse(req.body)),202);});
    server.Post("/api/emulation/home",[](const auto& req,auto& res){
      std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();
      if(transferring||extracting||retiring||compressor.busy())throw std::runtime_error("Wait for the current file operation");
      const auto body=json::parse(req.body);
      if(!body.is_object()||body.size()!=1||!body.contains("id")||!body.at("id").is_string())throw std::runtime_error("Choose a Switch library game");
      reply(res,switchHome.add(emulation,body.at("id").template get<std::string>()));
    });
    server.Post("/api/emulation/launch",[](const auto& req,auto& res){
      std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();
      if(transferring||extracting||retiring)throw std::runtime_error("Wait for the current file operation");
      const auto path=emulation.launchPath(json::parse(req.body).at("id").template get<std::string>());
      reply(res,{{"ok",true},{"launchPath",path},{"engine","embedded-eden"}});
    });
    server.Post("/api/search",[](const auto& req,auto& res){std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();search.start(paths,json::parse(req.body).at("query").template get<std::string>());reply(res,{{"ok",true}},202);});
    server.Post("/api/search/add",[](const auto& req,auto& res){std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();const auto body=json::parse(req.body);const auto selected=body.value("storage","internal");storage.get(selected);const bool automatic=body.value("automatic",true);const auto engine=selectedDownloadEngine(body);const auto platform=body.value("platform",std::string("ps5"));search.add(paths,body.at("id").template get<std::string>(),[selected,automatic,engine,platform](const std::string& method,const json& args){return queueDownload(method,args,selected,automatic,engine,platform);});reply(res,{{"ok",true}},202);});
    server.Post("/api/torrent",[](const auto& req,auto& res){
      auto body=json::parse(req.body);const auto action=body.at("action").template get<std::string>();
      if(action=="add") {
        if(body.contains("metainfo")) {
          if(body.contains("magnet"))throw std::runtime_error("Choose a magnet link or a torrent file");
          const auto metainfo=body.at("metainfo").template get<std::string>();
          if(metainfo.empty()||metainfo.size()>1398104)throw std::runtime_error("Torrent files must be between 1 byte and 1 MiB");
          reply(res,queueDownload("torrent-add",{{"metainfo",metainfo}},body.value("storage","internal"),body.value("automatic",false),body.value("engine",std::string("rtorrent")),body.value("platform",std::string("ps5"))));return;
        }
        const auto magnet=body.at("magnet").template get<std::string>();
        if(magnet.rfind("magnet:?",0)!=0 || magnet.size()>16384)throw std::runtime_error("Enter a valid magnet link");
        reply(res,queueDownload("torrent-add",{{"filename",magnet}},body.value("storage","internal"),body.value("automatic",false),body.value("engine",std::string("rtorrent")),body.value("platform",std::string("ps5"))));return;
      }
      if(transferring)throw std::runtime_error("Wait for the storage transfer to finish");
      const int id=body.at("id").template get<int>();
      if(id<0)throw std::runtime_error("Invalid torrent ID");
      if(action=="remove-data") {
        if(!body.value("confirmed",false))throw std::runtime_error("Confirm deletion of the torrent and downloaded files");
        std::lock_guard<std::mutex> guard(lock);
        nativeUpdater->requireAdmission();
        compressor.requireIdle();requireLibraryIdle();
        if(registeringLibrary||transferring||extracting)throw std::runtime_error("Wait for extraction to finish before deleting archives");
        const auto torrent=getTorrent(id);
        if(torrent.value("remote",false)){
          if(torrent.value("adopted",false))for(const auto& job:jobs)if(job.value("remote",false)&&job.value("torrentId",-1)==id&&job.value("adoptedImport",false)&&job.value("status","")!="moved")throw std::runtime_error("Publish the imported game to Library before removing its tracking record");
          const auto hash=torrent.at("hashString").template get<std::string>();
          const auto selected=downloadReferences(torrent);
          requireUnsharedReferences(selected,inspectedLocalTorrents(),-1,{},"Downloaded files are shared with another torrent; deletion refused");
          writeJson(automaticTaskPath(hash,id,true),{{"torrentId",id},{"remote",true},{"hash",hash},{"status","deletion-requested"}});remote->removeData(id);reply(res,{{"ok",true}});return;}
        DeletionScope operation(operations,std::to_string(id),torrent.value("name","Download"));
        const auto downloadRoot=storage.get(storage.forDownload(torrent.at("downloadDir").template get<std::string>())).root/"downloads";
        const auto directory=containedExisting(downloadRoot,torrent.at("downloadDir").template get<std::string>());
        std::vector<fs::path> locations{directory};
        const auto session=rpc("session-get");
        if(session.value("incomplete-dir-enabled",false))locations.push_back(containedExisting(downloadRoot,session.at("incomplete-dir").template get<std::string>()));
        std::vector<fs::path> members;
        for(const auto& file:torrent.at("files")){
          const auto relative=safeRelative(file.at("name").template get<std::string>());
          for(const auto& suffix:{"", ".part"})members.emplace_back(relative.string()+suffix);
        }
        for(const auto& location:locations)downloadedFiles(location,members,false);
        const auto hash=torrent.at("hashString").template get<std::string>();
        if(!std::regex_match(hash,std::regex("[a-fA-F0-9]{40}")))throw std::runtime_error("Invalid torrent identity");
        // Two torrents can refer to the same pathname. Do not remove another
        // torrent's files, including incomplete files in the shared directory.
        std::set<fs::path> selectedPaths;
        for(const auto& location:locations)for(const auto& member:members)selectedPaths.insert(location/member);
        auto references=inspectedLocalTorrents(false);if(remote)for(const auto& r:remote->list())references.push_back(r);
        std::vector<fs::path> extra;if(locations.size()>1)extra.push_back(locations[1]);
        requireUnsharedReferences(selectedPaths,references,torrent.at("id"),extra,"Downloaded files are shared with another torrent; deletion refused");
        const auto queue=automaticTaskPath(hash,torrent.at("id"),torrent.value("remote",false));
        if(fs::exists(queue))writeJson(queue,{{"hash",hash},{"status","deletion-requested"}});
        rpc("torrent-stop",{{"ids",{torrent.at("id")}}});
        bool stopped=false;
        for(int attempt=0;attempt<20;++attempt) {
          for(const auto& item:torrents())if(item.at("id")==torrent.at("id"))stopped=item.value("status",-1)==0;
          if(stopped)break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if(!stopped)throw std::runtime_error("Torrent did not stop; no files were deleted");
        // The torrent RPC success only acknowledges the removal request.
        // Unlink and verify exact members ourselves before discarding metadata,
        // so a filesystem failure leaves a paused torrent that can be retried.
        for(const auto& location:locations)downloadedFiles(location,members,false);
        for(const auto& location:locations)downloadedFiles(location,members,true,operation.reporter());
        const auto removed=rpc("torrent-remove",{{"ids",{torrent.at("id")}},{"delete-local-data",false}});operation.complete();reply(res,removed);return;
      }
      const std::map<std::string,std::string> methods={{"pause","torrent-stop"},{"resume","torrent-start"},{"verify","torrent-verify"}};
      const auto method=methods.find(action);if(method==methods.end())throw std::runtime_error("Unsupported torrent action");
      std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();
      if(action=="resume"||action=="verify")storage.get(storage.forDownload(getTorrent(id).at("downloadDir").template get<std::string>()));
      json arguments={{"ids",{id}}};if(body.contains("parallelConnections"))arguments["parallelConnections"]=body.at("parallelConnections");reply(res,rpc(method->second,arguments));
    });
    server.Post("/api/remote/import",[](const auto& req,auto& res){
      auto body=json::parse(req.body);std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();
      if(!remote||transferring||retiring)throw std::runtime_error("Remote downloads unavailable or storage transfer active");
      const auto hash=body.contains("hash")?body.at("hash").template get<std::string>():remote->candidateHash(body.at("id").template get<int>());if(!std::regex_match(hash,std::regex("[a-f0-9]{40}")))throw std::runtime_error("Invalid remote hash");
      const auto selected=body.value("storage",std::string("internal"));auto selectedPaths=storage.get(selected);
      std::string detected="ps5";for(const auto& candidate:torrents())if(candidate.at("hashString")==hash&&switchTorrent(candidate)){detected="switch";break;}
      const auto platform=body.value("platform",detected);if(platform!="switch"&&platform!="ps5")throw std::runtime_error("Invalid preparation platform");
      json arguments={{"download-dir",selectedPaths.complete.string()},{"storage",selected},{"platform",platform},{"automatic",body.value("automatic",true)}};if(body.contains("parallelConnections"))arguments["parallelConnections"]=body.at("parallelConnections");
      const auto result=remote->add(hash,arguments,true);
      if(result.contains("torrent-added"))writeJson(automaticTaskPath(hash,result.at("torrent-added").at("id"),true),{{"torrentId",result.at("torrent-added").at("id")},{"remote",true},{"hash",hash},{"status",body.value("automatic",true)?"waiting":"download-only"},{"storage",selected},{"platform",platform}});
      reply(res,result,202);
    });
    server.Post("/api/extract",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);const auto torrent=getTorrent(body.at("id"));
      if(torrent.value("remote",false)&&torrent.value("remotePhase","")=="remote"&&torrent.contains("remotePreparation")){
        std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();
        const auto selected=body.value("storage",torrent.value("storage",std::string("internal")));
        if(selected!=torrent.value("storage",std::string("internal")))throw std::runtime_error("Keep the import destination chosen at download time");
        rpc("torrent-prepare",{{"ids",{torrent.at("id")}},{"package",body.value("archive",std::string{})}});
        writeJson(automaticTaskPath(torrent.at("hashString"),torrent.at("id"),true),{{"torrentId",torrent.at("id")},{"remote",true},{"hash",torrent.at("hashString")},{"status","waiting"},{"platform",switchTorrent(torrent)?"switch":"ps5"},{"storage",selected}});
        reply(res,{{"ok",true}},202);return;
      }
      if(body.value("platform",std::string{})=="switch"||switchTorrent(torrent)){reply(res,switchPreparation.start(body,true),202);return;}
      reply(res,startExtraction(body,body.value("automatic",false)),202);
    });
    server.Post("/api/move",[](const auto& req,auto& res){
      auto body=json::parse(req.body);
      if(!body.contains("storage")){std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();compressor.requireIdle();requireLibraryIdle();if(registeringLibrary||extracting||transferring)throw std::runtime_error("Wait for the active file operation");auto job=findJob(body.at("id").template get<std::string>());const auto selected=storage.get(job.value("storage","internal"));resumeAdoptedOutput(selected,job);job=movePrepared(selected,job);for(auto& old:jobs)if(old.at("id")==job.at("id"))old=job;reply(res,job);return;}
      body["kind"]="publish";reply(res,startTransfer(body),202);
    });
    server.Post("/api/transfer",[](const auto& req,auto& res){reply(res,startTransfer(json::parse(req.body)),202);});
    server.Post("/api/cancel-extraction",[](const auto& req,auto& res){
      const auto id=json::parse(req.body).at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);const auto job=findJob(id);
      if(!extracting || activeJob!=id || job.value("status","")!="extracting")throw std::runtime_error("This extraction is no longer running");
      cancelExtraction=true;reply(res,{{"ok",true},{"cancelRequested",true}},202);
    });
    server.Post("/api/dismiss-extraction",[](const auto& req,auto& res){
      const auto id=json::parse(req.body).at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();requireLibraryIdle();if(extracting||transferring)throw std::runtime_error("Wait for the current file operation to finish");auto job=findJob(id);const auto status=job.value("status","");
      if(status!="moved"&&job.value("adoptedImport",false))throw std::runtime_error("Publish the imported game to Library before removing its preparation record");
      if(status!="ready"&&status!="moved"&&status!="failed"&&status!="cancelled"&&status!="interrupted")throw std::runtime_error("Only finished extractions can be removed from the list");
      if(status=="failed"||status=="cancelled"||status=="interrupted"){
        if(job.value("platform",std::string{})=="switch")switchPreparation.removePartial(job);
        DeletionScope operation(operations,id,job.value("name","Partial extraction"));
        for(const auto& suffix:{"", ".working"}){const auto selected=storage.get(job.value("storage","internal"));const auto path=selected.extracted/(id+suffix);if(fs::exists(fs::symlink_status(path)))deleteGameDirectory(selected.extracted,path.filename(),operation.reporter());}
        operation.complete();
      }
      job["dismissed"]=true;writeJson(paths.jobs/(id+".json"),job);
      for(auto& old:jobs)if(old.at("id")==id){old=job;break;}
      reply(res,{{"ok",true}});
    });
    server.Post("/api/beta/shutdown",[&](const auto& req,auto& res){
      if(!json::parse(req.body).value("confirmed",false))throw std::runtime_error("Confirm beta shutdown");
      std::lock_guard<std::mutex> guard(lock);
      nativeUpdater->requireAdmission();
      if(registeringLibrary||transferring||extracting)throw std::runtime_error("Wait for extraction to finish");
      compressor.stopWorker();retiring=true;reply(res,{{"ok",true}});
      serverThreads.stop();
    });
    server.Post("/api/compress-game",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);
      if(!body.value("confirmed",false))throw std::runtime_error("Confirm creating a separate compressed copy");
      std::lock_guard<std::mutex> guard(lock);
      nativeUpdater->requireAdmission();
      if(registeringLibrary||transferring||extracting)throw std::runtime_error("Wait for extraction to finish");
      reply(res,compressor.start(findJob(body.at("id").template get<std::string>()),body.value("storage","")),202);
    });
    server.Post("/api/restore-uncompressed",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);if(!body.value("confirmed",false))throw std::runtime_error("Confirm restoring the original game");
      std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();if(registeringLibrary||transferring||extracting)throw std::runtime_error("Wait for extraction to finish");
      const auto id=body.at("id").template get<std::string>();findJob(id);reply(res,compressor.requestRestore(id),202);
    });
    server.Post("/api/delete-uncompressed",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);if(!body.value("confirmed",false))throw std::runtime_error("Confirm that you tested the compressed game before deleting its original");
      std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();if(registeringLibrary||transferring||extracting)throw std::runtime_error("Wait for extraction to finish");
      const auto id=body.at("id").template get<std::string>();findJob(id);reply(res,compressor.requestOriginalDeletion(id),202);
    });
    server.Post("/api/verify-compressed",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);std::lock_guard<std::mutex> guard(lock);
      nativeUpdater->requireAdmission();
      if(registeringLibrary||extracting||transferring)throw std::runtime_error("Wait for the current file operation");
      const auto id=body.at("id").template get<std::string>();findJob(id);reply(res,compressor.requestVerification(id),202);
    });
    server.Post("/api/skip-verification",[](const auto& req,auto& res){reply(res,compressor.skipVerification(json::parse(req.body).at("id").template get<std::string>()),202);});
    server.Post("/api/cancel-compression",[](const auto& req,auto& res){reply(res,compressor.cancel(json::parse(req.body).at("id").template get<std::string>()),202);});
    server.Post("/api/delete-library-game",[](const auto& req,auto& res){
      const auto request=json::parse(req.body);
      if(!request.value("confirmed",false))throw std::runtime_error("Confirm deletion of the installed game; archives are kept");
      const auto id=request.at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();compressor.requireIdle();requireLibraryIdle();if(registeringLibrary||transferring||extracting)throw std::runtime_error("Wait for the active extraction to finish");
      auto job=findJob(id);
      for(const auto& other:jobs)if(other.at("id")!=id&&other.value("destination","")==job.value("destination","")&&other.value("status","")=="moved")
        throw std::runtime_error("Another job uses this library destination; manual review required");
      if(compressor.protects(id)){reply(res,compressor.requestGameDeletion(id),202);return;}
      const auto selected=storage.get(job.value("storage","internal"));
      // Validate the recorded identity before dispatch; the worker rechecks source selection.
      const auto title=job.at("content").value("titleId","");
      const auto kind=job.at("content").value("kind",std::string{});
      const auto name=safeRelative(job.at("content").value("destination",std::string{}));
      if(job.value("status","")!="moved"||(kind!="folder"&&kind!="image"&&kind!="exfat")||!std::regex_match(title,std::regex("PPSA[0-9]{5}"))||title=="PPSA99071"||name.has_parent_path()||(kind=="folder"&&name!=title+"-app")||job.value("destination","")!=(selected.library/name).string())throw std::runtime_error("Invalid tracked Library game");
      operations.start(id,job.value("name","Game"));transferState={{"status","running"},{"kind","deletion"},{"id",id},{"phase","Preparing deletion"}};writeJson(paths.root/"transfer.json",transferState);transferring=true;
      try {std::thread([job,id,title,selected]{
        bool dispatched=false;
        try {
          BackgroundStorage background(shadowPort);
          if(background.supported()){dispatched=true;background.run("delete",title,job.at("destination").template get<std::string>(),{},[](const json& task){const auto progress=backgroundProgress(task);operations.progress(progress);std::lock_guard<std::mutex> g(lock);transferState.update(progress);writeJson(paths.root/"transfer.json",transferState);});}
          else {if(job.at("content").value("kind","")!="folder")throw std::runtime_error("Image deletion requires ShadowMount background storage support; no files removed");dispatched=true;deleteLibraryGame(selected,job,[](uint64_t done,uint64_t total,const std::string& file){operations.update(done,total,file);});}
          std::lock_guard<std::mutex> g(lock);
          fs::remove(paths.jobs/(id+".json"));for(auto it=jobs.begin();it!=jobs.end();++it)if(it->at("id")==id){jobs.erase(it);break;}
          transferState["status"]="complete";transferState["phase"]="Deletion completed";writeJson(paths.root/"transfer.json",transferState);operations.finish(true);transferring=false;try{notifySystem("Botty+: Game deletion completed. Saves and archives kept.");}catch(...){}
        }catch(const std::exception& e){operations.finish(false,e.what());std::lock_guard<std::mutex> g(lock);const bool uncertain=dispatched&&dynamic_cast<const BackgroundRejected*>(&e)==nullptr;transferring=uncertain;transferState={{"status",uncertain?"uncertain":"failed"},{"error",e.what()},{"phase","Deletion stopped. Check Processing before retrying."}};try{writeJson(paths.root/"transfer.json",transferState);}catch(...){}/* A remote worker may still be active. Keep file operations blocked. */}
      }).detach();}catch(...){transferring=false;operations.finish(false);throw;}
      reply(res,{{"accepted",true},{"id",id},{"archivesKept",true}},202);
    });
    server.Post("/api/delete-extraction",[](const auto& req,auto& res){
      const auto id=json::parse(req.body).at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);nativeUpdater->requireAdmission();compressor.requireIdle();requireLibraryIdle();if(registeringLibrary||transferring||extracting)throw std::runtime_error("Wait for the active extraction to finish");
      auto job=findJob(id);if(job.value("status","")!="moved"&&job.value("adoptedImport",false))throw std::runtime_error("Publish the imported game to Library before deleting it");
      if(job.value("status","")=="moved" || job.value("status","")=="moving" || job.value("status","")=="move-error")throw std::runtime_error("Moved or uncertain library files require manual review");
      if(job.value("platform",std::string{})=="switch")switchPreparation.removePartial(job);
      DeletionScope operation(operations,id,job.value("name","Extraction"));
      for(const auto& suffix:{"", ".working"}) {const auto selected=storage.get(job.value("storage","internal"));const auto path=selected.extracted/(id+suffix);if(fs::exists(fs::symlink_status(path)))deleteGameDirectory(selected.extracted,path.filename(),operation.reporter());}
      operation.complete();fs::remove(paths.jobs/(id+".json"));for(auto it=jobs.begin();it!=jobs.end();++it)if(it->at("id")==id){jobs.erase(it);break;}
      reply(res,{{"ok",true}});
    });
    for(const auto& asset:std::map<std::string,std::string>{{"/","index.html"},{"/index.html","index.html"},{"/app.js","app.js"},{"/style.css","style.css"}}) {
      server.Get(asset.first,[asset](const auto&,auto& res){const std::string type=asset.second=="app.js"?"text/javascript":asset.second=="style.css"?"text/css":"text/html";res.set_content(readText(fs::path(uiDir)/asset.second,2*1024*1024),type);});
    }
    server.set_exception_handler([](const auto&,auto& res,std::exception_ptr error){try{std::rethrow_exception(error);}catch(const std::exception& failure){reply(res,{{"error",failure.what()}},400);}catch(...){reply(res,{{"error","Operation failed"}},500);}});
    stage="binding HTTP port";
    if(!server.bind_to_port("0.0.0.0",port))throw std::runtime_error("Botty port is already in use");
    serverThreads.watchReady();
    restMode.start();
    std::thread automaticThread(automaticDownloads);
    const auto registerPrepared=[] {
      static size_t cursor=0;json job;
      {
        std::lock_guard<std::mutex> guard(lock);
        if(retiring||extracting||transferring||registeringLibrary||nativeUpdater->requested()||compressor.busy())return;
        json eligible=json::array();
        for(const auto& candidate:jobs){
          if((candidate.value("status","")=="moved"&&candidate.value("libraryStatus","")=="pending")||
             (candidate.value("status","")=="ready"&&candidate.value("automatic",false)&&candidate.value("error",std::string{}).empty()))eligible.push_back(candidate);
        }
        if(eligible.empty())return;job=eligible[cursor++%eligible.size()];
        registeringLibrary=true;
      }
      struct Lease {~Lease(){registeringLibrary=false;}} lease;
      // Keep admission reserved for file mutations, but never hold the catalog
      // mutex across filesystem work or a ShadowMount network timeout.
      try{
        const auto disk=storage.get(job.value("storage","internal"),false);
        if(job.value("status","")=="ready")try{resumeAdoptedOutput(disk,job);job=movePrepared(disk,job);}catch(...){
          // Preserve the publication journal when a rename/copy had an uncertain outcome.
          job=json::parse(readText(paths.jobs/(job.at("id").get<std::string>()+".json")));throw;
        }
        bool stopped=false;try{confirmNativeProcessesStopped();stopped=true;}catch(...){}
        if(stopped){
          const auto target=containedExisting(disk.library,fs::path(job.at("destination").get<std::string>()));
          CompressionLibrary library(disk,shadowPort);
          if(!job.value("librarySubmitted",false)){library.api("manual/add",{{"path",target.string()}});job["librarySubmitted"]=true;}
          confirmNativeProcessesStopped();
          const auto catalog=library.api("games",{{"include_size",false}});bool found=false;
          for(const auto& game:catalog.at("games"))if(game.value("path","")==target.string()){
            const auto title=game.value("title_id",std::string{});
            if(std::regex_match(title,std::regex("PPSA[0-9]{5}"))){job["content"]["titleId"]=title;found=true;break;}
          }
          job["libraryStatus"]=found?"ready":"pending";job["error"]="";
          job["phase"]=found?"Ready in Library":"Waiting for ShadowMount; stay on the home screen";
        }
      }catch(const std::exception& e){job["error"]=e.what();job["phase"]=job.value("status","")=="moved"?"Close Botty+ and games, then return to the home screen to finish":"Publication needs attention; files kept";}
      try{
        std::lock_guard<std::mutex> guard(lock);
        for(auto& current:jobs)if(current.at("id")==job.at("id")){
          job["dismissed"]=current.value("dismissed",false);
          if(current!=job){writeJson(paths.jobs/(job.at("id").get<std::string>()+".json"),job);current=job;}break;
        }
      }catch(const std::exception& e){std::cerr<<"Library registration checkpoint failed: "<<e.what()<<'\n';}
    };
    std::thread libraryThread([&]{while(!retiring){
      std::this_thread::sleep_for(std::chrono::seconds(2));if(retiring)break;compressor.poll();registerPrepared();
      std::lock_guard<std::mutex> guard(lock);
      for(auto it=jobs.begin();it!=jobs.end();){
        const auto id=it->at("id").get<std::string>();
        if(compressor.game(id).value("status","")!="deleted"){++it;continue;}
        try{if(!std::regex_match(id,std::regex("[a-f0-9]{32}")))throw std::runtime_error("Invalid deleted job ID");downloadedFiles(paths.jobs,{id+".json"},true);it=jobs.erase(it);}catch(...){++it;}
      }
    }});
    std::cerr<<"Botty startup complete\n";
    std::cout<<"Botty listening on "<<origin<<'\n';
    const bool served=serveWithRestRecovery(server,"0.0.0.0",port,[]{return retiring.load();},[&server](int seconds){
      for(int n=0;n<seconds*10&&!retiring&&!server.listen_stop_requested();++n)std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return !retiring&&!server.listen_stop_requested();
    },[](int error){httpReady=false;std::fprintf(stderr,"Botty HTTP listener interrupted (%d); rebinding while preserving background jobs\n",error);});
    retiring=true;serverThreads.join();automaticThread.join();libraryThread.join();remote.reset();nativeUpdater.reset();return served?0:1;
  }catch(const std::exception& error){retiring=true;std::cerr<<"Botty failed while "<<stage<<": "<<error.what()<<'\n';return 1;}
}
