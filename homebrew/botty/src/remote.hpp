#pragma once
#include "core.hpp"
#include "progress.hpp"
#include "storage.hpp"
#include "remote-buffer.hpp"
#include "remote-ledger.hpp"
#include "remote-outcome.hpp"
#include <sys/socket.h>
#include <curl/curl.h>
#include <atomic>
#include <arpa/inet.h>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <regex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace botty {
inline constexpr const char* remoteOwnerFile=".botty-remote-owner";
inline void removeEmptyRemoteRoot(const fs::path& directory,const fs::path& folder) {
  if(!fs::exists(fs::symlink_status(folder)))return;
  containedExisting(directory,folder);
  std::vector<fs::path> dirs;
  for(const auto& e:fs::recursive_directory_iterator(folder)){
    if(e.path()==folder/remoteOwnerFile&&e.is_regular_file()&&!e.is_symlink())continue;
    if(e.is_symlink()||!e.is_directory())throw std::runtime_error("Unexpected retained files; empty-folder cleanup stopped");
    dirs.push_back(e.path());
  }
  std::sort(dirs.begin(),dirs.end(),[](const fs::path& a,const fs::path& b){return std::distance(a.begin(),a.end())>std::distance(b.begin(),b.end());});
  for(const auto& d:dirs)if(::rmdir(d.c_str()))throw std::runtime_error("Cannot remove empty imported folder");
  if(fs::exists(fs::symlink_status(folder/remoteOwnerFile))&&!fs::remove(folder/remoteOwnerFile))throw std::runtime_error("Cannot remove import ownership marker");
  if(::rmdir(folder.c_str()))throw std::runtime_error("Cannot remove empty import root");
}
// A single joined worker owns remote HTTP transfers. HTTP handlers read snapshots.
class RemoteDownloads {
  Paths paths_;
  json config_,records_=json::object(),candidates_=json::array();
  std::mutex mutex_,requests_;
  std::condition_variable wake_;
  std::thread worker_,monitor_;
  std::atomic<bool> stop_{false},cancel_{false},blocked_{false};
  std::string active_,discoveryError_;
  int nextId_=1000000000;
  int allocateId(){if(nextId_>=2147483646)throw std::runtime_error("Remote ID space exhausted");const int id=nextId_++;writeJson(paths_.root/"remote-next-id.json",nextId_);return id;}
  std::function<bool()> admitted_;
  std::function<void(const fs::path&,const fs::path&,const json&)> checkMembers_;
  std::function<void(const std::string&,const fs::path&)> checkStorage_;
  struct Buffer {std::string text;};
  static size_t receive(char* p,size_t s,size_t n,void* opaque){auto& b=*static_cast<Buffer*>(opaque);size_t bytes=s*n;if(bytes>16*1024*1024-b.text.size())return 0;b.text.append(p,bytes);return bytes;}
  struct Handle {
    CURL* curl=curl_easy_init();curl_slist* headers=nullptr;curl_slist* resolve=nullptr;
    ~Handle(){curl_slist_free_all(headers);curl_slist_free_all(resolve);if(curl)curl_easy_cleanup(curl);}
  };
  static int receiveSocket(void*,curl_socket_t fd,curlsocktype purpose) {
    if(purpose==CURLSOCKTYPE_IPCXN){
      // Set before connect so TCP window scaling can use the larger receive window.
      // Some PS5 kernels cap socket memory: keep a working smaller buffer on refusal.
      for(const int bytes:{4*1024*1024,1024*1024,256*1024})
        if(::setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&bytes,sizeof(bytes))==0)break;
    }
    return CURL_SOCKOPT_OK;
  }
  void setup(Handle& h,const std::string& route) {
    if(!h.curl)throw std::runtime_error("Cannot initialize remote HTTPS");
    curl_easy_reset(h.curl);curl_slist_free_all(h.headers);h.headers=nullptr;curl_slist_free_all(h.resolve);h.resolve=nullptr;
    const auto url=config_.at("url").get<std::string>()+route;
    h.headers=curl_slist_append(h.headers,("Authorization: Bearer "+config_.at("token").get<std::string>()).c_str());
    h.headers=curl_slist_append(h.headers,"Content-Type: application/json");
    curl_easy_setopt(h.curl,CURLOPT_URL,url.c_str());curl_easy_setopt(h.curl,CURLOPT_HTTPHEADER,h.headers);
    curl_easy_setopt(h.curl,CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(h.curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(h.curl,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(h.curl,CURLOPT_NOSIGNAL,1L);
    const auto address=config_.value("connectAddress",std::string{});
    if(!address.empty()){
      std::smatch match;const auto base=config_.at("url").get<std::string>();
      std::regex_search(base,match,std::regex("^https://([^:/]+)(?::([0-9]+))?/"));
      const auto resolve=match[1].str()+":"+(match[2].matched?match[2].str():"443")+":"+address;
      h.resolve=curl_slist_append(nullptr,resolve.c_str());curl_easy_setopt(h.curl,CURLOPT_RESOLVE,h.resolve);
    }
    const auto ca=config_.value("caFile",std::string{});if(!ca.empty())curl_easy_setopt(h.curl,CURLOPT_CAINFO,ca.c_str());
  }
  json http(const std::string& route,const json* body=nullptr) {
    Handle h;setup(h,route);Buffer buffer;std::string data;
    curl_easy_setopt(h.curl,CURLOPT_TIMEOUT,30L);curl_easy_setopt(h.curl,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(h.curl,CURLOPT_WRITEDATA,&buffer);
    if(body){data=body->dump();curl_easy_setopt(h.curl,CURLOPT_POSTFIELDS,data.c_str());curl_easy_setopt(h.curl,CURLOPT_POSTFIELDSIZE,static_cast<long>(data.size()));}
    const auto result=curl_easy_perform(h.curl);long status=0;curl_easy_getinfo(h.curl,CURLINFO_RESPONSE_CODE,&status);
    if(result!=CURLE_OK||status!=200)throw std::runtime_error("Dedicated server unavailable or request rejected");
    return json::parse(buffer.text);
  }
  void save(){
    if(records_.dump(2).size()+1>16*1024*1024){
      const auto file=paths_.root/"remote-downloads.json";
      records_=fs::exists(file)?json::parse(readText(file,16*1024*1024)):json::object();
      throw std::runtime_error("Remote registry metadata limit reached; remove completed tracking before importing more");
    }
    writeJson(paths_.root/"remote-downloads.json",records_);
    int parent=::open(paths_.root.c_str(),O_RDONLY|O_DIRECTORY);if(parent<0)throw std::runtime_error("Cannot sync remote registry directory");
    const int synced=fsync(parent);::close(parent);if(synced)throw std::runtime_error("Cannot sync remote registry directory");
  }
  json row(const std::string& hash,const json& r) const {
    const auto snapshot=r.value("snapshot",json::object());
    uint64_t total=snapshot.value("total",uint64_t(0));const bool ready=r.value("phase","")=="ready";
    const auto phase=r.value("phase",std::string("remote"));
    uint64_t done=ready?total:(phase=="transfer"?r.value("bytes",uint64_t(0)):snapshot.value("done",uint64_t(0)));
    // Server completion never unlocks extraction before local publication.
    uint64_t left=ready?0:std::max(uint64_t(1),total>done?total-done:uint64_t(0));
    json files=json::array();for(const auto& f:r.value("inventory",snapshot.value("files",json::array())))files.push_back({{"name",hash+"/"+f.at("name").get<std::string>()},{"length",f.at("size")},{"bytesCompleted",ready?f.at("size"):json(0)}});
    const auto error=r.value("error",std::string{});
    const bool paused=r.value("paused",false)||(phase=="remote"&&snapshot.value("paused",false));
    bool archive=false,loose=false;for(const auto& f:r.value("inventory",json::array())){const fs::path name=asciiLower(f.at("name").get<std::string>());archive=archive||name.extension()==".rar";loose=loose||name.extension()==".exfat"||(name.filename()=="param.json"&&name.parent_path().filename()=="sce_sys");}
    return {{"id",r.at("id")},{"hashString",hash},{"name",snapshot.value("name",r.value("name",std::string("Remote torrent")))},{"status",paused?0:ready?6:4},
      {"remote",true},{"parallelConnections",r.value("parallelConnections",2)},{"effectiveConnections",r.value("effectiveConnections",r.value("parallelConnections",2))},{"remoteLoose",ready&&loose&&!archive},{"remoteCandidate",phase=="available"},{"remotePhase",phase},{"phase",phase=="available"?"Available on dedicated server":phase=="transfer"?"Importing from dedicated server":phase=="ready"?"Imported to PS5":"Torrent downloading on dedicated server"},{"percentDone",total?double(done)/total:0.0},{"leftUntilDone",left},{"totalSize",total},{"sizeWhenDone",total},
      {"rateDownload",phase=="transfer"?r.value("rate",uint64_t(0)):ready?uint64_t(0):snapshot.value("rate",uint64_t(0))},{"rateUpload",phase=="remote"?snapshot.value("upload",uint64_t(0)):uint64_t(0)},{"eta",r.value("eta",-1.0)},
      {"downloadDir",r.at("directory")},{"storage",r.at("storage")},{"files",files},{"error",error.empty()?0:1},{"errorString",error},{"peersConnected",snapshot.value("peers",0)},{"peersSendingToUs",0},{"peersGettingFromUs",0}};
  }
  void update(const std::string& hash,const json& changes,bool durable=false) {
    std::lock_guard<std::mutex> g(mutex_);if(!records_.contains(hash))return;
    auto values=changes;
    if(values.contains("snapshot")&&records_[hash].contains("inventory"))values["snapshot"].erase("files");
    const bool persist=durable||values.contains("snapshot")||values.contains("inventory");
    const auto previous=persist?records_[hash]:json();
    records_[hash].update(values);
    if(values.contains("inventory")&&records_[hash].contains("snapshot"))records_[hash]["snapshot"].erase("files");
    if(persist){try{save();}catch(...){records_[hash]=previous;throw;}}
  }
  static constexpr uint64_t segmentSize=8ULL*1024*1024;
  static int connections(const json& args,int fallback=2){
    if(!args.contains("parallelConnections"))return fallback;
    if(!args.at("parallelConnections").is_number_integer())throw std::runtime_error("parallelConnections must be 1, 2 or 4");
    const auto n=args.at("parallelConnections").get<int>();
    if(n!=1&&n!=2&&n!=4)throw std::runtime_error("parallelConnections must be 1, 2 or 4");return n;
  }
  struct Sink {
    RemoteDownloads* owner;std::string storage;fs::path directory;
    uint64_t start,end,total,bytes=0;RemoteWriteBuffer output;CURL* curl;std::atomic<bool>* abort;
    bool rangeValid=false,lengthValid=false;std::string error;
    std::chrono::steady_clock::time_point last=std::chrono::steady_clock::now();
  };
  static int cancelled(void* opaque,curl_off_t,curl_off_t,curl_off_t,curl_off_t){auto& s=*static_cast<Sink*>(opaque);return s.owner->stop_||s.owner->cancel_||s.abort->load();}
  static size_t rangeHeader(char* p,size_t size,size_t count,void* opaque){
    auto& s=*static_cast<Sink*>(opaque);const size_t n=size*count;
    const std::string line(p,n),lower=asciiLower(line);
    if(lower.rfind("http/",0)==0){s.rangeValid=false;s.lengthValid=false;}
    if(lower.rfind("content-range:",0)==0){
      const auto expected="content-range: bytes "+std::to_string(s.start)+"-"+std::to_string(s.end-1)+"/"+std::to_string(s.total)+"\r\n";
      s.rangeValid=lower==expected;
    }
    if(lower.rfind("content-length:",0)==0)s.lengthValid=lower=="content-length: "+std::to_string(s.end-s.start)+"\r\n";
    return n;
  }
  static size_t writeFile(char* p,size_t size,size_t count,void* opaque) {
    auto& s=*static_cast<Sink*>(opaque);const size_t bytes=size*count;
    try{
      if(s.owner->stop_||s.owner->cancel_||s.abort->load()||bytes>s.end-s.start-s.bytes)return 0;
      long status=0;curl_easy_getinfo(s.curl,CURLINFO_RESPONSE_CODE,&status);
      if(status!=206||!s.rangeValid||!s.lengthValid)throw std::runtime_error("Invalid HTTPS range response; segment not committed");
      const auto now=std::chrono::steady_clock::now();
      if(now-s.last>=std::chrono::milliseconds(250)){s.owner->checkStorage_(s.storage,s.directory);s.last=now;}
      s.output.append(p,bytes);s.bytes+=bytes;return bytes;
    }catch(const std::exception& e){s.error=e.what();return 0;}
  }
  void segment(Handle& h,const std::string& hash,const json& file,const std::string& storage,const fs::path& directory,int fd,uint64_t start,uint64_t end,std::atomic<bool>& abort){
    setup(h,"/file/"+hash+"/"+std::to_string(file.at("index").get<unsigned>()));
    uint64_t position=start;
    Sink sink{this,storage,directory,start,end,file.at("size").get<uint64_t>(),0,
      RemoteWriteBuffer([fd,&position](const char* p,size_t n){const auto wrote=::pwrite(fd,p,n,static_cast<off_t>(position));if(wrote>0)position+=static_cast<uint64_t>(wrote);return wrote;}),h.curl,&abort,false,false,"",std::chrono::steady_clock::now()};
    const auto name=file.at("name").get<std::string>();auto* escaped=curl_easy_escape(h.curl,name.c_str(),static_cast<int>(name.size()));if(!escaped)throw std::runtime_error("Cannot encode pinned remote member");
    const std::string nameHeader=std::string("X-Botty-File-Name: ")+escaped;curl_free(escaped);
    h.headers=curl_slist_append(h.headers,nameHeader.c_str());h.headers=curl_slist_append(h.headers,("X-Botty-File-Size: "+std::to_string(sink.total)).c_str());curl_easy_setopt(h.curl,CURLOPT_HTTPHEADER,h.headers);
    const auto range=std::to_string(start)+"-"+std::to_string(end-1);curl_easy_setopt(h.curl,CURLOPT_RANGE,range.c_str());
    curl_easy_setopt(h.curl,CURLOPT_SOCKOPTFUNCTION,receiveSocket);curl_easy_setopt(h.curl,CURLOPT_BUFFERSIZE,256L*1024);
    curl_easy_setopt(h.curl,CURLOPT_HEADERFUNCTION,rangeHeader);curl_easy_setopt(h.curl,CURLOPT_HEADERDATA,&sink);
    curl_easy_setopt(h.curl,CURLOPT_WRITEFUNCTION,writeFile);curl_easy_setopt(h.curl,CURLOPT_WRITEDATA,&sink);curl_easy_setopt(h.curl,CURLOPT_FAILONERROR,1L);
    curl_easy_setopt(h.curl,CURLOPT_LOW_SPEED_LIMIT,1L);curl_easy_setopt(h.curl,CURLOPT_LOW_SPEED_TIME,30L);
    curl_easy_setopt(h.curl,CURLOPT_XFERINFOFUNCTION,cancelled);curl_easy_setopt(h.curl,CURLOPT_XFERINFODATA,&sink);curl_easy_setopt(h.curl,CURLOPT_NOPROGRESS,0L);
    const auto code=curl_easy_perform(h.curl);long status=0;curl_easy_getinfo(h.curl,CURLINFO_RESPONSE_CODE,&status);
    if(status==416)throw RemoteTransferError(416,"Server rejected pinned byte range (416); pause and inspect the remote file");
    if(status==429)throw RemoteTransferError(429,"Server transfer slots busy (429); retrying after bounded delay");
    if(code!=CURLE_OK||status!=206||!sink.rangeValid||!sink.lengthValid||sink.bytes!=end-start)throw std::runtime_error(sink.error.empty()?"HTTPS segment interrupted; uncommitted range will be retried":sink.error);
    checkStorage_(storage,directory);sink.output.flush();
  }
  // File-local coordinator: all threads finish before fsync, and only synced
  // successful ranges enter the ledger. Lifecycle/persistence stays with caller.
  RemoteBatchOutcome copyFileSegments(const std::string& hash,const json& file,const std::string& storage,const fs::path& directory,int fd,int parallel,RemoteRangeLedger& ranges,const std::function<void(uint64_t,bool)>& committed){
    std::vector<std::unique_ptr<Handle>> handles;for(int i=0;i<parallel;++i)handles.push_back(std::make_unique<Handle>());
    uint64_t cursor=0;const uint64_t size=file.at("size");
    while(cursor<size){
      const auto missing=ranges.gaps(cursor,size_t(parallel),segmentSize);if(missing.empty())break;
      if(stop_||cancel_)throw std::runtime_error("Transfer paused");
      const size_t count=missing.size();std::vector<std::thread> workers;std::vector<RemoteBatchOutcome> outcomes(count);RemoteBatchOutcome outcome;std::atomic<bool> abort{false};
      try{for(size_t i=0;i<count;++i)workers.emplace_back([&,i]{try{segment(*handles[i],hash,file,storage,directory,fd,missing[i].start,missing[i].end,abort);}catch(...){outcomes[i]=RemoteBatchOutcome::failure(std::current_exception());if(outcomes[i].cancelSiblings())abort=true;}});}
      catch(...){abort=true;for(auto& worker:workers)worker.join();throw;}
      for(auto& worker:workers)worker.join();
      if(fsync(fd))throw std::runtime_error("Cannot sync downloaded segments");
      uint64_t bytes=0;
      for(size_t i=0;i<count;++i){
        outcome.include(outcomes[i]);
        if(outcomes[i].succeeded()){ranges.commit(missing[i]);bytes+=missing[i].end-missing[i].start;}
      }
      committed(bytes,!outcome.succeeded());
      if(!outcome.succeeded())return outcome;
    }
    return {};
  }
  static int leaf(const fs::path& root,const fs::path& relative,std::set<fs::path>* synced=nullptr) {
    int fd=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)throw std::runtime_error("Download storage unavailable");
    try{
      fs::path parentPath=root;
      for(const auto& part:relative.parent_path()){
        const bool created=makeDirectoryRelative(fd,part.c_str(),0700)==0;
        if(!created&&errno!=EEXIST)throw std::runtime_error("Cannot create download folder");
        int next=openFileRelative(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(next<0)throw std::runtime_error("Unsafe download folder");
        if((created||!synced||!synced->count(parentPath))&&fsync(fd)){::close(next);throw std::runtime_error("Cannot sync download folder entry");}
        if(synced)synced->insert(parentPath);::close(fd);fd=next;parentPath/=part;
      }
      int result=openFileRelative(fd,relative.filename().c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK,0600);
      const bool created=result>=0;
      if(result<0&&errno==EEXIST)result=openFileRelative(fd,relative.filename().c_str(),O_RDWR|O_NOFOLLOW|O_NONBLOCK,0600);
      // Durable ledger ranges must never outlive their file/directory entries.
      if(result>=0&&(created||!synced||!synced->count(parentPath))&&fsync(fd)){::close(result);throw std::runtime_error("Cannot sync download member entry");}
      if(result>=0&&synced)synced->insert(parentPath);::close(fd);fd=-1;
      if(result<0)throw std::runtime_error("Cannot open partial download");
      struct stat st{};if(fstat(result,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1){::close(result);throw std::runtime_error("Unsafe partial download");}
      return result;
    }catch(...){if(fd>=0)::close(fd);throw;}
  }
  static fs::path stagingPath(const fs::path& directory,const std::string& hash,const json& r){
    const auto legacy=directory/(".remote-"+hash+".partial"),segmented=directory/(".remote2-"+hash+".partial");
    if(r.value("stagingFormat",1)!=2)return legacy;
    // Durable migration intent can precede rename. Resolve only the matching
    // owned legacy path so references/removal retain it across that window.
    if(!fs::exists(fs::symlink_status(segmented))&&r.contains("stageIdentity")&&fs::exists(fs::symlink_status(legacy))&&directoryIdentity(legacy)==r.at("stageIdentity"))return legacy;
    return segmented;
  }
  static json directoryIdentity(const fs::path& path) {
    struct stat st{};if(lstat(path.c_str(),&st)||!S_ISDIR(st.st_mode))throw std::runtime_error("Import directory is unavailable or unsafe");
    const auto marker=path/remoteOwnerFile;
    if(fs::exists(fs::symlink_status(marker))){
      int fd=::open(marker.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)throw std::runtime_error("Import ownership marker unavailable");
      struct Close{int fd;~Close(){::close(fd);}} close{fd};struct stat owner{};char token[33]{};
      if(fstat(fd,&owner)||!S_ISREG(owner.st_mode)||owner.st_nlink!=1||owner.st_size!=32||::read(fd,token,32)!=32||!std::regex_match(token,std::regex("[a-f0-9]{32}")))throw std::runtime_error("Invalid import ownership marker");
      return {{"marker",std::string(token)}};
    }
    return {{"device",uint64_t(st.st_dev)},{"inode",uint64_t(st.st_ino)}};
  }
  static json markDirectory(const fs::path& path,const std::string& token=randomId()){
    int fd=::open((path/remoteOwnerFile).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0)throw std::runtime_error("Cannot create import ownership marker");struct Close{int fd;~Close(){::close(fd);}} close{fd};
    if(::write(fd,token.data(),token.size())!=static_cast<ssize_t>(token.size())||fsync(fd))throw std::runtime_error("Cannot flush import ownership marker");
    return directoryIdentity(path);
  }
  static void syncStagingDirectories(const fs::path& stage,const fs::path& directory){
    for(const auto& path:{stage,directory}){
      int parent=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(parent<0)throw std::runtime_error("Cannot sync segmented staging directory");
      const int synced=fsync(parent);::close(parent);if(synced)throw std::runtime_error("Cannot sync segmented staging directory");
    }
  }
  void recoverStagingIdentity(const std::string& hash,json& r){
    if(r.contains("stageIdentity")||!r.contains("stageToken"))return;
    const auto stage=stagingPath(r.at("directory").get<std::string>(),hash,r);
    if(!fs::exists(fs::symlink_status(stage)))return;
    const json intended={{"marker",r.at("stageToken")}};
    const auto token=r.at("stageToken").get<std::string>();
    if(!std::regex_match(token,std::regex("[a-f0-9]{32}")))throw std::runtime_error("Invalid staging ownership intent");
    const auto marker=stage/remoteOwnerFile;const auto status=fs::symlink_status(marker);
    if(!fs::exists(status)||(!fs::is_symlink(status)&&fs::is_regular_file(status)&&fs::file_size(marker)<32)){
      containedExisting(r.at("directory").get<std::string>(),stage);
      for(const auto& entry:fs::directory_iterator(stage))if(entry.path()!=marker)throw std::runtime_error("Incomplete staging initialization contains unexpected files; retained for inspection");
      if(!fs::exists(status))markDirectory(stage,token);
      else {
        int fd=::open(marker.c_str(),O_RDWR|O_NOFOLLOW|O_NONBLOCK);if(fd<0)throw std::runtime_error("Cannot recover partial ownership marker");
        struct Close{int fd;~Close(){::close(fd);}} close{fd};struct stat st{};char prefix[32]{};
        if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<0||st.st_size>=32||::pread(fd,prefix,size_t(st.st_size),0)!=st.st_size||token.compare(0,size_t(st.st_size),prefix,size_t(st.st_size)))throw std::runtime_error("Partial ownership marker does not match creation intent");
        if(ftruncate(fd,0)||lseek(fd,0,SEEK_SET)<0)throw std::runtime_error("Cannot recover partial ownership marker");
        size_t at=0;while(at<token.size()){const auto n=::write(fd,token.data()+at,token.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("Cannot recover partial ownership marker");at+=size_t(n);}
        if(fsync(fd))throw std::runtime_error("Cannot sync recovered ownership marker");
      }
    }
    if(directoryIdentity(stage)!=intended)throw std::runtime_error("Partial directory does not match persisted ownership intent");
    syncStagingDirectories(stage,r.at("directory").get<std::string>());
    update(hash,{{"stageIdentity",intended}},true);r["stageIdentity"]=intended;
  }
  enum class PublicationState { none, reservation, renamed };
  static PublicationState publicationState(const std::string& hash,const json& r) {
    if(!r.value("publishing",false)||!r.contains("stageIdentity")||!r.contains("inventory"))return PublicationState::none;
    const fs::path directory=r.at("directory").get<std::string>();
    const auto stage=stagingPath(directory,hash,r),target=directory/hash;
    if(!fs::exists(fs::symlink_status(target)))return PublicationState::none;
    containedExisting(directory,target);
    if(fs::exists(fs::symlink_status(stage)))return directoryIdentity(stage)==r.at("stageIdentity")&&r.contains("reservationIdentity")&&directoryIdentity(target)==r.at("reservationIdentity")?PublicationState::reservation:PublicationState::none;
    return directoryIdentity(target)==r.at("stageIdentity")?PublicationState::renamed:PublicationState::none;
  }
  bool recoverPublished(const std::string& hash,const json& r) {
    const fs::path directory=r.at("directory").get<std::string>();checkStorage_(r.at("storage"),directory);
    if(publicationState(hash,r)!=PublicationState::renamed)return false;
    checkMembers_(stagingPath(directory,hash,r),directory/hash,r.at("inventory"));validateTree(directory/hash,r.at("inventory"));uint64_t total=0;
    for(const auto& f:r.at("inventory")){const uint64_t size=f.at("size");if(size>UINT64_MAX-total)throw std::runtime_error("Remote size overflow");total+=size;}
    update(hash,{{"phase","ready"},{"publishedIdentity",r.at("stageIdentity")},{"bytes",total},{"error",""}},true);return true;
  }
  void transfer(const std::string& hash,json r,const json& snapshot) {
    const fs::path directory=r.at("directory").get<std::string>();const std::string storage=r.at("storage");
    checkStorage_(storage,directory);
    if(!snapshot.value("capabilities",json::object()).value("boundedRanges",false))
      throw std::runtime_error("Update the dedicated-server bridge before importing with manager 1.7.0 (bounded HTTP ranges required)");
    auto stage=stagingPath(directory,hash,r);const auto target=directory/hash;
    json inventory=json::array();std::set<fs::path> seen;std::set<unsigned> indexes;uint64_t total=0;
    for(const auto& f:snapshot.at("files")){
      if(!indexes.insert(f.at("index").get<unsigned>()).second)throw std::runtime_error("Duplicate remote member index");
      const auto name=safeRelative(f.at("name").get<std::string>());if(!seen.insert(name).second)throw std::runtime_error("Duplicate remote member");
      if(asciiLower(name.begin()->string())==remoteOwnerFile)throw std::runtime_error("Remote member conflicts with import ownership metadata");
      uint64_t size=f.at("size");if(size>INT64_MAX||size>UINT64_MAX-total)throw std::runtime_error("Remote size overflow");total+=size;inventory.push_back(f);
    }
    // Pin the manifest for resumptions. Never reuse partial bytes from a changed inventory.
    if(r.contains("inventory")&&r.at("inventory")!=inventory)throw std::runtime_error("Remote inventory changed; retained partial files need inspection");
    update(hash,{{"inventory",inventory},{"phase","transfer"},{"error",""},{"bytes",0}},true);
    if(recoverPublished(hash,r))return;
    if(fs::exists(fs::symlink_status(target))){
      if(publicationState(hash,r)!=PublicationState::reservation||::rmdir(target.c_str()))throw std::runtime_error("Local destination already exists; files kept");
    }
    // Separate staging names prevent 1.6.x rollback from mistaking sparse file
    // length for a downloaded prefix. Persist migration intent before rename.
    const auto oldStage=directory/(".remote-"+hash+".partial"),newStage=directory/(".remote2-"+hash+".partial");
    if(fs::exists(fs::symlink_status(oldStage))&&!fs::exists(fs::symlink_status(newStage))&&r.contains("stageIdentity")){
      if(directoryIdentity(oldStage)!=r.at("stageIdentity"))throw std::runtime_error("Legacy partial directory is not owned by this import");
      // Check both path spellings before rename; local torrents can still own
      // files beneath the legacy name even though the import marker matches.
      checkMembers_(oldStage,target,inventory);checkMembers_(newStage,target,inventory);
      update(hash,{{"stagingFormat",2}},true);r["stagingFormat"]=2;fs::rename(oldStage,newStage);
      int parent=::open(directory.c_str(),O_RDONLY|O_DIRECTORY);if(parent<0)throw std::runtime_error("Cannot sync staging migration");const int synced=fsync(parent);::close(parent);if(synced)throw std::runtime_error("Cannot sync staging migration");
    }else if(r.value("stagingFormat",1)!=2){update(hash,{{"stagingFormat",2}},true);r["stagingFormat"]=2;}
    stage=stagingPath(directory,hash,r);recoverStagingIdentity(hash,r);
    if(fs::exists(fs::symlink_status(stage))&&!r.contains("stageIdentity")){
      throw std::runtime_error("Unowned partial directory exists; retained for inspection");
    }
    if(!fs::exists(fs::symlink_status(stage))){
      if(!r.contains("stageToken")){const auto token=randomId();update(hash,{{"stageToken",token}},true);r["stageToken"]=token;}
      if(!fs::create_directory(stage))throw std::runtime_error("Partial import directory appeared; files kept");
      r["stageIdentity"]=markDirectory(stage,r.at("stageToken").get<std::string>());syncStagingDirectories(stage,directory);update(hash,{{"stageIdentity",r.at("stageIdentity")}},true);
    }else if(!r.contains("stageIdentity")||directoryIdentity(stage)!=r.at("stageIdentity"))throw std::runtime_error("Partial directory is not owned by this import");
    containedExisting(directory,stage);
    // Repeat the name/marker barrier on every attempt, including recovery after
    // an earlier directory fsync or ownership save failure, before sparse writes.
    syncStagingDirectories(stage,directory);
    checkMembers_(stage,target,inventory);uint64_t done=0;
    // Older managers exclusively appended a sequential prefix. New imports always
    // persist format 2 before pwrite can create holes; file length is never progress.
    json ledger=r.value("segments",json::object());const bool legacy=!r.contains("transferFormat");
    if(!ledger.is_object()||(!legacy&&r.at("transferFormat")!=2))throw std::runtime_error("Unsupported remote transfer ledger");
    if(legacy){
      // Commit every append-only prefix together before format 2 is persisted.
      // A pause/failure during this scan leaves the record legacy and retries the
      // scan; later members never lose their retained progress after a restart.
      ledger=json::object();
      for(const auto& file:inventory){
        if(stop_||cancel_)throw std::runtime_error("Transfer paused");
        const auto name=safeRelative(file.at("name").get<std::string>());const auto key=std::to_string(file.at("index").get<unsigned>());
        RemoteRangeLedger prefixes(file.at("size").get<uint64_t>());ledger[key]=prefixes.toJson();if(!fs::exists(fs::symlink_status(stage/name)))continue;
        const int fd=leaf(stage,name);struct Close{int fd;~Close(){::close(fd);}} close{fd};struct stat st{};
        if(fstat(fd,&st)||st.st_size<0||uint64_t(st.st_size)>file.at("size").get<uint64_t>())throw std::runtime_error("Legacy partial file size is invalid");
        if(fsync(fd))throw std::runtime_error("Cannot sync legacy partial file");
        if(st.st_size)prefixes.commit({0,uint64_t(st.st_size)});ledger[key]=prefixes.toJson();
      }
      update(hash,{{"transferFormat",2},{"segments",ledger}},true);
    }
    // Persist the sparse-write format barrier once per attempt, not per member.
    if(!legacy)update(hash,{{"transferFormat",2},{"segments",ledger}},true);
    bool dirty=false;uint64_t checkpointBytes=0;std::set<fs::path> syncedEntries;
    auto checkpointAt=std::chrono::steady_clock::now();
    auto checkpoint=[&](bool force){
      if(!dirty)return;
      const auto now=std::chrono::steady_clock::now();
      if(!force&&checkpointBytes<segmentSize&&now-checkpointAt<std::chrono::seconds(5))return;
      update(hash,{{"segments",ledger},{"bytes",done}},true);
      dirty=false;checkpointBytes=0;checkpointAt=now;
    };
    const int parallel=std::min(connections(r),connections(json{{"parallelConnections",r.value("effectiveConnections",connections(r))}}));const auto started=std::chrono::steady_clock::now();uint64_t received=0;
    try{for(const auto& file:inventory){
      checkStorage_(storage,directory);if(stop_||cancel_)throw std::runtime_error("Transfer paused");
      const auto name=safeRelative(file.at("name").get<std::string>());const uint64_t size=file.at("size");const auto key=std::to_string(file.at("index").get<unsigned>());
      int fd=leaf(stage,name,&syncedEntries);struct Close{int fd;~Close(){::close(fd);}} close{fd};struct stat st{};
      if(fstat(fd,&st)||st.st_size<0||uint64_t(st.st_size)>size)throw std::runtime_error("Partial file size is invalid");
      auto ranges=RemoteRangeLedger::fromJson(ledger.value(key,json::array()),size,uint64_t(st.st_size));
      const auto completed=ranges.completed();
      const uint64_t available=freeBytes(directory),reserve=512ULL*1024*1024;
      const auto required=remoteAdditionalAllocation(size,completed,st.st_blocks>0?uint64_t(st.st_blocks):0);
      if(required&&(available<=reserve||required>available-reserve))throw std::runtime_error("Not enough space to import remote torrent");
      done+=completed;update(hash,{{"bytes",done}});
      const auto outcome=copyFileSegments(hash,file,storage,directory,fd,parallel,ranges,[&](uint64_t bytes,bool force){
        if(bytes){ledger[key]=ranges.toJson();dirty=true;checkpointBytes+=bytes;done+=bytes;received+=bytes;}
        checkpoint(force);
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        update(hash,{{"bytes",done},{"rate",seconds>0?uint64_t(received/seconds):uint64_t(0)},{"eta",received?double(total-done)*seconds/double(received):-1.0}});
      });
      if(outcome.busy())update(hash,{{"effectiveConnections",outcome.connectionsAfterFailure(parallel)}},true);
      outcome.rethrow();
      if(fsync(fd))throw std::runtime_error("Cannot flush downloaded file");
    }checkpoint(true);
    }catch(...){const auto error=std::current_exception();checkpoint(true);std::rethrow_exception(error);}
    validateTree(stage,inventory);checkStorage_(storage,directory);if(stop_||cancel_)throw std::runtime_error("Transfer paused");
    checkMembers_(stage,target,inventory);if(stop_||cancel_)throw std::runtime_error("Transfer paused");update(hash,{{"publishing",true}},true);
    publishExclusive(stage,target,[&](uint64_t device,uint64_t inode){update(hash,{{"reservationIdentity",{{"device",device},{"inode",inode}}}},true);});
    update(hash,{{"phase","ready"},{"publishedIdentity",directoryIdentity(target)},{"bytes",total},{"rate",0},{"eta",-1.0},{"error",""}},true);
  }
  static void validateTree(const fs::path& root,const json& inventory) {
    std::map<fs::path,uint64_t> expected;for(const auto& f:inventory)expected[safeRelative(f.at("name"))]=f.at("size");
    for(const auto& entry:fs::recursive_directory_iterator(root)){
      if(entry.path()==root/remoteOwnerFile){if(!directoryIdentity(root).contains("marker"))throw std::runtime_error("Missing import ownership marker");continue;}
      if(entry.is_symlink()||(!entry.is_directory()&&!entry.is_regular_file()))throw std::runtime_error("Unsafe imported content");
      if(entry.is_regular_file()){auto key=entry.path().lexically_relative(root);auto it=expected.find(key);if(it==expected.end()||entry.file_size()!=it->second)throw std::runtime_error("Imported inventory mismatch");expected.erase(it);}
    }
    if(!expected.empty())throw std::runtime_error("Missing imported files");
  }
  json snapshotFor(const std::string& hash) {
    auto snapshot=http("/torrent/"+hash);
    if(snapshot.at("hash")!=hash||!snapshot.at("files").is_array()||snapshot.at("files").empty()||snapshot.at("files").size()>200000)throw std::runtime_error("Invalid remote snapshot");
    for(const auto& f:snapshot.at("files")){safeRelative(f.at("name").get<std::string>());f.at("size").get<uint64_t>();f.at("index").get<unsigned>();}
    return snapshot;
  }
  void discover(){
    try{auto values=http("/candidates");std::lock_guard<std::mutex> g(mutex_);candidates_=values;discoveryError_.clear();
        std::set<std::string> live;for(const auto& c:values)live.insert(c.at("hash").get<std::string>());
        for(auto it=records_.begin();it!=records_.end();)if(it.value().value("phase","")=="available"&&!live.count(it.key()))it=records_.erase(it);else ++it;
        for(const auto& c:values){const auto hash=c.at("hash").get<std::string>();if(!std::regex_match(hash,std::regex("[a-f0-9]{40}")))continue;
          if(!records_.contains(hash)&&records_.size()<4096){records_[hash]={{"id",allocateId()},{"directory",paths_.complete.string()},{"storage","internal"},{"paused",true},{"phase","available"}};}
          if(records_.contains(hash)&&records_[hash].value("phase","")=="available"){records_[hash]["snapshot"]={{"name",c.at("name")},{"total",c.at("size")},{"done",0}};records_[hash]["compatibility"]=c.at("compatibility");}
        }save();}catch(const std::exception& e){std::lock_guard<std::mutex> g(mutex_);discoveryError_=e.what();}
  }
  void monitor() {
    unsigned polls=0;
    // A long console copy must not freeze the server progress of other torrents.
    while(!stop_){
      if((polls++%6)==0)discover();
      json pending=json::object();
      {std::lock_guard<std::mutex> g(mutex_);if(!active_.empty()&&records_.contains(active_)&&records_[active_].value("phase","")=="transfer")
        for(auto it=records_.begin();it!=records_.end();++it)if(it.key()!=active_&&it.value().value("phase","")=="remote")pending[it.key()]=it.value().at("id");}
      for(auto it=pending.begin();it!=pending.end()&&!stop_;++it)try{
        auto snapshot=snapshotFor(it.key());std::lock_guard<std::mutex> g(mutex_);
        if(records_.contains(it.key())&&records_[it.key()].at("id")==it.value()&&records_[it.key()].value("phase","")=="remote"){
          if(records_[it.key()].contains("inventory"))snapshot.erase("files");
          records_[it.key()]["snapshot"]=snapshot;save();
        }
      }catch(const std::exception&){}
      std::unique_lock<std::mutex> g(mutex_);wake_.wait_for(g,std::chrono::seconds(5),[this]{return stop_.load();});
    }
  }
  void run() {
    while(!stop_){

      json records;{std::lock_guard<std::mutex> g(mutex_);records=records_;}
      for(auto it=records.begin();it!=records.end()&&!stop_;++it){
        const auto hash=it.key();auto r=it.value();if(r.value("paused",false)||r.value("phase","")=="ready"||r.value("phase","")=="available")continue;
        {std::lock_guard<std::mutex> g(mutex_);if(blocked_||!admitted_()||!records_.contains(hash)||records_[hash].value("paused",false))continue;r=records_[hash];if(r.value("phase","")=="ready"||r.value("phase","")=="available")continue;active_=hash;cancel_=false;}
        try{
          if(recoverPublished(hash,r)){std::lock_guard<std::mutex> g(mutex_);active_.clear();wake_.notify_all();continue;}
          auto snapshot=snapshotFor(hash);
          update(hash,{{"snapshot",snapshot},{"error",""}});
          if(snapshot.at("complete").get<bool>())transfer(hash,r,snapshot);
          else update(hash,{{"phase","remote"},{"rate",0},{"eta",-1.0}});
        }catch(const std::exception& e){try{json failure={{"error",e.what()},{"rate",0},{"eta",-1.0}};if(RemoteBatchOutcome::failure(std::current_exception()).pause())failure["paused"]=true;update(hash,failure,true);}catch(...){}}
        {std::lock_guard<std::mutex> g(mutex_);active_.clear();wake_.notify_all();}
      }
      std::unique_lock<std::mutex> g(mutex_);wake_.wait_for(g,std::chrono::seconds(5),[this]{return stop_.load();});
    }
  }
public:
  std::set<fs::path> references(const json& torrent){
    std::lock_guard<std::mutex> guard(mutex_);
    for(auto it=records_.begin();it!=records_.end();++it)if(it.value().at("id")==torrent.at("id")){
      const auto& r=it.value();const auto hash=it.key();const fs::path root=r.at("directory").get<std::string>();
      std::vector<fs::path> owned;
      for(const auto& pair:std::vector<std::pair<fs::path,std::string>>{{root/hash,"publishedIdentity"},{stagingPath(root,hash,r),"stageIdentity"},{root/hash,"reservationIdentity"}})
        if(r.contains(pair.second)&&fs::exists(fs::symlink_status(pair.first))&&directoryIdentity(pair.first)==r.at(pair.second))owned.push_back(pair.first);
      if(publicationState(hash,r)==PublicationState::renamed)owned.push_back(root/hash);
      std::set<fs::path> result;
      for(const auto& f:r.value("inventory",json::array()))for(const auto& suffix:{"", ".part"})for(const auto& directory:owned)result.insert(directory/fs::path(safeRelative(f.at("name").get<std::string>()).string()+suffix));
      return result;
    }
    return {};
  }
  static void checkPublishedIdentity(const fs::path& path,const json& expected){if(directoryIdentity(path)!=expected)throw std::runtime_error("Import directory changed; files kept");}
  json published(int id){
    std::lock_guard<std::mutex> g(mutex_);
    for(auto it=records_.begin();it!=records_.end();++it)if(it.value().at("id")==id){
      const auto& r=it.value();if(r.value("phase","")!="ready"||!r.contains("publishedIdentity"))throw std::runtime_error("Import is not published");
      const fs::path directory=r.at("directory").get<std::string>();checkStorage_(r.at("storage"),directory);checkPublishedIdentity(directory/it.key(),r.at("publishedIdentity"));
      return {{"storage",r.at("storage")},{"identity",r.at("publishedIdentity")},{"directory",directory.string()},{"hash",it.key()}};
    }
    throw std::runtime_error("Remote torrent no longer exists");
  }
  RemoteDownloads(const Paths& paths,std::function<void(const std::string&,const fs::path&)> check,std::function<bool()> admitted,std::function<void(const fs::path&,const fs::path&,const json&)> members):paths_(paths),admitted_(std::move(admitted)),checkMembers_(std::move(members)),checkStorage_(std::move(check)) {
    config_=json::parse(readText(paths.root/"remote.json",16384));
    const auto url=config_.at("url").get<std::string>(),token=config_.at("token").get<std::string>();
    if(!std::regex_match(url,std::regex("https://[A-Za-z0-9.-]+(?::[0-9]+)?/[A-Za-z0-9/_-]+"))||url.back()=='/'||!std::regex_match(token,std::regex("[a-f0-9]{64}")))throw std::runtime_error("Invalid remote.json configuration");
    const auto address=config_.value("connectAddress",std::string{});in_addr ipv4{};
    if(!address.empty()&&inet_pton(AF_INET,address.c_str(),&ipv4)!=1)throw std::runtime_error("Invalid remote connectAddress");
    if(fs::exists(paths.root/"remote-downloads.json"))records_=json::parse(readText(paths.root/"remote-downloads.json",16*1024*1024));
    if(!records_.is_object())throw std::runtime_error("Invalid remote download records");
    std::set<int> ids;for(auto it=records_.begin();it!=records_.end();++it){if(!std::regex_match(it.key(),std::regex("[a-f0-9]{40}"))||it.value().at("id").get<int>()<1000000000||!ids.insert(it.value().at("id").get<int>()).second)throw std::runtime_error("Invalid remote identity");}
    for(const auto& r:records_)nextId_=std::max(nextId_,r.at("id").get<int>()+1);
    if(fs::exists(paths.root/"remote-next-id.json"))nextId_=std::max(nextId_,json::parse(readText(paths.root/"remote-next-id.json",128)).get<int>());
  }
  void start(){worker_=std::thread([this]{run();});monitor_=std::thread([this]{monitor();});}
  ~RemoteDownloads(){stop_=true;cancel_=true;wake_.notify_all();if(worker_.joinable())worker_.join();if(monitor_.joinable())monitor_.join();}
  void block(bool value){blocked_=value;}
  bool busy(){std::lock_guard<std::mutex> g(mutex_);return !active_.empty();}
  json state(){std::lock_guard<std::mutex> g(mutex_);return {{"enabled",true},{"candidates",candidates_},{"error",discoveryError_}};}
  json list(){std::lock_guard<std::mutex> g(mutex_);json out=json::array();for(auto it=records_.begin();it!=records_.end();++it){
      if(it.value().value("phase","")=="available"){bool found=false;for(const auto& c:candidates_)if(c.at("hash")==it.key())found=true;if(!found)continue;}
      out.push_back(row(it.key(),it.value()));}return out;}
  std::string candidateHash(int id){std::lock_guard<std::mutex> g(mutex_);for(auto it=records_.begin();it!=records_.end();++it)if(it.value().at("id")==id&&it.value().value("phase","")=="available")return it.key();return "";}
  void removeData(int id){
    action("torrent-close",{{"ids",{id}}});
    json r;std::string hash;{std::lock_guard<std::mutex> g(mutex_);for(auto it=records_.begin();it!=records_.end();++it)if(it.value().at("id")==id){hash=it.key();r=it.value();}}
    if(hash.empty()||r.value("phase","")=="available")throw std::runtime_error("Only imported torrents can be removed");
    const fs::path directory=r.at("directory").get<std::string>();checkStorage_(r.at("storage"),directory);
    const bool recovering=!r.contains("stageIdentity");recoverStagingIdentity(hash,r);
    if(recovering&&r.contains("stageIdentity"))checkMembers_(stagingPath(directory,hash,r),directory/hash,r.value("inventory",json::array()));
    std::vector<fs::path> members;for(const auto& f:r.value("inventory",json::array()))members.push_back(safeRelative(f.at("name")));
    for(const auto& folder:{directory/hash,stagingPath(directory,hash,r)})if(fs::exists(fs::symlink_status(folder))){
      const auto identity=folder==directory/hash?"publishedIdentity":"stageIdentity";
      const auto stage=stagingPath(directory,hash,r);
      if(!r.contains(identity)&&folder==directory/hash&&publicationState(hash,r)==PublicationState::renamed)r[identity]=r.at("stageIdentity");
      if(!r.contains(identity)){
        // Only remove an empty publication reservation; unowned files are never touched.
        if(folder==directory/hash&&publicationState(hash,r)==PublicationState::reservation){
          if(::rmdir(folder.c_str())&&errno!=ENOTEMPTY&&errno!=EEXIST)throw std::runtime_error("Cannot remove empty publication reservation");
        }
        continue;
      }
      if(directoryIdentity(folder)!=r.at(identity))throw std::runtime_error("Import directory was replaced; no files removed");
      containedExisting(directory,folder);downloadedFiles(folder,members,false);downloadedFiles(folder,members,true);
      removeEmptyRemoteRoot(directory,folder);
    }
    action("torrent-remove",{{"ids",{id}}});
  }
  bool contains(const json& id){if(!id.is_number_integer())return false;std::lock_guard<std::mutex> g(mutex_);for(const auto& r:records_)if(r.at("id")==id)return true;return false;}
  json add(const std::string& hash,const json& args,bool importing=false) {
    std::lock_guard<std::mutex> serial(requests_);
    {std::lock_guard<std::mutex> g(mutex_);if(records_.contains(hash)&&records_[hash].value("phase","")!="available")return {{"torrent-duplicate",{{"id",records_[hash].at("id")},{"hashString",hash}}}};}
    const auto directory=args.at("download-dir").get<std::string>(),storage=args.value("storage",std::string("internal"));checkStorage_(storage,directory);
    const int parallel=connections(args);auto body=args;body["hash"]=hash;auto response=http(importing?"/import":"/add",&body);
    std::lock_guard<std::mutex> g(mutex_);if(records_.size()>=4096&&!records_.contains(hash))throw std::runtime_error("Remote download registry is full");const int id=records_.contains(hash)?records_[hash].at("id").get<int>():allocateId();
    records_[hash]={{"id",id},{"stagingFormat",2},{"transferFormat",2},{"segments",json::object()},{"parallelConnections",parallel},{"directory",directory},{"storage",storage},{"paused",false},{"phase","remote"},{"snapshot",importing?response:json::object()},{"name",args.value("name",std::string("Remote torrent"))}};save();
    return {{"torrent-added",{{"id",id},{"hashString",hash}}}};
  }
  json action(const std::string& method,const json& args) {
    std::lock_guard<std::mutex> serial(requests_);std::unique_lock<std::mutex> g(mutex_);
    for(const auto& id:args.at("ids")){
      std::string hash;for(auto it=records_.begin();it!=records_.end();++it)if((id.is_number_integer()&&it.value().at("id")==id)||(id.is_string()&&it.key()==id.get<std::string>()))hash=it.key();
      if(hash.empty())throw std::runtime_error("Remote torrent no longer exists");auto& r=records_[hash];if(r.value("phase","")=="available")throw std::runtime_error("Import this server candidate first");
      if(method=="torrent-stop"||method=="torrent-close"){
        r["paused"]=true;save();if(active_==hash){cancel_=true;if(!wake_.wait_for(g,std::chrono::seconds(35),[&]{return active_!=hash;}))throw std::runtime_error("Waiting for remote transfer to pause; no files removed");}
        if(method=="torrent-stop"&&records_[hash].value("phase","")=="remote"){g.unlock();try{json body={{"hash",hash},{"action","pause"}};http("/action",&body);}catch(...){g.lock();throw;}g.lock();}
      }else if(method=="torrent-start"||method=="torrent-start-now"){
        if(args.contains("parallelConnections")){if(active_==hash||!r.value("paused",false))throw std::runtime_error("Pause before changing parallelConnections");r["parallelConnections"]=connections(args);r.erase("effectiveConnections");}
        const bool control=r.value("phase","")=="remote";g.unlock();try{if(control){json body={{"hash",hash},{"action","resume"}};http("/action",&body);}}catch(...){g.lock();throw;}g.lock();records_[hash]["paused"]=false;records_[hash]["error"]="";save();
      }
      else if(method=="torrent-remove"){if(!r.value("paused",false)||active_==hash)throw std::runtime_error("Pause transfer before removing it");records_.erase(hash);save();}
      else if(method=="torrent-set-location"){if(r.value("phase","")!="ready"||active_==hash)throw std::runtime_error("Finish import before moving it");const fs::path moved=args.at("location").get<std::string>();validateTree(moved/hash,r.at("inventory"));r["publishedIdentity"]=markDirectory(moved/hash);r["directory"]=args.at("location");r["storage"]=args.value("storage",std::string("internal"));save();}
      else throw std::runtime_error("Remote transfers support pause/resume; piece verification belongs to qBittorrent on the server");
    }
    return json::object();
  }
};
}
