#include "core.hpp"
#include "progress.hpp"
#include "search.hpp"
#define CPPHTTPLIB_THREAD_POOL_COUNT 3
#include "httplib.h"
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
#define BOTTY_UI "/data/botty/manager/1.0.2/ui"
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
std::mutex lock;
json jobs=json::array(); bool extracting=false;
std::atomic<bool> cancelExtraction{false}; std::string activeJob;
Paths paths;
std::string uiDir=BOTTY_UI;
std::string token;
int rpcPort=9091, port=8088;

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
    // Match the existing Transmission LAN allowlist; never show loopback to a phone.
    if(std::string(address).rfind("192.168.",0)==0) {result="http://"+std::string(address)+":9091";break;}
  }
  freeifaddrs(interfaces);
  return result;
}
json rpc(const std::string& method, const json& arguments=json::object()) {
  const auto credentials=json::parse(readText(paths.root/"transmission/state/botty-credentials.json",4096));
  const auto username=credentials.at("username").get<std::string>(), password=credentials.at("password").get<std::string>();
  if(username!="botty" || !std::regex_match(password,std::regex("([A-Za-z0-9]{6}|[a-f0-9]{32})")))throw std::runtime_error("Invalid saved Transmission credentials");
  httplib::Client client("127.0.0.1",rpcPort);
  client.set_connection_timeout(2);client.set_read_timeout(10);client.set_write_timeout(5);
  client.set_basic_auth(username,password);
  httplib::Headers headers;
  const std::string body=json{{"method",method},{"arguments",arguments}}.dump();
  auto response=client.Post("/transmission/rpc",headers,body,"application/json");
  if(response && response->status==409) {
    const auto session=response->get_header_value("X-Transmission-Session-Id");
    if(session.empty() || session.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Invalid Transmission session response");
    headers.emplace("X-Transmission-Session-Id",session);
    response=client.Post("/transmission/rpc",headers,body,"application/json");
  }
  if(!response || response->status!=200)throw std::runtime_error("Transmission is unavailable. Start it from the Botty portal.");
  auto data=json::parse(response->body);
  if(data.value("result","")!="success")throw std::runtime_error("Transmission: "+data.value("result","RPC error"));
  return data.value("arguments",json::object());
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
        job["extractionRate"]=0;job["eta"]=-1;job["status"]="interrupted";job["error"]="Previous session ended during this operation. Extract the same archive again to verify and resume supported partial output.";
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
  if(extracting)throw std::runtime_error("Another extraction is running");
  const auto torrent=getTorrent(id);
  if(torrent.at("leftUntilDone").get<uint64_t>()!=0 || torrent.value("error",0)!=0 || torrent.value("status",0)==1 || torrent.value("status",0)==2)throw std::runtime_error("Wait until the torrent is complete and error-free");
  if(fs::canonical(torrent.at("downloadDir").get<std::string>())!=fs::canonical(paths.complete))throw std::runtime_error("Only Botty completed downloads can be extracted");
  bool found=false;
  for(const auto& file:torrent.at("files")) {
    // Require all files, including unselected volumes, to be downloaded.
    if(file.at("bytesCompleted").get<uint64_t>()!=file.at("length").get<uint64_t>())throw std::runtime_error("Some torrent files are missing or incomplete");
    if(file.at("name")==name)found=true;
  }
  if(!found || fs::path(name).extension()!=".rar")throw std::runtime_error("Select the first .rar volume from this torrent");
  std::smatch part; if(std::regex_search(name,part,std::regex("\\.part([0-9]+)\\.rar$",std::regex::icase)) && std::stoul(part[1])!=1)throw std::runtime_error("Select part1.rar, not a continuation volume");
  const auto archive=containedExisting(paths.complete,paths.complete/safeRelative(name));
  if(!fs::is_regular_file(archive))throw std::runtime_error("Archive is not a regular file");
  // Check duplicate active or completed jobs; archives are never deleted by extraction.
  for(const auto& job:jobs)if(job.value("hash","")==torrent.at("hashString") && job.value("archive","")==name &&
      (job.value("status","")=="ready" || job.value("status","")=="moved"))throw std::runtime_error("This archive was already extracted");
  json job={{"id",randomId()},{"name",torrent.at("name")},{"hash",torrent.at("hashString")},{"archive",name},
    {"automatic",automatic},{"status","extracting"},{"phase","Starting"},{"bytes",0},{"total",0},{"error",""}};
  bool resuming=false;
  for(const auto& old:jobs) {
    const auto status=old.value("status","");
    if(old.value("hash","")!=torrent.at("hashString")||old.value("archive","")!=name||
        (status!="interrupted"&&status!="cancelled"&&status!="failed")||old.value("dismissed",false))continue;
    const auto stage=paths.extracted/(old.at("id").get<std::string>()+".working");
    if(!fs::exists(fs::symlink_status(stage)))continue;
    if(resuming)throw std::runtime_error("Multiple partial extractions exist; choose which partial output to keep before retrying");
    containedExisting(paths.extracted,stage);job=old;resuming=true;
  }
  job["status"]="extracting";job["phase"]=resuming?"Verifying existing files for resume":"Starting";
  job["bytes"]=0;job["total"]=0;job["error"]="";job["extractionRate"]=0;job["eta"]=-1;
  writeJson(paths.jobs/(job.at("id").get<std::string>()+".json"),job);
  if(resuming){for(auto& old:jobs)if(old.at("id")==job.at("id")){old=job;break;}}else jobs.push_back(job);
  cancelExtraction=false;activeJob=job.at("id");extracting=true;
  try {
    std::thread([job,archive,password,resuming]()mutable{
      const auto stage=paths.extracted/(job.at("id").get<std::string>()+".working");
      const auto final=paths.extracted/job.at("id").get<std::string>();
      ProgressSchedule progressSchedule; ExtractionEstimate estimate;
      try {
        extractRar(archive,stage,[&](const Progress& progress){
          const auto now=std::chrono::steady_clock::now();
          const auto decision=progressSchedule.update(now,progress.phase);
          if(!decision.publish)return;
          job["phase"]=progress.phase;job["bytes"]=progress.bytes;job["total"]=progress.total;job["file"]=progress.file;
          if(progress.total>0){estimate.update(now,progress.bytes,progress.total);}
          job["extractionRate"]=estimate.rate;job["eta"]=estimate.eta;
          publishJob(job,decision.checkpoint);
        },password,[]{return cancelExtraction.load();},0,resuming);
        if(cancelExtraction.load())throw std::runtime_error("Extraction cancelled");
        fs::rename(stage,final);job["content"]=classify(final);job["status"]="ready";job["phase"]="Extraction verified";
        if(job.value("automatic",false)){
          publishJob(job);
          try{job=movePrepared(paths,job);job["phase"]="Ready in Library";}
          catch(const std::exception& failure){job=json::parse(readText(paths.jobs/(job.at("id").get<std::string>()+".json")));job["error"]=failure.what();job["phase"]="Needs attention";}
        }
      }catch(const std::exception& error){job["status"]=cancelExtraction.load()?"cancelled":"failed";job["error"]=cancelExtraction.load()?"":error.what();if(cancelExtraction.load())job["phase"]="Cancelled; partial files kept";}
      job["extractionRate"]=0;job["eta"]=job["status"]=="ready"?0:-1;
      try{publishJob(job);}catch(const std::exception& error){std::cerr<<"Job state save failed: "<<error.what()<<'\n';}
      std::lock_guard<std::mutex> guard(lock);extracting=false;activeJob.clear();
    }).detach();
  }catch(...){extracting=false;activeJob.clear();throw;}
  return job;
}
// Only explicitly requested Prowlarr downloads enter this durable queue.
json queueDownload(const std::string& method,const json& arguments){
  auto result=rpc(method,arguments);
  auto torrent=result.contains("torrent-added")?result.at("torrent-added"):result.at("torrent-duplicate");
  auto hash=torrent.at("hashString").get<std::string>();
  if(!std::regex_match(hash,std::regex("[a-fA-F0-9]{40}")))throw std::runtime_error("Invalid torrent confirmation");
  auto file=paths.root/"automatic"/(hash+".json");
  if(!fs::exists(file))writeJson(file,{{"hash",hash},{"status","waiting"}});
  return result;
}
void automaticDownloads(){
  for(;;){
    std::this_thread::sleep_for(std::chrono::seconds(5));
    try{
      {std::lock_guard<std::mutex> guard(lock);if(extracting)continue;}
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
          auto job=startExtraction({{"id",torrent.at("id")},{"archive",archives[0]}},true);
          task["job"]=job.at("id");task["status"]="tracked";writeJson(file.path(),task);break;
        }
        {std::lock_guard<std::mutex> guard(lock);if(extracting)break;}
      }
    }catch(...){/* Service/Transmission temporarily unavailable: retry without changing downloads. */}
  }
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
    token=randomId();recoverJobs();
    stage="creating HTTP server";
    httplib::Server server;server.set_payload_max_length(2*1024*1024);
    server.set_read_timeout(5);server.set_write_timeout(10);
    const auto origin="http://127.0.0.1:"+std::to_string(port);
    server.set_pre_routing_handler([&](const httplib::Request& req,httplib::Response& res){
      res.set_header("Cache-Control","no-store");res.set_header("X-Content-Type-Options","nosniff");
      res.set_header("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'");
      const auto host=req.get_header_value("Host"), source=req.get_header_value("Origin");
      if(host!="127.0.0.1:"+std::to_string(port) || (!source.empty() && source!=origin)) {
        reply(res,{{"error","Only the local Botty application can use this service"}},403);return httplib::Server::HandlerResponse::Handled;
      }
      if(req.path.rfind("/api/",0)==0 && req.path!="/api/bootstrap" && req.get_header_value("X-Botty-Token")!=token) {
        reply(res,{{"error","Reload Botty to reconnect"}},403);return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    server.Get("/health",[](const auto&,auto& res){reply(res,{{"app","Botty"},{"version","1.0.2"},{"titleId","BTTY00001"},{"apiVersion",1}});});
    server.Get("/api/bootstrap",[](const auto&,auto& res){reply(res,{{"token",token},{"apiVersion",1}});});
    // Explicit local, token-authenticated disclosure for the console UI only.
    server.Get("/api/connections",[](const auto&,auto& res){
      (void)rpc("session-get"); // Do not display unverified credentials as usable.
      const auto credentials=json::parse(readText(paths.root/"transmission/state/botty-credentials.json",4096));
      reply(res,{{"apiVersion",1},{"url",connectionUrl()},
        {"username",credentials.at("username")},{"password",credentials.at("password")}});
    });
    server.Get("/api/state",[](const auto&,auto& res){
      json result={{"search",search.state()},{"searchSupported",true},{"extractionControls",true},{"libraryDeletionSupported",true},{"freeBytes",freeBytes(paths.root)},{"library",paths.library.string()}};
      try{result["torrents"]=torrents();result["transmissionReady"]=true;}catch(const std::exception& error){result["torrents"]=json::array();result["transmissionReady"]=false;result["error"]=error.what();}
      {std::lock_guard<std::mutex> guard(lock);result["jobs"]=jobs;result["extracting"]=extracting;}
      explore.registerCatalogArtwork(result["torrents"],result["jobs"]);
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
    server.Post("/api/explore/add",[](const auto& req,auto& res){explore.add(paths,json::parse(req.body).at("id").template get<std::string>(),queueDownload);reply(res,{{"ok",true}},202);});
    server.Post("/api/search",[](const auto& req,auto& res){search.start(paths,json::parse(req.body).at("query").template get<std::string>());reply(res,{{"ok",true}},202);});
    server.Post("/api/search/add",[](const auto& req,auto& res){search.add(paths,json::parse(req.body).at("id").template get<std::string>(),queueDownload);reply(res,{{"ok",true}},202);});
    server.Post("/api/torrent",[](const auto& req,auto& res){
      auto body=json::parse(req.body);const auto action=body.at("action").template get<std::string>();
      if(action=="add") {
        const auto magnet=body.at("magnet").template get<std::string>();
        if(magnet.rfind("magnet:?",0)!=0 || magnet.size()>16384)throw std::runtime_error("Enter a valid magnet link");
        reply(res,rpc("torrent-add",{{"filename",magnet},{"download-dir",paths.complete.string()}}));return;
      }
      const int id=body.at("id").template get<int>();
      if(id<0)throw std::runtime_error("Invalid torrent ID");
      if(action=="remove-data") {
        if(!body.value("confirmed",false))throw std::runtime_error("Confirm deletion of the torrent and downloaded files");
        std::lock_guard<std::mutex> guard(lock);
        if(extracting)throw std::runtime_error("Wait for extraction to finish before deleting archives");
        const auto torrent=getTorrent(id);
        const auto downloadRoot=paths.root/"downloads";
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
          std::vector<fs::path> otherLocations{fs::path(other.at("downloadDir").template get<std::string>()).lexically_normal()};
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
        // Transmission's RPC success only acknowledges the removal request.
        // Unlink and verify exact members ourselves before discarding metadata,
        // so a filesystem failure leaves a paused torrent that can be retried.
        for(const auto& location:locations)downloadedFiles(location,members,false);
        for(const auto& location:locations)downloadedFiles(location,members,true);
        reply(res,rpc("torrent-remove",{{"ids",{hash}},{"delete-local-data",false}}));return;
      }
      const std::map<std::string,std::string> methods={{"pause","torrent-stop"},{"resume","torrent-start"},{"verify","torrent-verify"}};
      const auto method=methods.find(action);if(method==methods.end())throw std::runtime_error("Unsupported torrent action");
      reply(res,rpc(method->second,{{"ids",{id}}}));
    });
    server.Post("/api/extract",[](const auto& req,auto& res){reply(res,startExtraction(json::parse(req.body)),202);});
    server.Post("/api/move",[](const auto& req,auto& res){
      const auto id=json::parse(req.body).at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);if(extracting)throw std::runtime_error("Wait for the active extraction to finish");
      auto job=movePrepared(paths,findJob(id));for(auto& old:jobs)if(old.at("id")==id)old=job;reply(res,job);
    });
    server.Post("/api/cancel-extraction",[](const auto& req,auto& res){
      const auto id=json::parse(req.body).at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);const auto job=findJob(id);
      if(!extracting || activeJob!=id || job.value("status","")!="extracting")throw std::runtime_error("This extraction is no longer running");
      cancelExtraction=true;reply(res,{{"ok",true},{"cancelRequested",true}},202);
    });
    server.Post("/api/dismiss-extraction",[](const auto& req,auto& res){
      const auto id=json::parse(req.body).at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);auto job=findJob(id);const auto status=job.value("status","");
      if(status!="ready"&&status!="moved"&&status!="failed"&&status!="cancelled"&&status!="interrupted")throw std::runtime_error("Only finished extractions can be removed from the list");
      if(status=="failed"||status=="cancelled"||status=="interrupted"){
        for(const auto& suffix:{"", ".working"}){const auto path=paths.extracted/(id+suffix);if(fs::exists(fs::symlink_status(path)))fs::remove_all(containedExisting(paths.extracted,path));}
      }
      job["dismissed"]=true;writeJson(paths.jobs/(id+".json"),job);
      for(auto& old:jobs)if(old.at("id")==id){old=job;break;}
      reply(res,{{"ok",true}});
    });
    server.Post("/api/delete-library-game",[](const auto& req,auto& res){
      const auto request=json::parse(req.body);
      if(!request.value("confirmed",false))throw std::runtime_error("Confirm deletion of the installed game; archives are kept");
      const auto id=request.at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);if(extracting)throw std::runtime_error("Wait for the active extraction to finish");
      auto job=findJob(id);
      for(const auto& other:jobs)if(other.at("id")!=id&&other.value("destination","")==job.value("destination","")&&other.value("status","")=="moved")
        throw std::runtime_error("Another job uses this library destination; manual review required");
      deleteLibraryGame(paths,job);
      // Keep the record until deletion completes; failed or interrupted requests can be retried.
      fs::remove(paths.jobs/(id+".json"));
      for(auto it=jobs.begin();it!=jobs.end();++it)if(it->at("id")==id){jobs.erase(it);break;}
      reply(res,{{"ok",true},{"archivesKept",true}});
    });
    server.Post("/api/delete-extraction",[](const auto& req,auto& res){
      const auto id=json::parse(req.body).at("id").template get<std::string>();
      std::lock_guard<std::mutex> guard(lock);if(extracting)throw std::runtime_error("Wait for the active extraction to finish");
      auto job=findJob(id);if(job.value("status","")=="moved" || job.value("status","")=="moving" || job.value("status","")=="move-error")throw std::runtime_error("Moved or uncertain library files require manual review");
      for(const auto& suffix:{"", ".working"}) {const auto path=paths.extracted/(id+suffix);if(fs::exists(fs::symlink_status(path)))fs::remove_all(containedExisting(paths.extracted,path));}
      fs::remove(paths.jobs/(id+".json"));for(auto it=jobs.begin();it!=jobs.end();++it)if(it->at("id")==id){jobs.erase(it);break;}
      reply(res,{{"ok",true}});
    });
    for(const auto& asset:std::map<std::string,std::string>{{"/","index.html"},{"/index.html","index.html"},{"/app.js","app.js"},{"/style.css","style.css"}}) {
      server.Get(asset.first,[asset](const auto&,auto& res){const std::string type=asset.second=="app.js"?"text/javascript":asset.second=="style.css"?"text/css":"text/html";res.set_content(readText(fs::path(uiDir)/asset.second,2*1024*1024),type);});
    }
    server.set_exception_handler([](const auto&,auto& res,std::exception_ptr error){try{std::rethrow_exception(error);}catch(const std::exception& failure){reply(res,{{"error",failure.what()}},400);}catch(...){reply(res,{{"error","Operation failed"}},500);}});
    stage="binding HTTP port";
    if(!server.bind_to_port("127.0.0.1",port))throw std::runtime_error("Botty port is already in use");
    std::thread(automaticDownloads).detach();
    std::cerr<<"Botty startup complete\n";
    std::cout<<"Botty listening on "<<origin<<'\n';
    return server.listen_after_bind()?0:1;
  }catch(const std::exception& error){std::cerr<<"Botty failed while "<<stage<<": "<<error.what()<<'\n';return 1;}
}
