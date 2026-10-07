#include "native-update.hpp"
#include "native-process.hpp"
#include <cassert>
#include <iostream>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace botty;
template<class F> void rejects(F fn) {bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}assert(rejected);}
template<class F> void until(F fn) {for(int i=0;i<400;++i){if(fn())return;std::this_thread::sleep_for(std::chrono::milliseconds(25));}assert(false);}
struct Fixture {
  fs::path root=fs::temp_directory_path()/("botty-native-updater-"+std::to_string(getpid())+"-"+std::to_string(counter++));
  static int counter;
  NativeTransaction::Config config;
  std::map<std::string,std::string> data;
  json manifest,index;
  std::mutex mutex;
  std::atomic<bool> downloadFails{false};
  std::atomic<int> downloads{0};
  Fixture() {
    fs::create_directories(root);config.nativeRoot=root/"homebrew/PPSA99071";config.stateRoot=root/"native";
    for(size_t i=0;i<3;++i)config.metadataRoots[i]=root/("metadata"+std::to_string(i));
    manifest={{"schema",1},{"titleId","PPSA99071"},{"version","01.004.002"},{"files",json::array()}};
    json param={{"titleId","PPSA99071"},{"contentId","UP9000-PPSA99071_00-BOTTYNATIVE00001"},{"contentVersion","01.004.002"},{"localizedParameters",{{"en-US",{{"titleName","Botty+"}}}}}};
    for(const auto& file:NativeTransaction::files()) {
      data[file]=file=="sce_sys/param.json"?param.dump():"new "+file;
      manifest["files"].push_back({{"path",file},{"size",data[file].size()},{"sha256",nativeHash(data[file])}});
      auto previous=data[file];if(file=="sce_sys/param.json"){param["contentVersion"]="01.004.001";previous=param.dump();param["contentVersion"]="01.004.002";}else previous="old "+file;
      fs::create_directories((config.nativeRoot/file).parent_path());nativeWrite(config.nativeRoot/file,previous);
    }
    index={{"schema",1},{"sha256",json::object()}};
    index["sha256"]["botty-native/manifest.json"]=nativeHash(manifest.dump());
    for(const auto& file:manifest["files"])index["sha256"]["botty-native/"+file["path"].get<std::string>()]=file["sha256"];
  }
  ~Fixture(){fs::remove_all(root);}
  std::string fetch(const std::string& path,size_t limit) {
    std::lock_guard<std::mutex> guard(mutex);std::string bytes;
    if(path=="botty-release.json")bytes=index.dump();else if(path=="botty-native/manifest.json")bytes=manifest.dump();
    else {++downloads;if(downloadFails)throw std::runtime_error("Injected download failure.");bytes=data.at(path.substr(13));}
    assert(bytes.size()<=limit);return bytes;
  }
  NativeUpdater::Fetch transport(){return [this](const std::string& p,size_t n){return fetch(p,n);};}
};
int Fixture::counter=0;
int main() {
  {
    std::vector<unsigned char> bytes(480);int32_t size=480,pid=42;
    std::memcpy(bytes.data(),&size,4);std::memcpy(bytes.data()+72,&pid,4);std::memcpy(bytes.data()+447,"botty-manager",14);
    nativeProcessesStopped(bytes,42);rejects([&]{nativeProcessesStopped(bytes,43);});
    std::memset(bytes.data()+447,0,32);std::memcpy(bytes.data()+447,"eboot.bin",10);rejects([&]{nativeProcessesStopped(bytes,43);});
    std::memset(bytes.data()+447,'x',32);rejects([&]{nativeProcessesStopped(bytes,43);});
    bytes.resize(479);rejects([&]{nativeProcessesStopped(bytes,42);});
    rejects([&]{nativeProcessesStopped({},42);});
  }
  {
    Fixture fixture;auto a=nativeRelease(fixture.transport());
    fixture.index["sha256"]["botty/unrelated"]="changed";auto b=nativeRelease(fixture.transport());assert(a.releaseHash==b.releaseHash);
    fixture.index["sha256"]["botty-native/eboot.bin"]=std::string(64,'a');rejects([&]{nativeRelease(fixture.transport());});
    rejects([&]{nativeDownload("botty-native/../eboot.bin",10,"missing");});
    rejects([&]{nativeDownload("https://evil.test/native",10,"missing");});
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);std::atomic<bool> idle{false},closed{false};std::atomic<int> scans{0},notifications{0};
    NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[&]{if(!idle)throw std::runtime_error("Waiting for extraction and complete activation.");},[&]{if(!closed)throw std::runtime_error("Close native apps.");},[]{},[&]{++scans;},[&](const std::string&){++notifications;});
    updater.start();until([&]{return updater.state()["status"]=="available";});
    const auto response=updater.request();assert(response==json({{"apiVersion",1},{"status","queued"},{"version","01.004.002"}}));assert(updater.request()==response);
    assert(json::parse(nativeRead(fixture.config.stateRoot/"request.json",65536))["status"]=="queued");
    assert(updater.state()["closeRequired"]==true);rejects([&]{updater.requireAdmission();});updater.recheck();
    until([&]{return updater.state()["status"]=="waiting";});assert(fixture.downloads==0);assert(transaction.installedVersion()=="01.004.001");
    idle=true;updater.recheck();until([&]{return updater.state()["message"]=="Close native apps.";});assert(fixture.downloads==0);
    closed=true;updater.recheck();until([&]{return updater.state()["status"]=="complete";});assert(transaction.installedVersion()=="01.004.002");assert(!updater.requested());assert(scans==1);until([&]{return notifications==1;});updater.requireAdmission();
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);fixture.downloadFails=true;
    NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[]{},[]{},[]{},[]{});updater.start();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();
    until([&]{return updater.state()["status"]=="error";});assert(!updater.requested());updater.requireAdmission();assert(transaction.installedVersion()=="01.004.001");assert(json::parse(nativeRead(fixture.config.stateRoot/"request.json",65536))["status"]=="failed");
    fixture.downloadFails=false;updater.recheck();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();until([&]{return updater.state()["status"]=="complete";});
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);std::atomic<bool> recovered{false};
    NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[&]{if(!recovered)throw std::logic_error("Compression needs manual recovery.");},[]{},[]{},[]{});
    updater.start();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();
    until([&]{return updater.state()["status"]=="error";});assert(!updater.requested());updater.requireAdmission();assert(fixture.downloads==0);assert(transaction.installedVersion()=="01.004.001");
    recovered=true;updater.recheck();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();until([&]{return updater.state()["status"]=="complete";});
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);const auto release=nativeRelease(fixture.transport());const auto stage=fixture.config.stateRoot/release.hash/"PPSA99071";
    fs::create_directories(stage);nativeWrite(stage/"unexpected.bin","untrusted stage content");
    NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[]{},[]{},[]{},[]{});updater.start();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();until([&]{return updater.state()["status"]=="error";});assert(!updater.requested());assert(transaction.installedVersion()=="01.004.001");assert(nativeRead(stage/"unexpected.bin",128)=="untrusted stage content");
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);std::atomic<bool> idle{false};
    NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[&]{if(!idle)throw std::runtime_error("Idle unavailable.");},[]{},[]{},[]{});updater.start();until([&]{return updater.state()["status"]=="available";});updater.request();
    {std::lock_guard<std::mutex> guard(fixture.mutex);fixture.manifest["version"]="01.004.003";fixture.index["sha256"]["botty-native/manifest.json"]=nativeHash(fixture.manifest.dump());}
    updater.recheck();until([&]{return updater.state()["status"]=="error";});assert(!updater.requested());assert(fixture.downloads==0);assert(transaction.installedVersion()=="01.004.001");
  }
  {
    Fixture fixture;fixture.config.checkpoint=[](const std::string& phase){if(phase=="backup")throw std::runtime_error("Injected interruption after backup.");};NativeTransaction transaction(fixture.config);
    NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[]{},[]{},[]{},[]{});updater.start();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();
    until([&]{return updater.state()["status"]=="blocked";});assert(updater.requested());assert(transaction.pendingJournal());rejects([&]{updater.requireAdmission();});
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);fixture.downloadFails=true;
    {
      NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[]{},[]{},[]{},[]{});updater.start();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();until([&]{return updater.state()["status"]=="error";});
    }
    fixture.downloadFails=false;fixture.downloads=0;
    NativeUpdater restarted(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[]{},[]{},[]{},[]{});restarted.start();until([&]{return restarted.state()["status"]=="available";});assert(!restarted.requested());assert(fixture.downloads==0);
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);std::atomic<bool> scanFails{true};
    NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[]{},[]{},[]{},[&]{if(scanFails)throw std::runtime_error("Scan unavailable.");});updater.start();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();until([&]{return updater.state()["status"]=="blocked";});
    assert(updater.state()["installedVersion"]=="01.004.002");assert(updater.state()["message"].get<std::string>().find("Native files were updated")!=std::string::npos);assert(updater.requested());const int downloads=fixture.downloads;scanFails=false;updater.recheck();until([&]{return updater.state()["status"]=="complete";});assert(fixture.downloads==downloads);
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);std::atomic<int> calls{0};
    auto slow=[&](const std::string& path,size_t n){if(path=="botty-release.json"){++calls;std::this_thread::sleep_for(std::chrono::milliseconds(200));}return fixture.fetch(path,n);};
    NativeUpdater updater(transaction,fixture.config.stateRoot/"request.json",slow,[]{},[]{},[]{},[]{});updater.start();until([&]{return calls>0;});
    const auto start=std::chrono::steady_clock::now();assert(updater.state()["status"]=="checking");assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100));
  }
  {
    Fixture fixture;NativeTransaction transaction(fixture.config);
    {
      NativeUpdater queued(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[]{throw std::runtime_error("Existing compression running.");},[]{},[]{},[]{});
      queued.start();until([&]{return queued.state()["status"]=="available";});queued.request();queued.recheck();until([&]{return queued.state()["status"]=="waiting";});
    }
    auto param=json::parse(fixture.data.at("sce_sys/param.json"));param["contentVersion"]="01.004.003";fixture.data["sce_sys/param.json"]=param.dump();fixture.manifest["version"]="01.004.003";
    for(auto& file:fixture.manifest["files"])if(file["path"]=="sce_sys/param.json"){
      file["size"]=fixture.data.at("sce_sys/param.json").size();file["sha256"]=nativeHash(fixture.data.at("sce_sys/param.json"));fixture.index["sha256"]["botty-native/sce_sys/param.json"]=file["sha256"];
    }
    const auto hash=nativeHash(fixture.manifest.dump());fixture.index["sha256"]["botty-native/manifest.json"]=hash;
    transaction.stage(fixture.manifest,hash,[&](const std::string& path,size_t){return fixture.data.at(path);});transaction.publish(fixture.manifest,hash,[]{});
    NativeUpdater restarted(transaction,fixture.config.stateRoot/"request.json",fixture.transport(),[]{throw std::logic_error("Must not start another installation.");},[]{},[]{},[]{});
    restarted.start();until([&]{return restarted.state()["status"]=="current";});assert(!restarted.requested());assert(restarted.state()["requested"]==false);restarted.requireAdmission();assert(fixture.downloads==0);
    assert(json::parse(nativeRead(fixture.config.stateRoot/"request.json",65536))["status"]=="complete");
  }
  std::cout<<"Native updater tests passed\n";
}
