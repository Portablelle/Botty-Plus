#pragma once
#include "background-storage.hpp"
#include "storage.hpp"
#include <optional>
namespace botty {
// Shared identity, paths and settlement rules for live and recovered Library moves.
class LibraryTransfer {
public:
  struct Locations {fs::path source,destination;};
  struct Settlement {json job,transfer;bool rejected;};
  static Locations locations(const json& job,const Paths& source,const Paths& destination,const std::string& sourceId,const std::string& selected,bool recovering=false) {
    if(job.value("status","")!="moved"||job.value("platform","")=="switch"||sourceId==selected)throw std::runtime_error("Invalid Library move");
    const auto name=safeRelative(job.at("content").at("destination").get<std::string>());
    if(name.has_parent_path())throw std::runtime_error("Invalid tracked transfer path");
    Locations result{source.library/name,destination.library/name};
    const auto recorded=job.value("destination","");
    if(!((job.value("storage","internal")==sourceId&&recorded==result.source.string())||
         (recovering&&job.value("storage","internal")==selected&&recorded==result.destination.string())))
      throw std::runtime_error("Tracked library location changed");
    return result;
  }
  static std::string title(const json& job) {
    const auto id=job.at("content").value("titleId","");
    if(id=="PPSA99071"||!std::regex_match(id,std::regex("(PPSA|CUSA)[0-9]{5}")))throw std::runtime_error("ShadowMount must identify this game before transfer");
    return id;
  }
  static json completeJob(json job,const std::string& selected,const fs::path& destination) {
    job["storage"]=selected;job["destination"]=destination.string();job["phase"]="Ready in Library";job["error"]="";
    return job;
  }
  // Read-only reconciliation also supports legacy journals, deriving paths from
  // the tracked entry and volume IDs rather than arbitrary journal path strings.
  static std::optional<Settlement> reconcile(const json& pending,json job,Storage& storage,int port) {
    if(pending.value("kind","")!="job"||job.value("status","")!="moved")return {};
    const auto sourceId=pending.at("sourceStorage").get<std::string>(),selected=pending.at("storage").get<std::string>();
    const auto source=storage.get(sourceId,false),destination=storage.get(selected,false);
    const auto location=locations(job,source,destination,sourceId,selected,true);
    containedExisting(source.library.parent_path(),source.library);containedExisting(destination.library.parent_path(),destination.library);
    if(!fs::is_directory(source.library)||!fs::is_directory(destination.library))return {};
    const auto id=title(job);
    BackgroundStorage background(port);
    if(!background.supported())return {};
    const auto task=background.api("games/storage/status",json::object());
    if(task.value("active",true)||task.value("queue_running",true)||task.value("queue_count",1)!=0)return {};
    const auto state=task.value("state","");
    if(state!="idle"&&state!="completed"&&state!="failed"&&state!="cancelled")return {};
    bool rejected=false;
    if(task.value("job_id",uint64_t(0))==pending.value("remoteJobId",uint64_t(0))&&task.value("job_id",uint64_t(0))!=0&&
       task.value("operation","")=="move"&&task.value("title_id","")==id&&task.value("source","")==location.source.string()&&task.value("destination","")==location.destination.string())
      rejected=BackgroundStorage::rejectedBeforeWork(task,location.source,location.destination);
    if(!rejected){
      if(fs::exists(fs::symlink_status(location.source)))return {};
      containedExisting(destination.library,location.destination);
      const auto info=background.api("games/info",{{"title_id",id}});
      if(info.value("title_id","")!=id||info.value("path","")!=location.destination.string()||!info.value("source_available",false))return {};
      job=completeJob(std::move(job),selected,location.destination);
    }
    auto settled=pending;settled["status"]=rejected?"failed":"complete";settled["error"]=rejected?task.value("result_error",std::string("ShadowMount was busy; retry the move")):"";
    settled["phase"]=rejected?"Move not started; source kept":"Move completed. Ready in Library.";
    settled["rate"]=0;settled["eta"]=-1;
    return Settlement{std::move(job),std::move(settled),rejected};
  }
};
}
