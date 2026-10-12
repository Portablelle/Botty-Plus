#pragma once
#include "core.hpp"
#include "progress.hpp"
#include "storage.hpp"
#include "remote-buffer.hpp"
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
      {"remote",true},{"remoteLoose",ready&&loose&&!archive},{"remoteCandidate",phase=="available"},{"remotePhase",phase},{"phase",phase=="available"?"Available on dedicated server":phase=="transfer"?"Importing from dedicated server":phase=="ready"?"Imported to PS5":"Torrent downloading on dedicated server"},{"percentDone",total?double(done)/total:0.0},{"leftUntilDone",left},{"totalSize",total},{"sizeWhenDone",total},
      {"rateDownload",phase=="transfer"?r.value("rate",uint64_t(0)):ready?uint64_t(0):snapshot.value("rate",uint64_t(0))},{"rateUpload",phase=="remote"?snapshot.value("upload",uint64_t(0)):uint64_t(0)},{"eta",r.value("eta",-1.0)},
      {"downloadDir",r.at("directory")},{"storage",r.at("storage")},{"files",files},{"error",error.empty()?0:1},{"errorString",error},{"peersConnected",snapshot.value("peers",0)},{"peersSendingToUs",0},{"peersGettingFromUs",0}};
  }
  void update(const std::string& hash,const json& changes,bool durable=false) {
    std::lock_guard<std::mutex> g(mutex_);if(!records_.contains(hash))return;
    auto values=changes;
    if(values.contains("snapshot")&&records_[hash].contains("inventory"))values["snapshot"].erase("files");
    records_[hash].update(values);
    if(values.contains("inventory")&&records_[hash].contains("snapshot"))records_[hash]["snapshot"].erase("files");
    if(durable||values.contains("snapshot")||values.contains("inventory"))save();
  }
  struct Sink {
    RemoteDownloads* owner;std::string hash,storage;fs::path directory;int fd;
    uint64_t bytes,limit,base,total,offset;RemoteWriteBuffer output;CURL* curl;long expectedStatus;ExtractionEstimate estimate;
    std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now(),last=start;
    std::string error;
  };
  static int cancelled(void* opaque,curl_off_t,curl_off_t,curl_off_t,curl_off_t){auto& s=*static_cast<Sink*>(opaque);return s.owner->stop_||s.owner->cancel_;}
  static size_t writeFile(char* p,size_t size,size_t count,void* opaque) {
    auto& s=*static_cast<Sink*>(opaque);const size_t bytes=size*count;
    try{
      if(s.owner->stop_||s.owner->cancel_||bytes>s.limit-s.bytes)return 0;
      long status=0;curl_easy_getinfo(s.curl,CURLINFO_RESPONSE_CODE,&status);if(status!=s.expectedStatus)throw std::runtime_error("Unexpected HTTPS response; partial bytes kept unchanged");
      const auto now=std::chrono::steady_clock::now();
      if(now-s.last>=std::chrono::milliseconds(250)){
        s.owner->checkStorage_(s.storage,s.directory);
        const auto done=s.base+s.offset+s.output.written();s.estimate.update(now,done,s.total);
        const auto seconds=std::chrono::duration<double>(now-s.start).count();
        s.owner->update(s.hash,{{"bytes",done},{"rate",seconds>0?uint64_t(s.output.written()/seconds):uint64_t(0)},{"eta",s.estimate.eta}});s.last=now;
      }
      s.output.append(p,bytes);
      s.bytes+=bytes;return bytes;
    }catch(const std::exception& e){s.error=e.what();return 0;}
  }
  static int leaf(const fs::path& root,const fs::path& relative) {
    int fd=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)throw std::runtime_error("Download storage unavailable");
    try{
      for(const auto& part:relative.parent_path()){
        if(makeDirectoryRelative(fd,part.c_str(),0700)&&errno!=EEXIST)throw std::runtime_error("Cannot create download folder");
        int next=openFileRelative(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(next<0)throw std::runtime_error("Unsafe download folder");::close(fd);fd=next;
      }
      int result=openFileRelative(fd,relative.filename().c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK,0600);::close(fd);fd=-1;
      if(result<0)throw std::runtime_error("Cannot open partial download");
      struct stat st{};if(fstat(result,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1){::close(result);throw std::runtime_error("Unsafe partial download");}
      return result;
    }catch(...){if(fd>=0)::close(fd);throw;}
  }
  static fs::path stagingPath(const fs::path& directory,const std::string& hash){return directory/(".remote-"+hash+".partial");}
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
  static json markDirectory(const fs::path& path){
    const auto token=randomId();int fd=::open((path/remoteOwnerFile).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0)throw std::runtime_error("Cannot create import ownership marker");struct Close{int fd;~Close(){::close(fd);}} close{fd};
    if(::write(fd,token.data(),token.size())!=static_cast<ssize_t>(token.size())||fsync(fd))throw std::runtime_error("Cannot flush import ownership marker");
    return directoryIdentity(path);
  }
  enum class PublicationState { none, reservation, renamed };
  static PublicationState publicationState(const std::string& hash,const json& r) {
    if(!r.value("publishing",false)||!r.contains("stageIdentity")||!r.contains("inventory"))return PublicationState::none;
    const fs::path directory=r.at("directory").get<std::string>();
    const auto stage=stagingPath(directory,hash),target=directory/hash;
    if(!fs::exists(fs::symlink_status(target)))return PublicationState::none;
    containedExisting(directory,target);
    if(fs::exists(fs::symlink_status(stage)))return directoryIdentity(stage)==r.at("stageIdentity")&&r.contains("reservationIdentity")&&directoryIdentity(target)==r.at("reservationIdentity")?PublicationState::reservation:PublicationState::none;
    return directoryIdentity(target)==r.at("stageIdentity")?PublicationState::renamed:PublicationState::none;
  }
  bool recoverPublished(const std::string& hash,const json& r) {
    const fs::path directory=r.at("directory").get<std::string>();checkStorage_(r.at("storage"),directory);
    if(publicationState(hash,r)!=PublicationState::renamed)return false;
    checkMembers_(stagingPath(directory,hash),directory/hash,r.at("inventory"));validateTree(directory/hash,r.at("inventory"));uint64_t total=0;
    for(const auto& f:r.at("inventory")){const uint64_t size=f.at("size");if(size>UINT64_MAX-total)throw std::runtime_error("Remote size overflow");total+=size;}
    update(hash,{{"phase","ready"},{"publishedIdentity",r.at("stageIdentity")},{"bytes",total},{"error",""}},true);return true;
  }
  void transfer(const std::string& hash,json r,const json& snapshot) {
    const fs::path directory=r.at("directory").get<std::string>();const std::string storage=r.at("storage");
    checkStorage_(storage,directory);const auto stage=stagingPath(directory,hash),target=directory/hash;
    json inventory=json::array();std::set<fs::path> seen;uint64_t total=0;
    for(const auto& f:snapshot.at("files")){
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
    if(fs::exists(fs::symlink_status(stage))&&!r.contains("stageIdentity")){
      throw std::runtime_error("Unowned partial directory exists; retained for inspection");
    }
    if(!fs::exists(fs::symlink_status(stage))){
      if(!fs::create_directory(stage))throw std::runtime_error("Partial import directory appeared; files kept");
      r["stageIdentity"]=markDirectory(stage);update(hash,{{"stageIdentity",r.at("stageIdentity")}},true);
    }else if(!r.contains("stageIdentity")||directoryIdentity(stage)!=r.at("stageIdentity"))throw std::runtime_error("Partial directory is not owned by this import");
    containedExisting(directory,stage);
    checkMembers_(stage,target,inventory);uint64_t done=0;Handle h;
    for(const auto& file:inventory){
      checkStorage_(storage,directory);if(stop_||cancel_)throw std::runtime_error("Transfer paused");
      const auto name=safeRelative(file.at("name").get<std::string>());const uint64_t size=file.at("size");
      int fd=leaf(stage,name);struct Close{int fd;~Close(){::close(fd);}} close{fd};struct stat st{};
      if(fstat(fd,&st)||st.st_size<0||uint64_t(st.st_size)>size)throw std::runtime_error("Partial file size is invalid");
      const auto offset=uint64_t(st.st_size);
      const uint64_t available=freeBytes(directory),reserve=512ULL*1024*1024;
      if(available<=reserve||size-offset>available-reserve)throw std::runtime_error("Not enough space to import remote torrent");
      if(offset<size){
        if(lseek(fd,st.st_size,SEEK_SET)<0)throw std::runtime_error("Cannot resume local file");
        Sink sink{this,hash,storage,directory,fd,offset,size,done,total,offset,RemoteWriteBuffer(fd),h.curl,offset?206L:200L,ExtractionEstimate{},std::chrono::steady_clock::now(),std::chrono::steady_clock::now(),""};sink.start=sink.last=std::chrono::steady_clock::now();
        setup(h,"/file/"+hash+"/"+std::to_string(file.at("index").get<unsigned>()));
        auto* escaped=curl_easy_escape(h.curl,name.string().c_str(),static_cast<int>(name.string().size()));if(!escaped)throw std::runtime_error("Cannot encode pinned remote member");
        const std::string nameHeader=std::string("X-Botty-File-Name: ")+escaped;curl_free(escaped);
        h.headers=curl_slist_append(h.headers,nameHeader.c_str());h.headers=curl_slist_append(h.headers,("X-Botty-File-Size: "+std::to_string(size)).c_str());curl_easy_setopt(h.curl,CURLOPT_HTTPHEADER,h.headers);
        curl_easy_setopt(h.curl,CURLOPT_SOCKOPTFUNCTION,receiveSocket);
        curl_easy_setopt(h.curl,CURLOPT_BUFFERSIZE,256L*1024);
        curl_easy_setopt(h.curl,CURLOPT_WRITEFUNCTION,writeFile);curl_easy_setopt(h.curl,CURLOPT_WRITEDATA,&sink);
        curl_easy_setopt(h.curl,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(h.curl,CURLOPT_LOW_SPEED_LIMIT,1L);curl_easy_setopt(h.curl,CURLOPT_LOW_SPEED_TIME,30L);
        curl_easy_setopt(h.curl,CURLOPT_XFERINFOFUNCTION,cancelled);curl_easy_setopt(h.curl,CURLOPT_XFERINFODATA,&sink);curl_easy_setopt(h.curl,CURLOPT_NOPROGRESS,0L);
        if(offset)curl_easy_setopt(h.curl,CURLOPT_RESUME_FROM_LARGE,static_cast<curl_off_t>(offset));
        const auto code=curl_easy_perform(h.curl);long status=0;curl_easy_getinfo(h.curl,CURLINFO_RESPONSE_CODE,&status);
        // Keep a valid received prefix on pause or a truncated response. A crash may
        // lose only the bounded in-memory tail; the next Range starts at file size.
        if(sink.output.pending()){checkStorage_(storage,directory);sink.output.flush();}
        if(code!=CURLE_OK||status!=(offset?206:200)||sink.bytes!=size)throw std::runtime_error(sink.error.empty()?"HTTPS transfer interrupted; resume retains partial files":sink.error);
      }
      if(fsync(fd))throw std::runtime_error("Cannot flush downloaded file");done+=size;update(hash,{{"bytes",done}});
    }
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
        }catch(const std::exception& e){try{update(hash,{{"error",e.what()},{"rate",0},{"eta",-1.0}},true);}catch(...){}}
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
      for(const auto& pair:std::vector<std::pair<fs::path,std::string>>{{root/hash,"publishedIdentity"},{stagingPath(root,hash),"stageIdentity"},{root/hash,"reservationIdentity"}})
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
    std::vector<fs::path> members;for(const auto& f:r.value("inventory",json::array()))members.push_back(safeRelative(f.at("name")));
    for(const auto& folder:{directory/hash,stagingPath(directory,hash)})if(fs::exists(fs::symlink_status(folder))){
      const auto identity=folder==directory/hash?"publishedIdentity":"stageIdentity";
      const auto stage=stagingPath(directory,hash);
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
    auto body=args;body["hash"]=hash;auto response=http(importing?"/import":"/add",&body);
    std::lock_guard<std::mutex> g(mutex_);if(records_.size()>=4096&&!records_.contains(hash))throw std::runtime_error("Remote download registry is full");const int id=records_.contains(hash)?records_[hash].at("id").get<int>():allocateId();
    records_[hash]={{"id",id},{"directory",directory},{"storage",storage},{"paused",false},{"phase","remote"},{"snapshot",importing?response:json::object()},{"name",args.value("name",std::string("Remote torrent"))}};save();
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
