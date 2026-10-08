#include "native-transaction.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>

using namespace botty;
static void put(const fs::path& path,const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path,std::ios::binary);out<<bytes;out.close();
}
template<class F> static void refuses(F action) {
  bool threw=false;try {action();}catch(const std::exception&){threw=true;}assert(threw);
}
static std::string param(const std::string& version) {
  return json({{"titleId","PPSA99071"},{"contentId","UP9000-PPSA99071_00-BOTTYNATIVE00001"},{"contentVersion",version},{"localizedParameters",{{"en-US",{{"titleName","Botty+"}}}}}}).dump();
}
struct Fixture {
  fs::path root;
  NativeTransaction::Config config;
  json manifest;
  std::map<std::string,std::string> data;
  std::string hash=nativeHash("manifest");
  Fixture() {
    auto base=fs::temp_directory_path()/"botty-native-test-XXXXXX";
    auto name=base.string();assert(::mkdtemp(name.data()));root=name;
    config.nativeRoot=root/"homebrew/PPSA99071";config.stateRoot=root/"state";
    config.metadataRoots={{root/"app/PPSA99071/sce_sys",root/"appmeta/PPSA99071",root/"system/appmeta/PPSA99071"}};
    put(config.nativeRoot/"sce_sys/param.json",param("01.003.003"));
    put(config.nativeRoot/"original.dat","untouched original");
    fs::create_directory(config.stateRoot);
    manifest={{"schema",1},{"titleId","PPSA99071"},{"version","01.003.004"},{"files",json::array()}};
    for(const auto& file:NativeTransaction::files()) {
      data[file]=file=="sce_sys/param.json"?param("01.003.004"):"new "+file;
      if(file!="sce_sys/param.json")put(config.nativeRoot/file,"old "+file);
      manifest["files"].push_back({{"path",file},{"size",data[file].size()},{"sha256",nativeHash(data[file])}});
    }
  }
  ~Fixture(){fs::remove_all(root);}
  NativeTransaction::Fetch fetch(){return [&](const std::string& file,size_t size){assert(data.at(file).size()==size);return data.at(file);};}
  json record(){return json::parse(nativeRead(config.stateRoot/"update.json",8192));}
  fs::path backup(){return record().at("backup").get<std::string>();}
};
int main() {
  assert(nativeHash("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(nativeHash("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(nativeHash(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  for(const auto name:{"botty-131-20261003","botty-131-stackfix-20261003"}){
    Fixture f;NativeTransaction t(f.config);
    json record={{"schema",1},{"status","complete"},{"target",f.hash},{"previous",nativeHash("previous")},{"backup",(f.config.stateRoot/"backups"/name/"PPSA99071").string()}};
    put(f.config.stateRoot/"update.json",record.dump());
    assert(!t.pendingJournal());t.preflight(f.manifest);
    record["status"]="pending";put(f.config.stateRoot/"update.json",record.dump());refuses([&]{t.pendingJournal();});
    record["status"]="complete";record["backup"]=(f.config.stateRoot/"backups/another-named-backup/PPSA99071").string();put(f.config.stateRoot/"update.json",record.dump());refuses([&]{t.pendingJournal();});
  }
  {
    Fixture f;NativeTransaction t(f.config);assert(t.installedVersion()=="01.003.003");
    auto bad=f.manifest;bad["files"][0]["path"]="../evil";refuses([&]{t.preflight(bad);});
    bad=f.manifest;bad["files"][0]=bad["files"][1];refuses([&]{t.preflight(bad);});
    bad=f.manifest;bad["files"][0]["size"]=0;refuses([&]{t.preflight(bad);});
    bad=f.manifest;bad["files"][0]["sha256"]="abc";refuses([&]{t.preflight(bad);});
    bad=f.manifest;bad["files"].erase(0);refuses([&]{t.preflight(bad);});
    bad=f.manifest;bad["version"]="01.003.002";refuses([&]{t.preflight(bad);});
    refuses([&]{t.stage(f.manifest,"../bad",f.fetch());});
    refuses([&]{t.stage(f.manifest,f.hash,[](const std::string&,size_t){return "bad";});});
    assert(!fs::exists(f.config.stateRoot/f.hash));
    auto foreign=json::parse(param("01.003.003"));foreign["contentId"]="foreign";
    put(f.config.nativeRoot/"sce_sys/param.json",foreign.dump());refuses([&]{t.preflight(f.manifest);});
  }
  {
    Fixture f;NativeTransaction t(f.config);
    ::chmod(f.config.stateRoot.c_str(),0777);refuses([&]{t.preflight(f.manifest);});assert(fs::is_empty(f.config.stateRoot));
    ::chmod(f.config.stateRoot.c_str(),0755);
    fs::create_directory_symlink(f.config.nativeRoot,f.config.stateRoot/"link");refuses([&]{t.preflight(f.manifest);});
    fs::remove(f.config.stateRoot/"link");
    fs::create_directory_symlink(f.config.nativeRoot,f.root/"alias");
    refuses([&]{nativeRead(f.root/"alias/sce_sys/param.json",16384);});
    refuses([&]{nativeWrite(f.root/"alias/test","bad");});
    put(f.root/"bounded","12345");refuses([&]{nativeRead(f.root/"bounded",4);});
    nativeWrite(f.root/"atomic","first",true);refuses([&]{nativeWrite(f.root/"atomic","second",true);});
    assert(nativeRead(f.root/"atomic",10)=="first");
  }
  {
    Fixture f;NativeTransaction t(f.config);t.stage(f.manifest,f.hash,f.fetch());
    refuses([&]{t.publish(f.manifest,f.hash,[]{throw std::runtime_error("running");});});
    assert(!fs::exists(f.config.stateRoot/"update.json"));assert(t.installedVersion()=="01.003.003");
    put(f.config.stateRoot/f.hash/"PPSA99071/eboot.bin","corrupt");
    refuses([&]{t.publish(f.manifest,f.hash,[]{});});assert(t.installedVersion()=="01.003.003");
  }
  for(const std::string point:{"pending","backup","published"}) {
    Fixture f;f.config.checkpoint=[&](const std::string& p){if(p==point)throw std::runtime_error("power loss");};
    NativeTransaction t(f.config);t.stage(f.manifest,f.hash,f.fetch());refuses([&]{t.publish(f.manifest,f.hash,[]{});});
    assert(f.record().at("status")=="pending");
    auto config=f.config;config.checkpoint={};NativeTransaction reboot(config);
    refuses([&]{reboot.recover(f.manifest,f.hash,[]{throw std::runtime_error("busy");});});
    reboot.recover(f.manifest,f.hash,[]{});
    if(point=="published") {assert(reboot.installedVersion()=="01.003.004");assert(f.record().at("status")=="complete");}
    else {assert(reboot.installedVersion()=="01.003.003");assert(f.record().at("status")=="rolled-back");}
    if(point!="pending")assert(nativeRead(f.backup()/"original.dat",1024)=="untouched original");
  }
  {
    Fixture f;f.config.checkpoint=[](const std::string& p){if(p=="backup")throw std::runtime_error("power loss");};
    NativeTransaction t(f.config);t.stage(f.manifest,f.hash,f.fetch());refuses([&]{t.publish(f.manifest,f.hash,[]{});});
    put(f.config.nativeRoot/"foreign","preserve");
    refuses([&]{t.recover(f.manifest,f.hash,[]{});});
    assert(nativeRead(f.config.nativeRoot/"foreign",100)=="preserve");assert(fs::exists(f.backup()/"original.dat"));
    auto backup=f.backup();
    put(f.config.stateRoot/"update.json","{bad");refuses([&]{t.preflight(f.manifest);});assert(fs::exists(backup/"original.dat"));
  }
  {
    Fixture f;
    for(const auto& root:f.config.metadataRoots) {put(root/"param.json",param("01.003.003"));put(root/"icon0.png","old icon");put(root/"pic0.dds","old pic");put(root/"snd0.at9","old snd");}
    put(f.config.metadataRoots[0].parent_path()/"icon0.png","old app icon");
    NativeTransaction t(f.config);t.stage(f.manifest,f.hash,f.fetch());t.publish(f.manifest,f.hash,[]{});
    assert(t.installedVersion()=="01.003.004");assert(f.record().at("status")=="complete");
    for(const auto& root:f.config.metadataRoots) for(const auto& name:{"param.json","icon0.png","pic0.dds","snd0.at9"}) assert(nativeRead(root/name,16384)==f.data.at("sce_sys/"+std::string(name)));
    auto metadata=f.backup().parent_path()/"metadata";
    assert(nativeRead(metadata/"0.bin",16384)==param("01.003.003"));
    assert(nativeRead(metadata/"9.bin",100)=="old app icon");assert(nativeRead(metadata/"10.bin",100)=="old snd");
    assert(nativeRead(f.backup()/"original.dat",100)=="untouched original");
    struct stat st{};assert(!::stat((f.config.nativeRoot/"eboot.bin").c_str(),&st));assert((st.st_mode&0777)==0755);
  }
  {
    Fixture f;put(f.config.metadataRoots[0]/"param.json","foreign");NativeTransaction t(f.config);
    refuses([&]{t.stage(f.manifest,f.hash,f.fetch());});assert(fs::is_empty(f.config.stateRoot));
  }
  {
    Fixture f;
    put(f.config.metadataRoots[1]/"icon0.png","unregistered icon");
    NativeTransaction t(f.config);t.stage(f.manifest,f.hash,f.fetch());t.publish(f.manifest,f.hash,[]{});
    assert(nativeRead(f.config.metadataRoots[1]/"icon0.png",100)=="unregistered icon");
    auto record=f.record();record["backup"]=(f.root/"outside/PPSA99071").string();
    put(f.config.stateRoot/"update.json",record.dump());refuses([&]{t.preflight(f.manifest);});
  }
  {
    Fixture f;f.config.checkpoint=[](const std::string& p){if(p=="backup")throw std::runtime_error("power loss");};
    NativeTransaction t(f.config);t.stage(f.manifest,f.hash,f.fetch());refuses([&]{t.publish(f.manifest,f.hash,[]{});});
    fs::create_directory(f.config.nativeRoot);t.recover(f.manifest,f.hash,[]{});
    assert(t.installedVersion()=="01.003.003");assert(fs::exists(f.backup()/"original.dat"));
  }
  {
    Fixture f;put(f.config.nativeRoot/"private/nested/save.dat","retained data");
    const std::vector<std::pair<fs::path,mode_t>> directories={{f.config.nativeRoot,02710},{f.config.nativeRoot/"assets",0750},{f.config.nativeRoot/"private",01700},{f.config.nativeRoot/"private/nested",0711}};
    for(const auto& entry:directories)assert(!::chmod(entry.first.c_str(),entry.second));
    assert(!::chmod((f.config.nativeRoot/"private/nested/save.dat").c_str(),06740));
    f.config.checkpoint=[](const std::string& point){if(point=="backup")throw std::runtime_error("power loss");};
    NativeTransaction transaction(f.config);transaction.stage(f.manifest,f.hash,f.fetch());refuses([&]{transaction.publish(f.manifest,f.hash,[]{});});transaction.recover(f.manifest,f.hash,[]{});
    for(const auto& entry:directories){struct stat st{};assert(!::lstat(entry.first.c_str(),&st));assert((st.st_mode&07777)==entry.second);}
    struct stat st{};assert(!::lstat((f.config.nativeRoot/"private/nested/save.dat").c_str(),&st));assert((st.st_mode&07777)==0740);
    assert(!::lstat((f.backup()/"private/nested/save.dat").c_str(),&st));assert((st.st_mode&07777)==06740);
    assert(nativeRead(f.config.nativeRoot/"private/nested/save.dat",128)=="retained data");
  }
  {
    Fixture f;fs::remove_all(f.config.nativeRoot);NativeTransaction t(f.config);
    refuses([&]{t.installedVersion();});refuses([&]{t.stage(f.manifest,f.hash,f.fetch());});assert(fs::is_empty(f.config.stateRoot));
  }
  {
    Fixture f;f.data["sce_sys/param.json"]=param("01.003.005");
    for(auto& file:f.manifest["files"])if(file.at("path")=="sce_sys/param.json") {file["size"]=f.data["sce_sys/param.json"].size();file["sha256"]=nativeHash(f.data["sce_sys/param.json"]);}
    NativeTransaction t(f.config);refuses([&]{t.stage(f.manifest,f.hash,f.fetch());});assert(t.installedVersion()=="01.003.003");
    refuses([&]{t.publish(f.manifest,f.hash,[]{});});assert(!fs::exists(f.config.stateRoot/"update.json"));
  }
  {
    Fixture f;NativeTransaction t(f.config);auto fingerprint=t.installedFingerprint();
    assert(fingerprint.size()==64);assert(t.installedFingerprint()==fingerprint);
    assert(!t.pendingJournal());assert(t.canReleaseRequest(fingerprint));
    refuses([&]{t.stage(f.manifest,f.hash,[](const std::string&,size_t){throw std::runtime_error("download failed");return std::string{};});});
    assert(t.canReleaseRequest(fingerprint));
    put(f.config.nativeRoot/"eboot.bin","unexpected executable");
    refuses([&]{t.canReleaseRequest(fingerprint);});
    fs::remove(f.config.nativeRoot/"eboot.bin");refuses([&]{t.installedFingerprint();});
    refuses([&]{t.canReleaseRequest(fingerprint);});
  }
  {
    Fixture f;f.config.checkpoint=[](const std::string& p){if(p=="pending")throw std::runtime_error("power loss");};
    NativeTransaction t(f.config);auto fingerprint=t.installedFingerprint();
    t.stage(f.manifest,f.hash,f.fetch());refuses([&]{t.publish(f.manifest,f.hash,[]{});});
    assert(t.pendingJournal());assert(!t.canReleaseRequest(fingerprint));
    t.recover(f.manifest,f.hash,[]{});
    assert(!t.pendingJournal());assert(t.canReleaseRequest(fingerprint));
    put(f.config.stateRoot/"update.json","damaged");
    refuses([&]{t.pendingJournal();});refuses([&]{t.canReleaseRequest(fingerprint);});
  }
  {
    Fixture f;NativeTransaction t(f.config);auto old=t.installedFingerprint();
    t.stage(f.manifest,f.hash,f.fetch());t.publish(f.manifest,f.hash,[]{});
    assert(!t.pendingJournal());refuses([&]{t.canReleaseRequest(old);});
    assert(t.canReleaseRequest(t.installedFingerprint()));
    fs::remove(f.config.nativeRoot/"assets/build.txt");
    fs::create_symlink(f.config.nativeRoot/"eboot.bin",f.config.nativeRoot/"assets/build.txt");
    refuses([&]{t.installedFingerprint();});
  }
  std::cout<<"native transaction tests passed\n";
}
