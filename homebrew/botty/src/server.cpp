#include "core.hpp"
#include "storage.hpp"
#include "progress.hpp"
#include "operations.hpp"
#include "search.hpp"
#include "rtorrent.hpp"
#define CPPHTTPLIB_THREAD_POOL_COUNT 3
#include "httplib.h"
#include "compressor.hpp"
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
#define BOTTY_UI "/data/botty/manager/1.3.3/ui"
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
Compressor compressor;
Operations operations;
std::mutex lock;
json jobs=json::array(); bool extracting=false;
std::atomic<bool> retiring{false};
std::atomic<bool> cancelExtraction{false}; std::string activeJob;
Paths paths;
Storage storage;
std::vector<fs::path> testMounts;
std::atomic<bool> transferring{false};
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
json rpc(const std::string& method, const json& arguments=json::object()) {
  static Rtorrent backend(paths,rpcPort);
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
void recoverJobs() {
  for(const auto& entry:fs::directory_iterator(paths.jobs)) {
    if(!entry.is_regular_file() || entry.path().extension()!=".json")continue;
    try {
      auto job=json::parse(readText(entry.path()));
      const std::string id=job.at("id");
      if(!std::regex_match(id,std::regex("[a-f0-9]{32}")) || entry.path().stem()!=id)continue;
      if(job.value("status","")=="extracting" || job.value("status","")=="moving") {
        job["extractionRate"]=0;job["eta"]=-1;job["elapsed"]=0;job["status"]="interrupted";job["error"]="Previous session ended during this operation. Extract the same archive again to verify and resume supported partial output.";
        writeJson(entry.path(),job);
      }
      jobs.push_back(job);
    }catch(...){/* Do not overwrite unknown or damaged state. */}
  }
}
json startExtraction(const json& request, bool automatic=false) {
  const int id=request.at("id").get<int>();
  const std::string name=request.at("archive").get<std::string>();
  const std::string password=request.value("password","");
  if(password.size()>1024)throw std::runtime_error("Password too long");
  std::lock_guard<std::mutex> guard(lock);
  if(retiring)throw std::runtime_error("Botty is shutting down");
  compressor.requireIdle();
  if(transferring||extracting)throw std::runtime_error("Another extraction is running");
  const auto torrent=getTorrent(id);
  if(torrent.at("leftUntilDone").get<uint64_t>()!=0 || torrent.value("error",0)!=0 || torrent.value("status",0)==1 || torrent.value("status",0)==2)throw std::runtime_error("Wait until the torrent is complete and error-free");
  const auto sourceStorage=storage.forDownload(torrent.at("downloadDir").get<std::string>());
  const auto targetStorage=request.value("storage",sourceStorage);
  const auto sourcePaths=storage.get(sourceStorage),selectedPaths=storage.get(targetStorage);
  bool found=false;
  for(const auto& file:torrent.at("files")) {
    // Require all files, including unselected volumes, to be downloaded.
    if(file.at("bytesCompleted").get<uint64_t>()!=file.at("length").get<uint64_t>())throw std::runtime_error("Some torrent files are missing or incomplete");
    if(file.at("name")==name)found=true;
  }
  if(!found || fs::path(name).extension()!=".rar")throw std::runtime_error("Select the first .rar volume from this torrent");
  std::smatch part; if(std::regex_search(name,part,std::regex("\\.part([0-9]+)\\.rar$",std::regex::icase)) && std::stoul(part[1])!=1)throw std::runtime_error("Select part1.rar, not a continuation volume");
  const auto archive=containedExisting(sourcePaths.complete,sourcePaths.complete/safeRelative(name));
  if(!fs::is_regular_file(archive))throw std::runtime_error("Archive is not a regular file");
  // Check duplicate active or completed jobs; archives are never deleted by extraction.
  for(const auto& job:jobs)if(job.value("hash","")==torrent.at("hashString") && job.value("archive","")==name &&
      (job.value("status","")=="ready" || job.value("status","")=="moved"))throw std::runtime_error("This archive was already extracted");
  json job={{"id",randomId()},{"name",torrent.at("name")},{"hash",torrent.at("hashString")},{"archive",name},
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
    containedExisting(oldPaths.extracted,stage);job=old;resuming=true;
  }
  job["status"]="extracting";job["phase"]=resuming?"Verifying existing files for resume":"Starting";
  job["bytes"]=0;job["total"]=0;job["error"]="";job["extractionRate"]=0;job["eta"]=-1;
  writeJson(paths.jobs/(job.at("id").get<std::string>()+".json"),job);
  if(resuming){for(auto& old:jobs)if(old.at("id")==job.at("id")){old=job;break;}}else jobs.push_back(job);
  cancelExtraction=false;activeJob=job.at("id");extracting=true;
  try {
    std::thread([job,archive,password,resuming,sourceStorage,targetStorage,sourcePaths,selectedPaths]()mutable{
      const auto check=[&]{if(storage.get(sourceStorage,false).root!=sourcePaths.root || storage.get(targetStorage,false).root!=selectedPaths.root)throw std::runtime_error("Disk mount changed during extraction");};
      const auto stage=selectedPaths.extracted/(job.at("id").get<std::string>()+".working");
      const auto final=selectedPaths.extracted/job.at("id").get<std::string>();
      ProgressSchedule progressSchedule; ExtractionEstimate estimate;const auto started=std::chrono::steady_clock::now();
      std::string measuredPhase;
      try {
        extractRar(archive,stage,[&](const Progress& progress){
          check();const auto now=std::chrono::steady_clock::now();
          const auto decision=progressSchedule.update(now,progress.phase);
          if(!decision.publish)return;
          job["elapsed"]=std::chrono::duration<double>(now-started).count();
          if(measuredPhase!=progress.phase){estimate=ExtractionEstimate{};measuredPhase=progress.phase;}
          job["phase"]=progress.phase;job["bytes"]=progress.bytes;job["total"]=progress.total;job["file"]=progress.file;
          if(progress.total>0){estimate.update(now,progress.bytes,progress.total);}
          job["extractionRate"]=estimate.rate;job["eta"]=estimate.eta;
          publishJob(job,decision.checkpoint);
        },password,[]{return cancelExtraction.load();},0,resuming);
        if(cancelExtraction.load())throw std::runtime_error("Extraction cancelled");
        check();fs::rename(stage,final);job["content"]=classify(final);job["status"]="ready";job["phase"]="Extraction verified";
        if(job.value("automatic",false)){
          publishJob(job);
          try{job=movePrepared(selectedPaths,job,nullptr,check);job["phase"]="Ready in Library";}
          catch(const std::exception& failure){job=json::parse(readText(paths.jobs/(job.at("id").get<std::string>()+".json")));job["error"]=failure.what();job["phase"]="Needs attention";}
        }
      }catch(const std::exception& error){job["status"]=cancelExtraction.load()?"cancelled":"failed";job["error"]=cancelExtraction.load()?"":error.what();if(cancelExtraction.load())job["phase"]="Cancelled; partial files kept";}
      job["elapsed"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
      job["extractionRate"]=0;job["eta"]=job["status"]=="ready"?0:-1;
      try{publishJob(job);}catch(const std::exception& error){std::cerr<<"Job state save failed: "<<error.what()<<'\n';}
      std::lock_guard<std::mutex> guard(lock);extracting=false;activeJob.clear();
    }).detach();
  }catch(...){extracting=false;activeJob.clear();throw;}
  return job;
}
// Only explicitly requested Prowlarr downloads enter this durable queue.
json queueDownload(const std::string& method,const json& arguments,const std::string& selected="internal",bool automatic=true){
  std::lock_guard<std::mutex> guard(lock);if(transferring||retiring)throw std::runtime_error("Wait for the storage transfer to finish");
  const auto selectedPaths=storage.get(selected);
  auto args=arguments;args["download-dir"]=selectedPaths.complete.string();
  auto result=rpc(method,args);
  auto torrent=result.contains("torrent-added")?result.at("torrent-added"):result.at("torrent-duplicate");
  auto hash=torrent.at("hashString").get<std::string>();
  if(!std::regex_match(hash,std::regex("[a-fA-F0-9]{40}")))throw std::runtime_error("Invalid torrent confirmation");
  auto file=paths.root/"automatic"/(hash+".json");
  if(!result.contains("torrent-duplicate"))writeJson(file,{{"hash",hash},{"status",automatic?"waiting":"download-only"},{"storage",selected}});
  return result;
}
void automaticDownloads(){
  for(;;){
    std::this_thread::sleep_for(std::chrono::seconds(5));
    try{
      {std::lock_guard<std::mutex> guard(lock);if(retiring||transferring||extracting||compressor.busy())continue;}
      auto entries=torrents();
      for(const auto& file:fs::directory_iterator(paths.root/"automatic")){
        if(file.path().extension()!=".json")continue;
        auto task=json::parse(readText(file.path()));if(task.value("status","")!="waiting")continue;
        bool existing=false;
        {std::lock_guard<std::mutex> guard(lock);for(const auto& job:jobs)if(job.value("automatic",false)&&job.at("hash")==task.at("hash"))existing=true;}
        if(existing){task["status"]="tracked";writeJson(file.path(),task);continue;}
        for(const auto& torrent:entries){
          if(torrent.at("hashString")!=task.at("hash")||torrent.value("leftUntilDone",1ULL)!=0||torrent.value("error",0)!=0||torrent.value("status",0)==1||torrent.value("status",0)==2)continue;
          std::vector<std::string> archives;
          for(const auto& item:torrent.at("files")){
            std::string name=item.at("name");if(fs::path(name).extension()!=".rar")continue;
            std::smatch part;if(std::regex_search(name,part,std::regex("\\.part([0-9]+)\\.rar$",std::regex::icase))&&std::stoul(part[1])!=1)continue;
            archives.push_back(name);
          }
          if(archives.size()!=1){
            json job={{"id",randomId()},{"hash",task.at("hash")},{"name",torrent.at("name")},{"automatic",true},{"status","failed"},{"phase","Needs attention"},{"error",archives.empty()?"No supported RAR archive found. Automatic installation supports one RAR set containing an app folder or exFAT image.":"Multiple RAR sets found. Choose the archive manually from Torrents."}};
            publishJob(job);task["status"]="attention";writeJson(file.path(),task);break;
          }
          auto job=startExtraction({{"id",torrent.at("id")},{"archive",archives[0]},{"storage",task.value("storage","internal")}},true);
          task["job"]=job.at("id");task["status"]="tracked";writeJson(file.path(),task);break;
        }
        {std::lock_guard<std::mutex> guard(lock);if(extracting)break;}
      }
    }catch(...){/* Service/rTorrent temporarily unavailable: retry without changing downloads. */}
  }
}
json startTransfer(const json& body) {
  std::lock_guard<std::mutex> guard(lock);
  compressor.requireIdle();if(extracting||transferring)throw std::runtime_error("Wait for the current file operation");
  const auto kind=body.value("kind","job");
  if(kind!="torrent"&&kind!="job"&&kind!="publish")throw std::runtime_error("Invalid transfer kind");
  const auto selected=body.at("storage").get<std::string>();
  const auto destination=storage.get(selected);
  json job,torrent;
  std::string sourceId;
  if(kind=="torrent"){
    torrent=getTorrent(body.at("id").get<int>());sourceId=storage.forDownload(torrent.at("downloadDir").get<std::string>());
    if(torrent.value("status",0)==1||torrent.value("status",0)==2)throw std::runtime_error("Wait for torrent verification");
  }else{
    job=findJob(body.at("id").get<std::string>());sourceId=job.value("storage","internal");
    const auto compressed=compressor.game(job.at("id").get<std::string>());
    if(compressed.value("verified",false))sourceId=compressed.value("storage","internal");
    else if(compressor.protects(job.at("id").get<std::string>()))throw std::runtime_error("Resolve compression before transferring");
    if(job.value("status","")!="ready"&&job.value("status","")!="moved")throw std::runtime_error("Only completed content can be transferred");
    if(kind=="publish"&&job.value("status","")!="ready")throw std::runtime_error("Extraction is not ready");
  }
  const auto source=storage.get(sourceId);
  if(kind!="publish"&&selected==sourceId)throw std::runtime_error("Choose a different disk");
  transferState={{"status","running"},{"kind",kind},{"id",body.at("id")},{"storage",selected},{"sourceStorage",sourceId},{"phase","Copying and verifying; source kept until completion"}};
  writeJson(paths.root/"transfer.json",transferState);transferring=true;
  std::thread([kind,selected,sourceId,source,destination,job,torrent]()mutable{
    bool changed=false;
    try {
      const auto check=[&]{if(storage.get(sourceId,false).root!=source.root||storage.get(selected,false).root!=destination.root)throw std::runtime_error("Disk mount changed during transfer");};
      const auto waitForGame=[&](const std::string& title){
        CompressionLibrary library(source,shadowPort);
        while(!library.idle(title)){
          check();{std::lock_guard<std::mutex> g(lock);transferState["phase"]="Close Botty+ and games to transfer Library content";}
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      };
      if(kind=="torrent"){
        const auto hash=torrent.at("hashString").get<std::string>();
        rpc("torrent-close",{{"ids",{hash}}});
        if(getTorrent(torrent.at("id")).value("status",-1)!=0)throw std::runtime_error("Torrent did not stop; no files moved");
        std::vector<fs::path> members;
        uint64_t total=0;
        for(const auto& f:torrent.at("files")){
          const auto name=safeRelative(f.at("name").get<std::string>());
          if(!fs::exists(source.complete/name))continue;
          containedExisting(source.complete,source.complete/name);
          if(fs::exists(fs::symlink_status(destination.complete/name)))throw std::runtime_error("Torrent destination already exists");
          const auto bytes=fs::file_size(source.complete/name);if(bytes>UINT64_MAX-total)throw std::runtime_error("Torrent is too large");total+=bytes;members.push_back(name);
        }
        if(total>UINT64_MAX-536870912ULL||freeBytes(destination.complete)<total+536870912ULL)throw std::runtime_error("Not enough destination space");
        // Refuse shared members before moving anything.
        for(const auto& other:torrents())if(other.at("hashString")!=hash&&other.at("downloadDir")==torrent.at("downloadDir"))
          for(const auto& f:other.at("files"))for(const auto& name:members)if(f.at("name")==name.string())throw std::runtime_error("Torrent files are shared with another torrent");
        for(const auto& name:members){check();auto parent=destination.complete;for(const auto& part:name.parent_path()){parent/=part;if(fs::is_symlink(fs::symlink_status(parent)))throw std::runtime_error("Unsafe transfer destination");fs::create_directories(parent);}copyVerified(source.complete/name,destination.complete/name,check);changed=true;}
        check();rpc("torrent-set-location",{{"ids",{hash}},{"location",destination.complete.string()}});
        if(getTorrent(torrent.at("id")).at("downloadDir")!=destination.complete.string())throw std::runtime_error("Torrent location was not confirmed; both copies kept");
        const auto queue=paths.root/"automatic"/(hash+".json");
        if(fs::exists(queue)){auto task=json::parse(readText(queue));task["storage"]=selected;writeJson(queue,task);}
        for(const auto& name:members){check();downloadedFiles(source.complete,{name},true);}
        // Remain paused: verification/resume is explicit and never races cleanup.
      }else if(kind=="publish"){
        changed=true;job["storage"]=selected;job=movePrepared(source,job,&destination,check);publishJob(job);
      }else if(compressor.game(job.at("id").get<std::string>()).value("verified",false)){
        waitForGame(job.at("content").value("titleId",""));changed=true;compressor.relocate(job.at("id").get<std::string>(),selected,check);
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
          job["content"]["titleId"]=title;waitForGame(title);
        }
        copyVerified(from,to,check);changed=true;
        auto cleanup=from;
        if(moved){
          prepareLibraryPermissions(to);
          const auto backup=source.root/"transfers"/id;fs::create_directories(backup.parent_path());
          if(fs::exists(fs::symlink_status(backup)))throw std::runtime_error("Transfer backup already exists");
          {std::lock_guard<std::mutex> g(lock);transferState["backup"]=backup.string();transferState["destination"]=to.string();writeJson(paths.root/"transfer.json",transferState);}
          check();fs::rename(from,backup);cleanup=backup;
          CompressionLibrary library(source,shadowPort);library.api("manual/remove",{{"path",from.string()}});library.api("manual/add",{{"path",to.string()}});library.api("scan",{{"reset_attempts",false}});
          bool found=false;for(int n=0;n<60;++n){check();try{if(library.api("games/info",{{"title_id",job.at("content").value("titleId","")}}).value("path","")==to.string()){found=true;break;}}catch(...){}std::this_thread::sleep_for(std::chrono::seconds(1));}
          if(!found)throw std::runtime_error("New Library source not confirmed; verified copy and backup kept");
        }
        check();job["storage"]=selected;if(moved)job["destination"]=to.string();publishJob(job);removeTransferred(cleanup);
      }
      std::lock_guard<std::mutex> g(lock);transferState["status"]="complete";transferState["phase"]="Transfer complete; files verified";writeJson(paths.root/"transfer.json",transferState);transferring=false;
    }catch(const std::exception& e){std::lock_guard<std::mutex> g(lock);transferState["status"]=changed?"uncertain":"failed";if(!changed)transferring=false;transferState["error"]=e.what();transferState["phase"]="Transfer stopped; retained copies need inspection";try{writeJson(paths.root/"transfer.json",transferState);}catch(...){}/* Do not replay an ambiguous move. */}
  }).detach();return transferState;
}

void reply(httplib::Response& response,const json& body,int status=200){response.status=status;response.set_content(body.dump(),"application/json");}
}
int main(int argc,char** argv) {
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
    if(fs::exists(paths.root/"transfer.json")){transferState=json::parse(readText(paths.root/"transfer.json"));if(transferState.value("status","")=="running"||transferState.value("status","")=="uncertain"){transferState["status"]="uncertain";transferState["error"]="Interrupted transfer. Copies and metadata need inspection before further file operations.";transferring=true;}}
    writeJson(paths.root/"manager-process.json",{{"pid",getpid()},{"version","1.3.3"}});
    stage="creating HTTP server";
    httplib::Server server;server.set_payload_max_length(2*1024*1024);
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
      if(req.path.rfind("/api/",0)==0 && req.path!="/api/bootstrap" && req.get_header_value("X-Botty-Token")!=token) {
        reply(res,{{"error","Reload Botty to reconnect"}},403);return httplib::Server::HandlerResponse::Handled;
      }
      if(retiring&&req.method=="POST"){reply(res,{{"error","Botty is shutting down"}},503);return httplib::Server::HandlerResponse::Handled;}
      return httplib::Server::HandlerResponse::Unhandled;
    });
    server.Get("/health",[](const auto&,auto& res){reply(res,{{"app","Botty"},{"version","1.3.3"},{"titleId","BTTY00001"},{"apiVersion",1}});});
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
      auto compression=compressor.state();
      if(compression.contains("jobId")){
        compression["id"]=compression.at("jobId");compression["name"]=compression.value("name",compression.value("titleId","Game"));
        compression["kind"]=(compression.value("deleteRequested",false)||compression.value("deleteGameRequested",false))?"deletion":"compression";
        tasks.push_back(compression);
      }
      reply(res,{{"tasks",tasks}});
    });
    server.Get("/api/state",[](const auto&,auto& res){
      json result={{"storage",storage.list()},{"storageSupported",true},{"search",search.state()},{"searchSupported",true},{"extractionControls",true},{"libraryDeletionSupported",true},{"freeBytes",freeBytes(paths.root)},{"library",paths.library.string()}};
      try{result["torrents"]=torrents();result["transmissionReady"]=true;result["torrentEngine"]="rtorrent";}catch(const std::exception& error){result["torrents"]=json::array();result["transmissionReady"]=false;result["error"]=error.what();}
      {std::lock_guard<std::mutex> guard(lock);result["jobs"]=jobs;result["extracting"]=extracting||transferring;result["transfer"]=transferState;}
      for(auto& torrent:result["torrents"])try{torrent["storage"]=storage.forDownload(torrent.at("downloadDir").get<std::string>());}catch(...){torrent["storage"]="unavailable";}
      explore.registerCatalogArtwork(result["torrents"],result["jobs"]);
      result["compression"]=compressor.state();
      for(auto& job:result["jobs"]){const auto c=compressor.game(job.value("id",""));job["compression"]=c;if(c.value("verified",false)){job["destination"]=c.at("output");job["storage"]=c.value("storage","internal");job["content"]["kind"]="compressed";}}
      result["catalogArtworkSupported"]=true;
      std::set<std::string> owned;
      for(const auto& item:result["torrents"])owned.insert(Search::gameKey(item.value("name","")));
      for(const auto& item:result["jobs"])if(item.value("status","")=="ready"||item.value("status","")=="moved"||item.value("status","")=="extracting")owned.insert(Search::gameKey(item.value("name","")));
      // Read only native metadata: never recursively walk large game content.
      try{unsigned count=0;for(const auto& item:fs::directory_iterator(paths.library)){
        if(++count>2048)break;const auto param=item.path()/"sce_sys/param.json";
        try{auto data=json::parse(readText(containedExisting(paths.library,param),65536));auto labels=data.at("localizedParameters");auto language=labels.value("defaultLanguage","en-US");owned.insert(Search::gameKey(labels.at(language).at("titleName").get<std::string>()));}catch(...){}
      }}catch(...){}
      result["torrentRemovalSupported"]=true;result["exploreSupported"]=true;result["explore"]=explore.state(owned);
      reply(res,result);
    });
    server.Get("/api/explore/artwork",[](const auto& req,auto& res){auto data=explore.artwork(paths,req.get_param_value("id"));if(data=="pending"){res.status=202;res.set_content("Pending","text/plain");}else if(data.empty()){res.status=404;res.set_content("Unavailable","text/plain");}else res.set_content(data,"application/octet-stream");});
    // Resolve only known local entries or search IDs, never client-supplied URLs/titles.
    server.Get("/api/artwork",[](const auto& req,auto& res){
      const auto id=req.get_param_value("id");
      auto data=id.rfind("s:",0)==0?search.artwork(paths,id.substr(2)):explore.artwork(paths,id,true);
      if(data=="pending"){res.status=202;res.set_content("Pending","text/plain");}
      else if(data.empty()){res.status=404;res.set_content("Unavailable","text/plain");}
      else res.set_content(data,"application/octet-stream");
    });
    server.Post("/api/explore",[](const auto& req,auto& res){const auto sort=json::parse(req.body).at("sort").template get<std::string>();if(sort.empty())throw std::runtime_error("Choose an Explore sort");explore.start(paths,"PS5",sort,json::parse(req.body).value("refresh",false));reply(res,{{"ok",true}},202);});
    server.Post("/api/explore/add",[](const auto& req,auto& res){const auto body=json::parse(req.body);const auto selected=body.value("storage","internal");storage.get(selected);const bool automatic=body.value("automatic",true);explore.add(paths,body.at("id").template get<std::string>(),[selected,automatic](const std::string& method,const json& args){return queueDownload(method,args,selected,automatic);});reply(res,{{"ok",true}},202);});
    server.Post("/api/search",[](const auto& req,auto& res){search.start(paths,json::parse(req.body).at("query").template get<std::string>());reply(res,{{"ok",true}},202);});
    server.Post("/api/search/add",[](const auto& req,auto& res){const auto body=json::parse(req.body);const auto selected=body.value("storage","internal");storage.get(selected);const bool automatic=body.value("automatic",true);search.add(paths,body.at("id").template get<std::string>(),[selected,automatic](const std::string& method,const json& args){return queueDownload(method,args,selected,automatic);});reply(res,{{"ok",true}},202);});
    server.Post("/api/torrent",[](const auto& req,auto& res){
      auto body=json::parse(req.body);const auto action=body.at("action").template get<std::string>();
      if(action=="add") {
        const auto magnet=body.at("magnet").template get<std::string>();
        if(magnet.rfind("magnet:?",0)!=0 || magnet.size()>16384)throw std::runtime_error("Enter a valid magnet link");
        reply(res,queueDownload("torrent-add",{{"filename",magnet}},body.value("storage","internal"),body.value("automatic",false)));return;
      }
      if(transferring)throw std::runtime_error("Wait for the storage transfer to finish");
      const int id=body.at("id").template get<int>();
      if(id<0)throw std::runtime_error("Invalid torrent ID");
      if(action=="remove-data") {
        if(!body.value("confirmed",false))throw std::runtime_error("Confirm deletion of the torrent and downloaded files");
        std::lock_guard<std::mutex> guard(lock);
        compressor.requireIdle();
        if(transferring||extracting)throw std::runtime_error("Wait for extraction to finish before deleting archives");
        const auto torrent=getTorrent(id);
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
        for(const auto& other:torrents())if(other.at("hashString")!=hash) {
          std::vector<fs::path> otherLocations{fs::weakly_canonical(fs::path(other.at("downloadDir").template get<std::string>()))};
          if(locations.size()>1)otherLocations.push_back(locations[1]);
          for(const auto& file:other.at("files"))for(const auto& suffix:{"", ".part"}) {
            const auto member=fs::path(safeRelative(file.at("name").template get<std::string>()).string()+suffix);
            for(const auto& otherLocation:otherLocations)if(selectedPaths.count(otherLocation/member))
                throw std::runtime_error("Downloaded files are shared with another torrent; deletion refused");
          }
        }
        const auto queue=paths.root/"automatic"/(hash+".json");
        if(fs::exists(queue))writeJson(queue,{{"hash",hash},{"status","deletion-requested"}});
        rpc("torrent-stop",{{"ids",{hash}}});
        bool stopped=false;
        for(int attempt=0;attempt<20;++attempt) {
          for(const auto& item:torrents())if(item.at("hashString")==hash)stopped=item.value("status",-1)==0;
          if(stopped)break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if(!stopped)throw std::runtime_error("Torrent did not stop; no files were deleted");
        // The torrent RPC success only acknowledges the removal request.
        // Unlink and verify exact members ourselves before discarding metadata,
        // so a filesystem failure leaves a paused torrent that can be retried.
        for(const auto& location:locations)downloadedFiles(location,members,false);
        for(const auto& location:locations)downloadedFiles(location,members,true,operation.reporter());
        const auto removed=rpc("torrent-remove",{{"ids",{hash}},{"delete-local-data",false}});operation.complete();reply(res,removed);return;
      }
      const std::map<std::string,std::string> methods={{"pause","torrent-stop"},{"resume","torrent-start"},{"verify","torrent-verify"}};
      const auto method=methods.find(action);if(method==methods.end())throw std::runtime_error("Unsupported torrent action");
      if(action=="resume"||action=="verify")storage.get(storage.forDownload(getTorrent(id).at("downloadDir").template get<std::string>()));
      reply(res,rpc(method->second,{{"ids",{id}}}));
    });
    server.Post("/api/extract",[](const auto& req,auto& res){reply(res,startExtraction(json::parse(req.body)),202);});
    server.Post("/api/move",[](const auto& req,auto& res){
      auto body=json::parse(req.body);
      if(!body.contains("storage")){std::lock_guard<std::mutex> guard(lock);compressor.requireIdle();if(extracting||transferring)throw std::runtime_error("Wait for the active file operation");auto job=findJob(body.at("id").template get<std::string>());const auto selected=storage.get(job.value("storage","internal"));job=movePrepared(selected,job);for(auto& old:jobs)if(old.at("id")==job.at("id"))old=job;reply(res,job);return;}
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
      std::lock_guard<std::mutex> guard(lock);if(transferring)throw std::runtime_error("Wait for the transfer to finish");auto job=findJob(id);const auto status=job.value("status","");
      if(status!="ready"&&status!="moved"&&status!="failed"&&status!="cancelled"&&status!="interrupted")throw std::runtime_error("Only finished extractions can be removed from the list");
      if(status=="failed"||status=="cancelled"||status=="interrupted"){
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
      if(transferring||extracting)throw std::runtime_error("Wait for extraction to finish");
      compressor.stopWorker();retiring=true;reply(res,{{"ok",true}});
      std::thread([&server]{std::this_thread::sleep_for(std::chrono::milliseconds(250));server.stop();}).detach();
    });
    server.Post("/api/compress-game",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);
      if(!body.value("confirmed",false))throw std::runtime_error("Confirm creating a separate compressed copy");
      std::lock_guard<std::mutex> guard(lock);
      if(transferring||extracting)throw std::runtime_error("Wait for extraction to finish");
      reply(res,compressor.start(findJob(body.at("id").template get<std::string>()),body.value("storage","")),202);
    });
    server.Post("/api/restore-uncompressed",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);if(!body.value("confirmed",false))throw std::runtime_error("Confirm restoring the original game");
      std::lock_guard<std::mutex> guard(lock);if(transferring||extracting)throw std::runtime_error("Wait for extraction to finish");
      const auto id=body.at("id").template get<std::string>();findJob(id);reply(res,compressor.requestRestore(id),202);
    });
    server.Post("/api/delete-uncompressed",[](const auto& req,auto& res){
      const auto body=json::parse(req.body);if(!body.value("confirmed",false))throw std::runtime_error("Confirm that you tested the compressed game before deleting its original");
      std::lock_guard<std::mutex> guard(lock);if(transferring||extracting)throw std::runtime_error("Wait for extraction to finish");
      const auto id=body.at("id").template get<std::string>();findJob(id);reply(res,compressor.requestOriginalDeletion(id),202);
    });
    server.Post("/api/cancel-compression",[](const auto& req,auto& res){reply(res,compressor.cancel(json::parse(req.body).at("id").template get<std::string>()),202);});
    server.Post("/api/delete-library-game",[](const auto& req,auto& res){
      const auto request=json::parse(req.body);
      if(!request.value("confirmed",false))throw std::runtime_error("Confirm deletion of the installed game; archives are kept");
      const auto id=request.at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);compressor.requireIdle();if(transferring||extracting)throw std::runtime_error("Wait for the active extraction to finish");
      auto job=findJob(id);
      for(const auto& other:jobs)if(other.at("id")!=id&&other.value("destination","")==job.value("destination","")&&other.value("status","")=="moved")
        throw std::runtime_error("Another job uses this library destination; manual review required");
      if(compressor.protects(id)){reply(res,compressor.requestGameDeletion(id),202);return;}
      DeletionScope operation(operations,id,job.value("name","Game"));
      deleteLibraryGame(storage.get(job.value("storage","internal")),job,operation.reporter());
      // Keep the record until deletion completes; failed or interrupted requests can be retried.
      downloadedFiles(paths.jobs,{id+".json"},true);
      for(auto it=jobs.begin();it!=jobs.end();++it)if(it->at("id")==id){jobs.erase(it);break;}
      operation.complete();reply(res,{{"ok",true},{"archivesKept",true}});
    });
    server.Post("/api/delete-extraction",[](const auto& req,auto& res){
      const auto id=json::parse(req.body).at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);compressor.requireIdle();if(transferring||extracting)throw std::runtime_error("Wait for the active extraction to finish");
      auto job=findJob(id);if(job.value("status","")=="moved" || job.value("status","")=="moving" || job.value("status","")=="move-error")throw std::runtime_error("Moved or uncertain library files require manual review");
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
    std::thread(automaticDownloads).detach();
    std::thread([]{for(;;){
      std::this_thread::sleep_for(std::chrono::seconds(2));compressor.poll();
      std::lock_guard<std::mutex> guard(lock);
      for(auto it=jobs.begin();it!=jobs.end();){
        const auto id=it->at("id").get<std::string>();
        if(compressor.game(id).value("status","")!="deleted"){++it;continue;}
        try{if(!std::regex_match(id,std::regex("[a-f0-9]{32}")))throw std::runtime_error("Invalid deleted job ID");downloadedFiles(paths.jobs,{id+".json"},true);it=jobs.erase(it);}catch(...){++it;}
      }
    }}).detach();
    std::cerr<<"Botty startup complete\n";
    std::cout<<"Botty listening on "<<origin<<'\n';
    return server.listen_after_bind()?0:1;
  }catch(const std::exception& error){std::cerr<<"Botty failed while "<<stage<<": "<<error.what()<<'\n';return 1;}
}
