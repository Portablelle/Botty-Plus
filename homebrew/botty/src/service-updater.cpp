#include "installation-runtime.hpp"
#include "notification.hpp"
#include "compression-library.hpp"
#include <iostream>

using namespace botty;
int main() {
#ifndef __PS5__
  std::cerr<<"Production service updater is disabled on the host. Use injected lifecycle tests.\n";return 1;
#else
  const fs::path root="/data/botty",handoffPath=root/"installation/handoff.json";
  int lock=-1;
  try {
    umask(0077);signal(SIGPIPE,SIG_IGN);
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("Cannot initialize updater transport.");
    auto handoff=installationRecord(handoffPath,256*1024);
    const auto request=installationRecord(root/"installation/request.json",65536);
    if(request.at("transaction")!=handoff.at("transaction")||request.at("hash")!=handoff.at("bundle").at("hash")||request.at("status")!="handoff")throw std::runtime_error("Untrusted installation handoff.");
    if(handoff.at("capability")!=nativeHash(handoff.at("transaction").get<std::string>()+nativeRead(root/"installation/secret",128)))throw std::runtime_error("Invalid updater handoff capability.");
    lock=::open((root/"installation/updater.lock").c_str(),O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(lock<0||::flock(lock,LOCK_EX|LOCK_NB))throw std::runtime_error("A service updater already owns this installation.");
    const auto& bundle=handoff.at("bundle");
    InstallationRelease release={{bundle.at("native"),bundle.at("nativeHash"),bundle.at("hash")},bundle.at("manager"),bundle.at("engine"),bundle.at("hash")};
    Rtorrent engine(Paths(root),5001);NativeTransaction native;
    if(!native.stagedComplete(release.native.manifest,release.native.hash))throw std::runtime_error("Native staging verification failed before takeover.");
    installationNoDowngrade(release,handoff.at("running"),native.installedVersion());
    const auto inspect=[&]{return inspectInstallation(root,engine);};
    const auto idle=[&] {
      const auto worker=installationHttp(5910,"/api/gc/job?token="+installationWorkerToken(root));
      if(worker.at("ok")!=true||!worker.at("busy").is_boolean()||worker.at("busy").get<bool>())throw std::runtime_error("Worker idle state is not confirmed.");
      const auto health=installationHttp(8088,"/api/installation/status");
      if(health.at("updateTransaction")!=handoff.at("transaction")||health.at("updateQueued")!=true)throw std::runtime_error("Manager is not gating file work for this transaction.");
      const auto durable=installationRecord(handoffPath,256*1024);
      if(inspect()!=durable.value("selectedRunning",durable.at("running")))throw std::runtime_error("Service identities changed at the publication gate.");
      confirmNativeProcessesStopped(health.at("pid").get<int>());
    };
    ServiceUpdaterTransport io;
    io.inspect=inspect;io.gate=idle;
    io.activeTorrents=[&] {
      auto result=json::array();
      for(const auto& row:engine.installationRpc("d.multicall",{"","main","d.hash=","d.is_active="})) {
        if(!row.is_array()||row.size()!=2||!row[1].is_number_integer())throw std::runtime_error("Invalid active torrent snapshot.");
        if(row[1].get<int>())result.push_back(row[0]);
      }
      return result;
    };
    io.pause=[&](const std::string& hash) {
      engine.installationRpc("d.stop",{hash});
      for(int i=0;i<120;++i){if(!engine.installationRpc("d.is_active",{hash}).get<int>())return;std::this_thread::sleep_for(std::chrono::milliseconds(250));}
      throw std::runtime_error("Torrent pause was not confirmed. Keep its durable intent for recovery.");
    };
    io.resume=[&](const std::string& hash) {
      const auto durable=installationRecord(handoffPath,256*1024);
      if(engine.installationRpc("system.pid")!=durable.at("selectedRunning").at("engine").at("pid"))throw std::runtime_error("Engine identity changed before restoring updater-paused torrents.");
      engine.installationRpc("d.start",{hash});
      if(!engine.installationRpc("d.is_active",{hash}).get<int>()&&!engine.installationRpc("d.hashing",{hash}).get<int>())engine.installationRpc("d.resume",{hash});
      if(!engine.installationRpc("d.is_active",{hash}).get<int>()&&!engine.installationRpc("d.hashing",{hash}).get<int>())throw std::runtime_error("Updater-paused torrent restoration was not confirmed.");
    };
    io.saveSession=[&]{engine.installationRpc("session.save");};
    io.retire=[&](const std::string& service,const json& identity) {
      if(service=="manager") {
        const json body={{"transaction",handoff.at("transaction")}};
        json response;
        for(int i=0;i<120;++i) {
          const auto health=installationHttp(8088,"/api/installation/status");
          if(health.at("pid")!=identity.at("pid"))throw std::runtime_error("Manager identity changed before retirement.");
          if(health.value("updaterThreadFinished",false))break;
          if(i==119)throw std::runtime_error("Original manager updater thread has not released the handoff.");
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        response=installationHttp(8088,"/api/installation/retire",&body,handoff.at("capability"));
        if(response.at("status")!="retiring"||response.at("transaction")!=handoff.at("transaction")||response.at("pid")!=identity.at("pid"))throw std::runtime_error("Manager retirement response mismatch.");
      }else if(service=="worker") {
        const auto token=installationWorkerToken(root);const auto current=installationHttp(5910,"/api/status?token="+token);
        if(current.at("pid")!=identity.at("pid")||current.at("version")!=identity.at("version"))throw std::runtime_error("Worker identity changed before retirement.");
        const json body=json::object();const auto response=installationHttp(5910,"/api/control/shutdown?token="+token,&body);
        if(response.at("ok")!=true||response.at("shutdown")!=true)throw std::runtime_error("Worker retirement was not acknowledged.");
      }else if(service=="engine") {
        if(engine.installationRpc("system.pid")!=identity.at("pid"))throw std::runtime_error("Engine identity changed before retirement.");
        engine.installationRpc("system.shutdown.normal");
      }else throw std::runtime_error("Unexpected service retirement.");
    };
    io.exited=installationExited;
    io.load=[&](const std::string& service,const fs::path& path) {
      const int port=service=="manager"?8088:service=="worker"?5910:service=="engine"?5001:0;
      if(!port||!installationPortClosed(port))throw std::runtime_error("Replacement service port is already occupied; duplicate launch refused.");
      const auto& manifest=service=="engine"?release.engine:release.manager;
      const auto file=std::find_if(manifest.at("files").begin(),manifest.at("files").end(),[&](const json& entry){return entry.at("path")==path.filename().string();});
      if(file==manifest.at("files").end())throw std::runtime_error("Unexpected replacement payload.");
      const auto bytes=nativeRead(path,NativeTransaction::maxFileBytes);
      if(bytes.size()!=file->at("size").get<size_t>()||nativeHash(bytes)!=file->at("sha256"))throw std::runtime_error("Replacement payload changed after staging.");
      installationLoad(bytes);
    };
    io.persist=[&](const json& record){nativeWrite(handoffPath,record.dump()+"\n");};
    io.publish=[&] {
      native.publish(release.native.manifest,release.native.hash,[&]{idle();CompressionLibrary(Paths(root),10101).api("games/unmount",{{"title_id","PPSA99071"}});idle();});
      if(native.installedVersion()!=release.native.manifest.at("version"))throw std::runtime_error("Published native version confirmation failed.");
    };
    io.scan=[&]{CompressionLibrary(Paths(root),10101).api("scan",{{"reset_attempts",false}});};
    io.notify=notifySystem;
    ServiceUpdater updater(handoff,release,std::move(io),root/"manager"/release.manager.at("id").get<std::string>(),root/"rtorrent"/release.engine.at("id").get<std::string>());
    updater.run();::close(lock);return 0;
  }catch(const std::exception& error) {
    if(lock>=0)::close(lock);
    std::cerr<<"Service update stopped: "<<error.what()<<'\n';return 1;
  }
#endif
}
