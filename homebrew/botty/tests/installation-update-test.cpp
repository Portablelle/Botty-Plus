#include "installation-update.hpp"
#include "service-updater-embedded.hpp"
#include <cassert>
#include <iostream>
#include <map>

using namespace botty;
template<class F> void reject(F fn){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}assert(failed);}
template<class F> void until(F fn){for(int i=0;i<500;++i){if(fn())return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}assert(false);}
struct Fixture {
  fs::path root=fs::temp_directory_path()/("botty-installation-"+nativeTransactionId());
  NativeTransaction::Config nativeConfig;
  std::map<std::string,std::string> bytes;
  json manifests=json::object(),index={{"schema",1},{"sha256",json::object()}},running;
  InstallationRelease release;
  Fixture(bool restartEngine=false) {
    fs::create_directories(root);assert(!::chmod(root.c_str(),0700));
    nativeConfig.nativeRoot=root/"homebrew/PPSA99071";nativeConfig.stateRoot=root/"native";
    for(size_t i=0;i<3;++i)nativeConfig.metadataRoots[i]=root/("metadata"+std::to_string(i));
    manifests["botty-native"]={{"schema",1},{"titleId","PPSA99071"},{"version","01.004.002"},{"requires",{{"manager","1.5.4"},{"worker","1.3.1"},{"rtorrent","0.16.24-botty5"},{"apiVersion",1}}},{"files",json::array()}};
    json param={{"titleId","PPSA99071"},{"contentId","UP9000-PPSA99071_00-BOTTYNATIVE00001"},{"contentVersion","01.004.002"},{"localizedParameters",{{"en-US",{{"titleName","Botty+"}}}}}};
    for(const auto& file:NativeTransaction::files()) {
      const auto data=file=="sce_sys/param.json"?param.dump():"native "+file;
      add("botty-native",file,data);nativeWrite(nativeConfig.nativeRoot/file,data);
    }
    manifests["botty"]={{"schema",1},{"id","1.5.5"},{"workerVersion","1.3.1"},{"workerApi","library-1.3"},{"apiVersion",1},{"updaterVersion","1.0.0"},{"files",json::array()}};
    manifests["rtorrent"]={{"schema",1},{"id",restartEngine?"0.16.24-botty6":"0.16.24-botty5"},{"files",json::array()}};
    for(const auto& package:{"botty","rtorrent"})for(const auto& file:serviceFiles(package))add(package,file,"service "+file);
    running={{"bootId","123:456"},{"manager",{{"pid",41},{"version","1.5.4"}}},{"worker",{{"pid",42},{"version","1.3.1"},{"bottyWorker","library-1.3"}}},{"engine",{{"pid",43},{"rpcPid",43},{"version","0.16.24-botty5"},{"bootId","123:456"}}}};
    anchor();release=installationRelease(fetch());
  }
  ~Fixture(){fs::remove_all(root);}
  void add(const std::string& package,const std::string& file,const std::string& data) {
    bytes[package+"/"+file]=data;manifests[package]["files"].push_back({{"path",file},{"size",data.size()},{"sha256",nativeHash(data)}});
  }
  void anchor() {
    index["sha256"]=json::object();
    for(auto it=manifests.begin();it!=manifests.end();++it){bytes[it.key()+"/manifest.json"]=it.value().dump();index["sha256"][it.key()+"/manifest.json"]=nativeHash(bytes.at(it.key()+"/manifest.json"));for(const auto& file:it.value().at("files"))index["sha256"][it.key()+"/"+file.at("path").get<std::string>()]=file.at("sha256");}
    bytes["botty-release.json"]=index.dump();
  }
  NativeUpdater::Fetch fetch(){return [this](const std::string& path,size_t limit){assert(installationPathAllowed(path));auto data=bytes.at(path);assert(data.size()<=limit);return data;};}
  fs::path managerRoot(){return root/"manager"/release.manager.at("id").get<std::string>();}
  fs::path engineRoot(){return root/"rtorrent"/release.engine.at("id").get<std::string>();}
  void stage(){stageService(managerRoot(),release.manager,"botty",fetch());stageService(engineRoot(),release.engine,"rtorrent",fetch());}
  json handoff(){return {{"schema",1},{"status","prepared"},{"transaction",std::string(32,'a')},{"bundle",release.record()},{"running",running}};}
};
int main() {
  {
    std::vector<unsigned char> table(960);
    for(int i=0;i<2;++i){const int32_t size=480,pid=41+i;std::memcpy(table.data()+480*i,&size,4);std::memcpy(table.data()+480*i+72,&pid,4);std::memcpy(table.data()+480*i+447,i?"botty-manager":"service-updater",i?14:16);}
    nativeProcessesStopped(table,41,42);reject([&]{nativeProcessesStopped(table,41,43);});
    std::memset(table.data()+480+447,0,32);std::memcpy(table.data()+480+447,"botty-manager-fake",19);reject([&]{nativeProcessesStopped(table,41,42);});
    std::memset(table.data()+480+447,0,32);std::memcpy(table.data()+480+447,"eboot.bin",10);reject([&]{nativeProcessesStopped(table,41,42);});
    const int32_t badSize=479;std::memcpy(table.data()+480,&badSize,4);reject([&]{nativeProcessesStopped(table,41,42);});
  }
  {
    Fixture fixture;NativeTransaction native(fixture.nativeConfig);
    installationNoDowngrade(fixture.release,fixture.running,native.installedVersion());
    assert(serviceVersion("1.5.10")>serviceVersion("1.5.9"));assert(serviceVersion("0.16.24-botty10",true)>serviceVersion("0.16.24-botty9",true));
    auto invalid=fixture.manifests;
    invalid["botty"]["apiVersion"]=2;reject([&]{validateInstallationCompatibility(invalid["botty-native"],invalid["botty"],invalid["rtorrent"]);});
    invalid=fixture.manifests;invalid["botty-native"]["requires"]["manager"]="1.5.6";reject([&]{validateInstallationCompatibility(invalid["botty-native"],invalid["botty"],invalid["rtorrent"]);});
    invalid=fixture.manifests;invalid["botty"]["workerApi"]="library-2";reject([&]{validateInstallationCompatibility(invalid["botty-native"],invalid["botty"],invalid["rtorrent"]);});
    invalid=fixture.manifests;invalid["botty"]["updaterVersion"]="2.0.0";reject([&]{validateInstallationCompatibility(invalid["botty-native"],invalid["botty"],invalid["rtorrent"]);});
    invalid=fixture.manifests;invalid["botty"]["files"][0]["size"]=16*1024*1024;reject([&]{validateServiceManifest(invalid["botty"],"botty");});
    invalid=fixture.manifests;invalid["rtorrent"]["files"][0]["size"]=NativeTransaction::maxFileBytes+1;reject([&]{validateServiceManifest(invalid["rtorrent"],"rtorrent");});
    invalid=fixture.manifests;invalid["botty"]["files"][1]["path"]="../token";reject([&]{validateServiceManifest(invalid["botty"],"botty");});
    invalid=fixture.manifests;invalid["botty"]["files"][1]=invalid["botty"]["files"][0];reject([&]{validateServiceManifest(invalid["botty"],"botty");});
    auto running=fixture.running;running["engine"]["bootId"]="previous";reject([&]{validateRunningInstallation(running);});
    running=fixture.running;running["engine"]["rpcPid"]=99;reject([&]{validateRunningInstallation(running);});
    running=fixture.running;running["worker"].erase("pid");reject([&]{validateRunningInstallation(running);});
    running=fixture.running;running["manager"]["version"]="1.5.6";reject([&]{installationNoDowngrade(fixture.release,running,native.installedVersion());});
    fixture.index["sha256"]["botty/game-compressor.elf"]=std::string(64,'a');fixture.bytes["botty-release.json"]=fixture.index.dump();reject([&]{installationRelease(fixture.fetch());});
    assert(!installationPathAllowed("botty/../token"));assert(!installationPathAllowed("https://evil/manifest.json"));
    reject([]{launchEmbeddedServiceUpdater();});
  }
  for(bool restart:{false,true}) {
    Fixture fixture(restart);fixture.stage();auto journal=fixture.handoff();std::vector<std::string> calls;const std::string hash(40,'a');bool published=false;
    ServiceUpdaterTransport io;
    io.inspect=[&]{return fixture.running;};io.gate=[]{};io.tick=[]{};io.persist=[&](const json& record){journal=record;};
    io.activeTorrents=[&]{return json::array({hash});};io.pause=[&](const std::string& value){assert(value==hash);calls.push_back("pause");};io.resume=[&](const std::string& value){assert(value==hash);assert(published);calls.push_back("resume");};io.saveSession=[&]{calls.push_back("save");};
    io.retire=[&](const std::string& service,const json&){assert(journal["attempts"]["retire-"+service]=="uncertain");calls.push_back("retire-"+service);};io.exited=[](const std::string&,int){return true;};
    io.load=[&](const std::string& service,const fs::path&) {assert(journal["attempts"]["load-"+service]=="uncertain");fixture.running[service]["pid"]=fixture.running[service]["pid"].get<int>()+100;if(service=="manager")fixture.running[service]["version"]="1.5.5";if(service=="engine"){fixture.running[service]["version"]="0.16.24-botty6";fixture.running[service]["rpcPid"]=fixture.running[service]["pid"];}calls.push_back("load-"+service);};
    io.publish=[&]{assert(fixture.running["manager"]["version"]=="1.5.5");published=true;};io.scan=[]{};
    io.publishConfig=[&](const fs::path& staged){assert(staged==fixture.engineRoot()/"rtorrent.rc");assert(std::count(calls.begin(),calls.end(),"retire-engine")==1);assert(std::count(calls.begin(),calls.end(),"load-engine")==0);calls.push_back("config");};
    ServiceUpdater updater(journal,fixture.release,io,fixture.managerRoot(),fixture.engineRoot());updater.run();assert(journal["status"]=="complete");assert(published);
    assert(std::count(calls.begin(),calls.end(),"config")==int(restart));
    assert(std::count(calls.begin(),calls.end(),"pause")==int(restart));assert(std::count(calls.begin(),calls.end(),"resume")==int(restart));assert(std::count(calls.begin(),calls.end(),"retire-engine")==int(restart));
  }
  {
    Fixture fixture;fixture.stage();auto journal=fixture.handoff();int stops=0,loads=0;
    ServiceUpdaterTransport io;io.inspect=[&]{return fixture.running;};io.gate=[]{};io.tick=[]{};io.persist=[&](const json& record){journal=record;};io.retire=[&](const std::string&,const json&){++stops;throw std::runtime_error("Ambiguous retirement response");};io.load=[&](const std::string&,const fs::path&){++loads;};
    ServiceUpdater updater(journal,fixture.release,io,fixture.managerRoot(),fixture.engineRoot());reject([&]{updater.run();});assert(journal["status"]=="recovery-required");assert(stops==1&&loads==0);
    ServiceUpdater retry(journal,fixture.release,io,fixture.managerRoot(),fixture.engineRoot());reject([&]{retry.run();});assert(stops==1&&loads==0);
  }
  {
    Fixture fixture(true);fixture.stage();const auto state=fixture.root/"rtorrent/state";fs::create_directories(state);assert(!::chmod(state.c_str(),0700));
    const auto active=state/"rtorrent.rc",previous=state/"rtorrent.rc.previous",staged=fixture.engineRoot()/"rtorrent.rc";
    nativeWrite(active,"old uncapped config");
    installationPublishEngineConfig(staged,state,fixture.release.engine);
    assert(nativeRead(active,NativeTransaction::maxFileBytes)==fixture.bytes.at("rtorrent/rtorrent.rc"));assert(nativeRead(previous,NativeTransaction::maxFileBytes)=="old uncapped config");
    installationPublishEngineConfig(staged,state,fixture.release.engine);assert(nativeRead(previous,NativeTransaction::maxFileBytes)=="old uncapped config");
    fs::remove(active);fs::remove(previous);installationPublishEngineConfig(staged,state,fixture.release.engine);
    assert(nativeRead(active,NativeTransaction::maxFileBytes)==fixture.bytes.at("rtorrent/rtorrent.rc")&&!fs::exists(previous));
    nativeWrite(active,"old uncapped config");
    auto changed=fixture.release.engine;for(auto& entry:changed["files"])if(entry["path"]=="rtorrent.rc")entry["sha256"]=std::string(64,'a');
    reject([&]{installationPublishEngineConfig(staged,state,changed);});assert(nativeRead(active,NativeTransaction::maxFileBytes)=="old uncapped config");
  }
  {
    Fixture fixture;fixture.stage();fs::create_symlink(fixture.root/"private",fixture.managerRoot()/"unexpected");reject([&]{verifyServiceTree(fixture.managerRoot(),fixture.release.manager,"botty");});
  }
  for(const std::string package:{"botty","rtorrent"}) {
    Fixture fixture;const auto root=package=="botty"?fixture.managerRoot():fixture.engineRoot();
    const auto manifest=package=="botty"?fixture.release.manager:fixture.release.engine;
    int calls=0;
    reject([&]{stageService(root,manifest,package,[&](const std::string& path,size_t size){if(++calls==2)throw std::runtime_error("Injected interrupted download");return fixture.fetch()(path,size);});});
    assert(calls==2&&!fs::exists(root));
    const auto staging=root.parent_path()/(".staging-"+root.filename().string()+"-"+nativeHash(manifest.dump()));
    const auto first=manifest.at("files")[0].at("path").get<std::string>();assert(fs::is_regular_file(staging/first));
    int resumed=0;
    stageService(root,manifest,package,[&](const std::string& path,size_t size){assert(path!=package+"/"+first);++resumed;return fixture.fetch()(path,size);});
    assert(resumed==int(manifest.at("files").size())-1&&!fs::exists(staging));verifyServiceTree(root,manifest,package);
    stageService(root,manifest,package,[](const std::string&,size_t)->std::string{throw std::runtime_error("Published immutable directory must not download");});
    auto changed=manifest;changed["files"][0]["sha256"]=std::string(64,'a');
    reject([&]{stageService(root,changed,package,fixture.fetch());});assert(nativeRead(root/first,NativeTransaction::maxFileBytes)==fixture.bytes.at(package+"/"+first));
  }
  for(bool exitConfirmed:{false,true}) {
    Fixture fixture;fixture.stage();auto journal=fixture.handoff();int loads=0,publications=0;
    ServiceUpdaterTransport io;io.inspect=[&]{return fixture.running;};io.gate=[]{};io.tick=[]{};io.persist=[&](const json& record){journal=record;};io.retire=[](const std::string&,const json&){};io.exited=[&](const std::string&,int){return exitConfirmed;};io.load=[&](const std::string&,const fs::path&){++loads;};io.publish=[&]{++publications;};
    ServiceUpdater updater(journal,fixture.release,io,fixture.managerRoot(),fixture.engineRoot());reject([&]{updater.run();});assert(journal["status"]=="recovery-required");assert(publications==0);assert(loads==(exitConfirmed?2:0));
  }
  {
    Fixture fixture;NativeTransaction native(fixture.nativeConfig);std::atomic<bool> idle{false},closed{false},owned{true};std::atomic<int> launches{0};
    InstallationUpdater::Config cfg;cfg.root=fixture.root;cfg.inspect=[&]{return fixture.running;};cfg.download=[&](const std::string& path,size_t limit,const std::atomic<bool>&){return fixture.fetch()(path,limit);};
    cfg.idle=[&]{if(!idle)throw std::runtime_error("Existing work must finish");};cfg.stopped=[&]{if(!closed)throw std::runtime_error("Close Botty+");};cfg.owns=[&](const json&){return owned.load();};cfg.launch=[&](const json& handoff){++launches;auto taken=handoff;taken["status"]="takeover";nativeWrite(fixture.root/"installation/handoff.json",taken.dump());};
    InstallationUpdater updater(native,cfg);updater.start();until([&]{return updater.state()["status"]=="available";});assert(updater.state()["installedVersion"]==updater.state()["availableVersion"]);assert(updater.state()["updateAvailable"]==true);
    const auto ack=updater.request();assert(ack["scope"]=="installation"&&ack["serviceVersion"]=="1.5.5"&&ack["version"]=="01.004.002");assert(std::regex_match(ack["transaction"].get<std::string>(),std::regex("[a-f0-9]{32}")));reject([&]{updater.requireAdmission();});assert(updater.request()==ack);assert(launches==0);
    idle=true;updater.recheck();until([&]{return updater.state()["message"]=="Close Botty+";});assert(launches==0);closed=true;updater.recheck();until([&]{return updater.finished();});assert(launches==1);assert(native.installedVersion()=="01.004.002");
    owned=false;assert(updater.state()["status"]=="blocked");reject([&]{updater.requireAdmission();});assert(launches==1);
  }
  {
    Fixture fixture;NativeTransaction native(fixture.nativeConfig);std::atomic<bool> entered{false};InstallationUpdater::Config cfg;cfg.root=fixture.root;
    cfg.download=[&](const std::string&,size_t,const std::atomic<bool>& cancelled){entered=true;while(!cancelled)std::this_thread::sleep_for(std::chrono::milliseconds(1));throw std::runtime_error("cancelled");return std::string{};};
    auto updater=std::make_unique<InstallationUpdater>(native,cfg);updater->start();until([&]{return entered.load();});const auto begin=std::chrono::steady_clock::now();updater.reset();assert(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(250));
  }
  for(bool workerMatches:{false,true}) {
    Fixture fixture;NativeTransaction native(fixture.nativeConfig);native.stage(fixture.release.native.manifest,fixture.release.native.hash,[&](const std::string& path,size_t limit){return fixture.fetch()("botty-native/"+path,limit);});native.publish(fixture.release.native.manifest,fixture.release.native.hash,[]{});
    auto handoff=fixture.handoff();handoff["status"]="complete";nativeWrite(fixture.root/"installation/handoff.json",handoff.dump());
    nativeWrite(fixture.root/"installation/request.json",json({{"schema",1},{"status","handoff"},{"transaction",handoff["transaction"]},{"hash",fixture.release.hash},{"version","01.004.002"},{"serviceVersion","1.5.5"},{"workerVersion","1.3.1"},{"engineVersion","0.16.24-botty5"}}).dump());
    fixture.running["manager"]["version"]="1.5.5";fixture.running["manager"]["pid"]=141;fixture.running["worker"]["pid"]=142;if(!workerMatches)fixture.running["worker"]["version"]="1.3.0";
    InstallationUpdater::Config cfg;cfg.root=fixture.root;cfg.managerVersion="1.5.5";cfg.inspect=[&]{return fixture.running;};cfg.download=[&](const std::string& path,size_t limit,const std::atomic<bool>&){return fixture.fetch()(path,limit);};cfg.launch=[](const json&){assert(false);};
    InstallationUpdater updater(native,cfg);updater.start();until([&]{return workerMatches?!updater.requested():updater.state()["status"]=="blocked";});
    if(workerMatches){updater.requireAdmission();assert(updater.state()["installedWorkerVersion"]=="1.3.1");}else reject([&]{updater.requireAdmission();});
  }
  {
    Fixture fixture;NativeTransaction native(fixture.nativeConfig);nativeWrite(fixture.root/"native/request.json",json({{"schema",1},{"status","queued"}}).dump());
    InstallationUpdater::Config cfg;cfg.root=fixture.root;cfg.download=[](const std::string&,size_t,const std::atomic<bool>&){assert(false);return std::string{};};
    InstallationUpdater updater(native,cfg);updater.start();until([&]{return updater.finished();});assert(updater.requested());reject([&]{updater.requireAdmission();});
  }
  for(bool compressionFailure:{false,true}) {
    Fixture fixture;NativeTransaction native(fixture.nativeConfig);const auto original=native.installedFingerprint();std::atomic<bool> fail{true},closed{true};std::atomic<int> downloads{0},launches{0};
    InstallationUpdater::Config cfg;cfg.root=fixture.root;cfg.inspect=[&]{return fixture.running;};
    cfg.download=[&](const std::string& path,size_t limit,const std::atomic<bool>&) {
      if(path.rfind("botty-native/",0)==0&&path!="botty-native/manifest.json"){++downloads;if(fail&&!compressionFailure)throw std::runtime_error("Injected pre-handoff download failure");}
      return fixture.fetch()(path,limit);
    };
    cfg.idle=[&]{if(fail&&compressionFailure)throw std::logic_error("Protected compression needs manual recovery");};cfg.stopped=[&]{if(!closed)throw std::runtime_error("Close Botty+");};cfg.launch=[&](const json&){++launches;};
    std::string transaction;
    {
      InstallationUpdater updater(native,cfg);updater.start();until([&]{return updater.state()["status"]=="available";});transaction=updater.request().at("transaction");updater.recheck();
      until([&]{return updater.state()["status"]=="error";});assert(!updater.requested());assert(!updater.finished());updater.requireAdmission();assert(updater.state()["closeRequired"]==false);
      const auto failed=installationRecord(fixture.root/"installation/request.json",65536);assert(failed["status"]=="failed"&&failed["previous"]==original&&failed["running"]==fixture.running);assert(native.installedFingerprint()==original);assert(launches==0);assert(!fs::exists(fixture.root/"installation/handoff.json"));
      if(compressionFailure)assert(downloads==0);
      updater.recheck();until([&]{return updater.state()["status"]=="available";});assert(!updater.requested());
    }
    fail=false;closed=false;
    InstallationUpdater restarted(native,cfg);restarted.start();until([&]{return restarted.state()["status"]=="available";});assert(!restarted.requested());restarted.requireAdmission();
    assert(installationRecord(fixture.root/"installation/request.json",65536)["status"]=="failed");const auto next=restarted.request();assert(next["transaction"]!=transaction);assert(restarted.requested());assert(launches==0);
  }
  for(bool changedRuntime:{false,true}) {
    Fixture fixture;NativeTransaction native(fixture.nativeConfig);InstallationUpdater::Config cfg;cfg.root=fixture.root;cfg.inspect=[&]{return fixture.running;};cfg.idle=[]{};cfg.stopped=[]{};cfg.launch=[](const json&){assert(false);};
    cfg.download=[&](const std::string& path,size_t limit,const std::atomic<bool>&) {
      if(path.rfind("botty-native/",0)==0&&path!="botty-native/manifest.json") {
        if(changedRuntime)fixture.running["manager"]["pid"]=141;else nativeWrite(fixture.nativeConfig.nativeRoot/"eboot.bin","changed original");
        throw std::runtime_error("Injected failure after originals changed");
      }
      return fixture.fetch()(path,limit);
    };
    InstallationUpdater updater(native,cfg);updater.start();until([&]{return updater.state()["status"]=="available";});updater.request();updater.recheck();until([&]{return updater.state()["status"]=="blocked";});assert(updater.requested());reject([&]{updater.requireAdmission();});assert(installationRecord(fixture.root/"installation/request.json",65536)["status"]=="queued");
  }
  {
    Fixture fixture;auto handoff=fixture.handoff();handoff["status"]="takeover";handoff["helperPid"]=getppid();assert(getppid()>1);
    nativeWrite(fixture.root/"installation/updater.lock","");const int fd=::open((fixture.root/"installation/updater.lock").c_str(),O_RDWR|O_NOFOLLOW);assert(fd>=0);assert(!::flock(fd,LOCK_EX|LOCK_NB));
    assert(installationHandoffOwned(fixture.root,handoff,"123:456"));assert(!installationHandoffOwned(fixture.root,handoff,"999:456"));
    auto reused=handoff;reused["running"]["bootId"]="old-boot";assert(!installationHandoffOwned(fixture.root,reused,"123:456"));
    assert(!::flock(fd,LOCK_UN));assert(!installationHandoffOwned(fixture.root,handoff,"123:456"));::close(fd);
    NativeTransaction native(fixture.nativeConfig);nativeWrite(fixture.root/"installation/handoff.json",handoff.dump());
    nativeWrite(fixture.root/"installation/request.json",json({{"schema",1},{"status","handoff"},{"transaction",handoff["transaction"]},{"hash",fixture.release.hash},{"version","01.004.002"},{"serviceVersion","1.5.5"},{"workerVersion","1.3.1"},{"engineVersion","0.16.24-botty5"}}).dump());
    InstallationUpdater::Config cfg;cfg.root=fixture.root;cfg.owns=[&](const json& record){return installationHandoffOwned(fixture.root,record,"123:456");};cfg.launch=[](const json&){assert(false);};
    InstallationUpdater updater(native,cfg);updater.start();until([&]{return updater.state()["status"]=="blocked";});assert(updater.requested());assert(installationRecord(fixture.root/"installation/request.json",65536)["status"]=="handoff");reject([&]{updater.requireAdmission();});
  }
  std::cout<<"Installation compatibility, handoff, lifecycle, admission and cancellation tests passed\n";
}
