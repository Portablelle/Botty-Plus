#pragma once
#include "service-updater.hpp"
#include <csignal>

namespace botty {
class InstallationUpdater {
public:
  using Download=std::function<std::string(const std::string&,size_t,const std::atomic<bool>&)>;
  struct Config {
    fs::path root="/data/botty";
    std::string managerVersion="1.5.4";
    Download download;
    std::function<json()> inspect;
    std::function<bool(const json&)> owns;
    std::function<void()> idle,stopped;
    std::function<void(const json&)> launch;
  };
private:
  NativeTransaction& native_;
  Config cfg_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  std::atomic<bool> cancelled_{false},requested_{false},finished_{false};
  bool check_=true;
  json state_={{"supported",true},{"scope","installation"},{"status","checking"},{"installedVersion",""},{"availableVersion",""},{"installedServiceVersion","1.5.4"},{"availableServiceVersion",""},{"installedWorkerVersion",""},{"availableWorkerVersion",""},{"installedEngineVersion",""},{"availableEngineVersion",""},{"updateAvailable",false},{"requested",false},{"closeRequired",false},{"message","Checking the installation release."}};
  InstallationRelease trusted_;
  json request_;
  fs::path requestPath() const {return cfg_.root/"installation/request.json";}
  fs::path handoffPath() const {return cfg_.root/"installation/handoff.json";}
  NativeUpdater::Fetch fetch() {return [this](const std::string& path,size_t limit){if(cancelled_)throw std::runtime_error("Update download cancelled.");return cfg_.download(path,limit,cancelled_);};}
  void status(const std::string& value,const std::string& message) {
    std::lock_guard<std::mutex> guard(mutex_);state_["status"]=value;state_["message"]=message.substr(0,240);state_["requested"]=requested_.load();state_["closeRequired"]=requested_.load();
    if(value=="error"||value=="blocked")state_["updateAvailable"]=false;
  }
  json ack() const {return {{"apiVersion",1},{"scope","installation"},{"status","queued"},{"version",request_.at("version")},{"serviceVersion",request_.at("serviceVersion")},{"transaction",request_.at("transaction")}};}
  bool releaseFailure(const std::string& message) {
    try {
      if(request_.value("status","")!="queued"||fs::exists(fs::symlink_status(handoffPath()))||native_.pendingJournal()||!native_.canReleaseRequest(request_.at("previous").get<std::string>()))return false;
      const auto running=cfg_.inspect();validateRunningInstallation(running);
      if(running!=request_.at("running"))return false;
      auto failed=request_;failed["status"]="failed";failed["message"]=message.substr(0,240);
      nativeWrite(requestPath(),failed.dump()+"\n");
      {std::lock_guard<std::mutex> guard(mutex_);request_=failed;trusted_={};requested_=false;}
      status("error",message+" Original installation retained. Run a new check to retry.");return true;
    }catch(const std::exception&){return false;}
  }
  void checkRelease() {
    auto release=installationRelease(fetch());
    const auto installed=native_.installedVersion();
    const auto running=cfg_.inspect();installationNoDowngrade(release,running,installed);
    if(running.at("manager").at("version")!=cfg_.managerVersion)throw std::runtime_error("Running manager identity disagrees with this executable.");
    const bool available=installed<release.native.manifest.at("version").get<std::string>()||serviceVersion(running.at("manager").at("version"))<serviceVersion(release.manager.at("id"))||serviceVersion(running.at("worker").at("version"))<serviceVersion(release.manager.at("workerVersion"))||serviceVersion(running.at("engine").at("version"),true)<serviceVersion(release.engine.at("id"),true);
    std::lock_guard<std::mutex> guard(mutex_);
    if(requested_) {
      if(release.hash!=request_.at("hash"))throw std::runtime_error("The queued installation release changed before staging. Existing files retained.");
      return;
    }
    trusted_=std::move(release);state_["installedVersion"]=installed;state_["availableVersion"]=trusted_.native.manifest.at("version");
    state_["installedServiceVersion"]=running.at("manager").at("version");state_["availableServiceVersion"]=trusted_.manager.at("id");
    state_["installedWorkerVersion"]=running.at("worker").at("version");state_["availableWorkerVersion"]=trusted_.manager.at("workerVersion");
    state_["installedEngineVersion"]=running.at("engine").at("version");state_["availableEngineVersion"]=trusted_.engine.at("id");
    state_["updateAvailable"]=available;state_["status"]=available?"available":"current";state_["message"]=available?"An installation update is available.":"All running installation components are current.";
  }
  void progress() {
    if(fs::exists(fs::symlink_status(handoffPath()))) {
      const auto handoff=installationRecord(handoffPath(),256*1024);
      if(handoff.at("schema")!=1)throw std::runtime_error("Unsupported installation handoff schema.");
      if(handoff.at("transaction")!=request_.at("transaction"))throw std::runtime_error("Installation handoff transaction mismatch.");
      if(handoff.at("bundle").at("hash")!=request_.at("hash"))throw std::runtime_error("Installation handoff bundle mismatch.");
      validateInstallationCompatibility(handoff.at("bundle").at("native"),handoff.at("bundle").at("manager"),handoff.at("bundle").at("engine"));
      const auto phase=handoff.at("status").get<std::string>();
      if(phase=="complete") {
        const auto running=cfg_.inspect();
        if(!native_.targetComplete(handoff.at("bundle").at("native"),handoff.at("bundle").at("nativeHash")))throw std::runtime_error("Completed native publication is not verified.");
        if(native_.installedVersion()!=request_.at("version")||running.at("manager").at("version")!=request_.at("serviceVersion")||running.at("worker").at("version")!=request_.at("workerVersion")||running.at("engine").at("version")!=request_.at("engineVersion"))throw std::runtime_error("Updated installation identity confirmation failed.");
        validateRunningInstallation(running);auto complete=request_;complete["status"]="complete";nativeWrite(requestPath(),complete.dump()+"\n");
        {std::lock_guard<std::mutex> guard(mutex_);request_=complete;state_["installedVersion"]=complete.at("version");state_["installedServiceVersion"]=complete.at("serviceVersion");state_["installedWorkerVersion"]=complete.at("workerVersion");state_["installedEngineVersion"]=complete.at("engineVersion");state_["updateAvailable"]=false;requested_=false;}
        status("complete","Installation update complete. You can reopen Botty+.");return;
      }
      if(phase=="recovery-required") {status("blocked",handoff.value("message","Installation recovery is required. Keep Botty+ closed; use Portal recovery."));return;}
      if(phase=="prepared")throw std::runtime_error("Service updater takeover is unconfirmed. Automatic launch retry refused; use Portal recovery.");
      if(phase!="takeover"&&phase!="starting"&&phase!="services-verified")throw std::runtime_error("Unknown service updater phase. Use Portal recovery.");
      if(!cfg_.owns||!cfg_.owns(handoff)) {
        const auto latest=installationRecord(handoffPath(),256*1024);
        if(latest.at("transaction")==request_.at("transaction")&&(latest.value("status","")=="complete"||latest.value("status","")=="recovery-required")){progress();return;}
        throw std::runtime_error("Service updater ownership or current boot is unconfirmed. Use Portal recovery; automatic restart refused.");
      }
      status("installing","The service updater owns this transaction. Keep Botty+ closed.");return;
    }
    InstallationRelease release;{std::lock_guard<std::mutex> guard(mutex_);release=trusted_;}
    if(release.hash.empty()&&request_.at("status")=="queued") {
      release=installationRelease(fetch());
      if(release.hash!=request_.at("hash"))throw std::runtime_error("Queued installation release changed. Existing files retained.");
      std::lock_guard<std::mutex> guard(mutex_);trusted_=release;
    }
    if(release.hash.empty()||release.hash!=request_.at("hash"))throw std::runtime_error("Queued update has no matching trusted bundle. Use Portal recovery.");
    if(release.native.manifest.at("version")!=request_.at("version")||release.manager.at("id")!=request_.at("serviceVersion")||release.manager.at("workerVersion")!=request_.at("workerVersion")||release.engine.at("id")!=request_.at("engineVersion"))throw std::runtime_error("Queued installation target versions disagree with the trusted bundle.");
    try{cfg_.idle();cfg_.stopped();}catch(const std::logic_error&){throw;}catch(const std::exception& error){status("waiting",error.what());return;}
    const auto running=cfg_.inspect();installationNoDowngrade(release,running,native_.installedVersion());
    native_.preflight(release.native.manifest);status("installing","Staging and verifying the complete installation. Keep Botty+ closed.");
    native_.stage(release.native.manifest,release.native.hash,[&](const std::string& path,size_t size){return fetch()("botty-native/"+path,size);});
    stageService(cfg_.root/"manager"/release.manager.at("id").get<std::string>(),release.manager,"botty",fetch());
    stageService(cfg_.root/"rtorrent"/release.engine.at("id").get<std::string>(),release.engine,"rtorrent",fetch());
    if(installationRelease(fetch()).hash!=release.hash)throw std::runtime_error("The installation release changed during download. Existing files retained.");
    cfg_.idle();cfg_.stopped();if(cfg_.inspect()!=running)throw std::runtime_error("Running service identities changed before handoff.");
    json handoff={{"schema",1},{"status","prepared"},{"transaction",request_.at("transaction")},{"capability",nativeHash(request_.at("transaction").get<std::string>()+nativeRead(cfg_.root/"installation/secret",128))},{"bundle",release.record()},{"running",running}};
    nativeWrite(handoffPath(),handoff.dump()+"\n",true);
    auto owned=request_;owned["status"]="handoff";nativeWrite(requestPath(),owned.dump()+"\n");{std::lock_guard<std::mutex> guard(mutex_);request_=owned;}
    cfg_.launch(handoff);
    for(int i=0;i<120&&!cancelled_;++i) {
      const auto taken=installationRecord(handoffPath(),256*1024);
      if(taken.at("transaction")==request_.at("transaction")&&taken.value("status","")!="prepared") {
        if(taken.value("status","")!="takeover"||!cfg_.owns||!cfg_.owns(taken))throw std::runtime_error("Durable updater takeover ownership or boot is unconfirmed. Automatic launch retry refused.");
        finished_=true;return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw std::runtime_error("Service updater takeover was not confirmed. Launch outcome is unknown; do not retry automatically.");
  }
  void run() {
    auto next=std::chrono::steady_clock::time_point{};
    while(!cancelled_&&!finished_) {
      bool checking=false;
      {std::unique_lock<std::mutex> guard(mutex_);wake_.wait_for(guard,std::chrono::seconds(2),[&]{return cancelled_||check_;});checking=check_||std::chrono::steady_clock::now()>=next;check_=false;}
      if(cancelled_)break;
      try {
        if(requested_)progress();else if(checking){next=std::chrono::steady_clock::now()+std::chrono::hours(6);checkRelease();}
      }catch(const std::exception& error) {
        if(requested_){if(releaseFailure(error.what()))continue;status("blocked",error.what());break;}
        else status("error",error.what());
      }
    }
    finished_=true;
  }
public:
  InstallationUpdater(NativeTransaction& native,Config config):native_(native),cfg_(std::move(config)) {state_["installedServiceVersion"]=cfg_.managerVersion;}
  ~InstallationUpdater(){cancelled_=true;wake_.notify_all();if(thread_.joinable())thread_.join();}
  void start() {
    try {
      const auto legacy=cfg_.root/"native/request.json";
      if(fs::exists(fs::symlink_status(legacy))) {
        const auto previous=installationRecord(legacy,65536);
        if(previous.value("schema",0)!=1||previous.value("status","")=="queued"||(previous.value("status","")!="complete"&&previous.value("status","")!="failed"))throw std::runtime_error("A legacy native update request needs Portal recovery before starting file work.");
      }
      if(fs::exists(fs::symlink_status(requestPath()))) {
        request_=installationRecord(requestPath(),65536);
        if(request_.at("schema")!=1||!std::regex_match(request_.at("transaction").get<std::string>(),std::regex("[a-f0-9]{32}"))||!std::regex_match(request_.at("hash").get<std::string>(),std::regex("[a-f0-9]{64}")))throw std::runtime_error("Damaged installation request.");
        if(!std::regex_match(request_.at("version").get<std::string>(),std::regex("[0-9]{2}\\.[0-9]{3}\\.[0-9]{3}")))throw std::runtime_error("Damaged installation target version.");
        serviceVersion(request_.at("serviceVersion"));serviceVersion(request_.at("workerVersion"));serviceVersion(request_.at("engineVersion"),true);
        const auto phase=request_.at("status").get<std::string>();
        if(phase!="queued"&&phase!="handoff"&&phase!="complete"&&phase!="failed")throw std::runtime_error("Unknown installation request phase.");
        if(phase=="queued"||phase=="failed") {
          if(!std::regex_match(request_.at("previous").get<std::string>(),std::regex("[a-f0-9]{64}")))throw std::runtime_error("Damaged original installation fingerprint.");
          validateRunningInstallation(request_.at("running"));
        }
        requested_=phase=="queued"||phase=="handoff";
        if(phase=="failed"&&fs::exists(fs::symlink_status(handoffPath())))throw std::runtime_error("A failed request has a retained service handoff. Use Portal recovery.");
        state_["transaction"]=request_.at("transaction");
        state_["availableVersion"]=request_.at("version");state_["availableServiceVersion"]=request_.at("serviceVersion");state_["availableWorkerVersion"]=request_.at("workerVersion");state_["availableEngineVersion"]=request_.at("engineVersion");
      }
      if(native_.pendingJournal()&&!requested_)throw std::runtime_error("Pending native publication requires Portal recovery.");
    }catch(const std::exception& error){requested_=true;finished_=true;status("blocked",error.what());}
    thread_=std::thread([this]{run();});
  }
  bool requested() const {return requested_;}
  bool finished() const {return finished_;}
  void requireAdmission() const {if(requested_)throw std::runtime_error("Installation update queued. Wait before starting new file work.");}
  json state() const {
    json result;{std::lock_guard<std::mutex> guard(mutex_);result=state_;}
    if(finished_&&requested_)try {
      if(fs::exists(fs::symlink_status(handoffPath()))) {
        const auto handoff=installationRecord(handoffPath(),256*1024);
        if(handoff.value("status","")=="recovery-required"){result["status"]="blocked";result["message"]=handoff.value("message","Installation recovery is required. Keep Botty+ closed.");}
        else if(handoff.value("status","")!="complete"&&(!cfg_.owns||!cfg_.owns(handoff))){result["status"]="blocked";result["message"]="Service updater ownership or current boot is unconfirmed. Use Portal recovery; automatic restart refused.";}
      }
    }catch(const std::exception&){result["status"]="blocked";result["message"]="The durable updater handoff needs inspection. Keep Botty+ closed.";}
    return result;
  }
  void recheck(){std::lock_guard<std::mutex> guard(mutex_);check_=true;wake_.notify_all();}
  json request() {
    std::unique_lock<std::mutex> guard(mutex_);
    if(requested_){if(request_.empty()||state_.at("status")=="blocked")throw std::runtime_error("Inspect installation recovery before retrying.");return ack();}
    if(state_.at("status")!="available"||!state_.at("updateAvailable").get<bool>()||trusted_.hash.empty())throw std::runtime_error("Run a trusted complete installation check before updating.");
    const auto release=trusted_;
    guard.unlock();const auto running=cfg_.inspect();installationNoDowngrade(release,running,native_.installedVersion());native_.preflight(release.native.manifest);const auto previous=native_.installedFingerprint();guard.lock();
    if(requested_)return ack();
    if(trusted_.hash!=release.hash||state_.at("status")!="available")throw std::runtime_error("Installation check changed; run a fresh check.");
    if(fs::exists(fs::symlink_status(handoffPath()))) {
      const auto previous=installationRecord(handoffPath(),256*1024);
      if(previous.at("status")!="complete"||request_.value("status","")!="complete"||previous.at("transaction")!=request_.at("transaction"))throw std::runtime_error("Previous updater handoff needs Portal recovery before another transaction.");
      nativeWrite(cfg_.root/"installation/history"/(previous.at("transaction").get<std::string>()+".json"),previous.dump()+"\n",true);
      if(::unlink(handoffPath().c_str()))throw std::runtime_error("Cannot archive the completed updater handoff.");
    }
    if(!fs::exists(fs::symlink_status(cfg_.root/"installation/secret")))nativeWrite(cfg_.root/"installation/secret",nativeHash(nativeTransactionId()),true);
    nativeRead(cfg_.root/"installation/secret",128);
    if(::chmod((cfg_.root/"installation").c_str(),0700))throw std::runtime_error("Cannot protect the installation transaction directory.");
    request_={{"schema",1},{"status","queued"},{"transaction",nativeTransactionId()},{"hash",trusted_.hash},{"version",trusted_.native.manifest.at("version")},{"serviceVersion",trusted_.manager.at("id")},{"workerVersion",trusted_.manager.at("workerVersion")},{"engineVersion",trusted_.engine.at("id")},{"previous",previous},{"running",running}};
    nativeWrite(requestPath(),request_.dump()+"\n");requested_=true;state_["transaction"]=request_.at("transaction");state_["status"]="waiting";state_["requested"]=true;state_["closeRequired"]=true;state_["message"]="Update queued. Existing file operations must finish, then close Botty+.";wake_.notify_all();return ack();
  }
};
}
