#pragma once
#include "installation-release.hpp"
#include <csignal>
#include <sys/file.h>

namespace botty {
inline bool installationHandoffOwned(const fs::path& root,const json& handoff,const std::string& bootId) {
  if(handoff.at("running").at("bootId")!=bootId||!handoff.contains("helperPid")||!handoff.at("helperPid").is_number_integer())return false;
  const auto helper=handoff.at("helperPid").get<int>();
  if(helper<=1||helper==getpid()||::kill(helper,0))return false;
  const int fd=::open((root/"installation/updater.lock").c_str(),O_RDWR|O_NOFOLLOW);
  if(fd<0)return false;
  struct stat st{};
  if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)){::close(fd);return false;}
  const auto result=::flock(fd,LOCK_EX|LOCK_NB),error=errno;
  if(result==0)::flock(fd,LOCK_UN);
  ::close(fd);
  return result!=0&&(error==EWOULDBLOCK||error==EAGAIN);
}
inline void validateRunningInstallation(const json& identity) {
  const auto boot=identity.at("bootId").get<std::string>();
  if(boot.empty()||boot.size()>128)throw std::runtime_error("Current boot identity is unavailable. Bootstrap the modern services through Portal.");
  std::set<int> pids;
  for(const auto& service:{"manager","worker","engine"}) {
    const auto& item=identity.at(service);
    if(!item.at("pid").is_number_integer()||item.at("pid").get<int>()<=1||!pids.insert(item.at("pid").get<int>()).second)throw std::runtime_error("Running service identity is unavailable. Bootstrap the modern services through Portal.");
    serviceVersion(item.at("version").get<std::string>(),std::string(service)=="engine");
  }
  if(identity.at("worker").at("bottyWorker")!="library-1.3"||identity.at("engine").at("bootId")!=boot||identity.at("engine").at("rpcPid")!=identity.at("engine").at("pid"))throw std::runtime_error("Running service evidence does not match this boot. Bootstrap the modern services through Portal.");
}
inline void installationNoDowngrade(const InstallationRelease& release,const json& identity,const std::string& nativeVersion) {
  validateRunningInstallation(identity);
  if(release.native.manifest.at("version").get<std::string>()<nativeVersion||serviceVersion(release.manager.at("id"))<serviceVersion(identity.at("manager").at("version"))||serviceVersion(release.manager.at("workerVersion"))<serviceVersion(identity.at("worker").at("version"))||serviceVersion(release.engine.at("id"),true)<serviceVersion(identity.at("engine").at("version"),true))throw std::runtime_error("Installation downgrade refused.");
}
struct ServiceUpdaterTransport {
  std::function<json()> inspect;
  std::function<json()> activeTorrents;
  std::function<void(const std::string&)> pause,resume;
  std::function<void()> saveSession;
  std::function<void(const std::string&,const json&)> retire;
  std::function<bool(const std::string&,int)> exited;
  std::function<void(const std::string&,const fs::path&)> load;
  std::function<void()> gate,publish,scan;
  std::function<void(const json&)> persist;
  std::function<void(const std::string&)> notify;
  std::function<void()> tick;
};
class ServiceUpdater {
  json journal_;
  InstallationRelease release_;
  ServiceUpdaterTransport io_;
  fs::path managerRoot_,engineRoot_;
  void tick() {if(io_.tick)io_.tick();else std::this_thread::sleep_for(std::chrono::milliseconds(250));}
  void save(const std::string& status) {journal_["status"]=status;io_.persist(journal_);}
  void intent(const std::string& operation) {
    if(journal_.at("attempts").contains(operation))throw std::runtime_error("A service command was already attempted. Outcome requires inspection; automatic repetition refused.");
    journal_["attempts"][operation]="uncertain";io_.persist(journal_);
  }
  void confirm(const std::string& operation) {journal_["attempts"][operation]="confirmed";io_.persist(journal_);}
  void waitExit(const std::string& service,int pid) {
    for(int i=0;i<120;++i){if(io_.exited(service,pid))return;tick();}
    throw std::runtime_error("Service exit was not confirmed; no replacement was launched.");
  }
  void retire(const std::string& service,const json& identity) {
    intent("retire-"+service);io_.retire(service,identity);confirm("retire-"+service);
    waitExit(service,identity.at("pid").get<int>());journal_["exited"][service]=true;io_.persist(journal_);
  }
  void load(const std::string& service,const fs::path& path) {intent("load-"+service);io_.load(service,path);journal_["attempts"]["load-"+service]="submitted";io_.persist(journal_);}
public:
  ServiceUpdater(json journal,InstallationRelease release,ServiceUpdaterTransport transport,fs::path managerRoot,fs::path engineRoot)
    :journal_(std::move(journal)),release_(std::move(release)),io_(std::move(transport)),managerRoot_(std::move(managerRoot)),engineRoot_(std::move(engineRoot)){}
  void run() {
    try {
      if(journal_.at("schema")!=1||journal_.at("status")!="prepared"||!std::regex_match(journal_.at("transaction").get<std::string>(),std::regex("[a-f0-9]{32}"))||journal_.at("bundle")!=release_.record())throw std::runtime_error("Damaged service handoff. Existing files retained.");
      validateInstallationCompatibility(release_.native.manifest,release_.manager,release_.engine);
      verifyServiceTree(managerRoot_,release_.manager,"botty");verifyServiceTree(engineRoot_,release_.engine,"rtorrent");
      io_.gate();const auto before=io_.inspect();validateRunningInstallation(before);
      if(before!=journal_.at("running"))throw std::runtime_error("Running service identities changed before takeover.");
      journal_["attempts"]=json::object();journal_["paused"]=json::array();journal_["exited"]=json::object();
      journal_["helperPid"]=getpid();save("takeover");
      const bool restartEngine=before.at("engine").at("version")!=release_.engine.at("id");
      if(restartEngine) {
        const auto active=io_.activeTorrents();std::set<std::string> seen;
        for(const auto& value:active){const auto hash=value.get<std::string>();if(!std::regex_match(hash,std::regex("[a-fA-F0-9]{40}"))||!seen.insert(hash).second)throw std::runtime_error("Unexpected active torrent identity.");}
        journal_["pauseCandidates"]=active;io_.persist(journal_);
        for(const auto& value:active) {
          const auto hash=value.get<std::string>();intent("pause-"+hash);io_.pause(hash);
          journal_["paused"].push_back(hash);confirm("pause-"+hash);
        }
        intent("session-save");io_.saveSession();confirm("session-save");
      }
      io_.gate();retire("manager",before.at("manager"));retire("worker",before.at("worker"));
      if(restartEngine)retire("engine",before.at("engine"));
      save("starting");
      if(restartEngine)load("engine",engineRoot_/"rtorrent.elf");
      load("worker",managerRoot_/"game-compressor.elf");load("manager",managerRoot_/"botty-manager.elf");
      json running;
      for(int i=0;i<120;++i) {
        try {
          running=io_.inspect();validateRunningInstallation(running);
          if(running.at("manager").at("version")==release_.manager.at("id")&&running.at("worker").at("version")==release_.manager.at("workerVersion")&&running.at("engine").at("version")==release_.engine.at("id")&&running.at("bootId")==before.at("bootId")&&running.at("manager").at("pid")!=before.at("manager").at("pid")&&running.at("worker").at("pid")!=before.at("worker").at("pid")&&(!restartEngine||running.at("engine").at("pid")!=before.at("engine").at("pid")))break;
          running=json::object();
        }catch(...){running=json::object();}
        tick();
      }
      if(running.empty())throw std::runtime_error("Selected running service versions were not confirmed. Native publication refused.");
      confirm("load-manager");confirm("load-worker");if(restartEngine)confirm("load-engine");
      verifyServiceTree(managerRoot_,release_.manager,"botty");verifyServiceTree(engineRoot_,release_.engine,"rtorrent");
      journal_["selectedRunning"]=running;save("services-verified");io_.gate();io_.publish();io_.scan();
      for(const auto& value:journal_.at("paused")){const auto hash=value.get<std::string>();intent("resume-"+hash);io_.resume(hash);confirm("resume-"+hash);}
      if(!journal_.at("paused").empty()){intent("session-save-restored");io_.saveSession();confirm("session-save-restored");}
      save("complete");if(io_.notify)io_.notify("Botty+: Installation updated. You can reopen Botty+.");
    }catch(const std::exception& error) {
      journal_["message"]=std::string(error.what()).substr(0,240);save("recovery-required");
      if(io_.notify)io_.notify("Botty+: Update requires recovery. Retained versions and paused torrent records were preserved. Keep Botty+ closed.");
      throw;
    }
  }
};
}
