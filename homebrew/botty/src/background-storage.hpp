#pragma once
#include "core.hpp"
#include "httplib.h"
#include <thread>
#include <regex>
namespace botty {
// Only use the scoped ShadowMount worker when it advertises the Botty guard.
class BackgroundRejected : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class BackgroundStorage {
  int port_;
public:
  using Report=std::function<void(const json&)>;
  explicit BackgroundStorage(int port):port_(port){}
  json api(const std::string& route,const json& body) const {
    httplib::Client client("127.0.0.1",port_);client.set_connection_timeout(1);client.set_read_timeout(5);client.set_write_timeout(5);
    auto r=client.Post("/api/v1/"+route,body.dump(),"application/json");
    if(!r)throw std::runtime_error("ShadowMount response lost. Check Processing before retrying; files may still be moving.");
    const auto data=json::parse(r->body);
    if((r->status!=200&&r->status!=202)||data.value("status",-1)!=0)throw BackgroundRejected(data.value("error",std::string("ShadowMount rejected the operation")));
    return data;
  }
  bool supported() const {
    const auto version=api("version",json::object());
    for(const auto& value:version.value("capabilities",json::array()))if(value=="botty_background_storage_v1")return true;
    return false;
  }
  json run(const std::string& operation,const std::string& title,const fs::path& source,const fs::path& destination,const Report& report,const std::function<void()>& check={}) const {
    if(title=="PPSA99071"||!std::regex_match(title,std::regex("(PPSA|CUSA)[0-9]{5}"))||(operation!="move"&&operation!="delete"))throw std::runtime_error("Invalid background Library operation");
    if(check)check();
    json body={{"title_id",title},{"expected_source",source.string()},{"confirm",true}};
    if(operation=="move")body["destination_dir"]=destination.parent_path().string();
    auto task=api("games/"+operation,body);const auto id=task.at("job_id").get<uint64_t>();
    if(!id)throw std::runtime_error("ShadowMount returned no operation identity");
    for(;;){
      if(task.value("job_id",uint64_t(0))!=id||task.value("title_id","")!=title||task.value("source","")!=source.string()||task.value("operation","")!=operation||(operation=="move"&&task.value("destination","")!=destination.string()))throw std::runtime_error("ShadowMount operation changed; inspect retained files before retrying");
      if(report)report(task);
      const auto state=task.value("state","");
      if(state=="completed") {
        if(task.value("result_status",0)!=0||fs::exists(fs::symlink_status(source))||
           (operation=="move"&&!fs::exists(fs::symlink_status(destination))))
          throw std::runtime_error("ShadowMount completion does not match the files on disk; inspect retained copies before retrying");
        return task;
      }
      if(state=="failed"||state=="cancelled")throw std::runtime_error("Library operation stopped: "+task.value("result_error",std::string("inspect retained files before retrying")));
      if(state!="preparing"&&state!="measuring"&&state!="transferring"&&state!="deleting"&&state!="finalizing")throw std::runtime_error("Unknown Library operation state; check ShadowMount before retrying");
      if(check)try{check();}catch(...){
        if(task.value("cancellable",false))try{api("games/storage/cancel",{{"job_id",id}});}catch(...){}
        throw;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      // Once accepted, always observe that exact job; never replay its POST.
      try{task=api("games/storage/status",{{"job_id",id}});}
      catch(const std::exception& e){throw std::runtime_error(std::string("Lost status for ShadowMount job ")+std::to_string(id)+". "+e.what());}
    }
  }
};
inline json backgroundProgress(const json& task) {
  const auto phase=task.value("state","");const bool deleting=phase=="deleting";
  const bool measured=phase=="transferring"||deleting||phase=="completed";
  const auto total=measured?task.value(deleting?"total_files":"total_bytes",uint64_t(0)):uint64_t(0);
  const auto done=std::min(total,task.value(deleting?"processed_files":"processed_bytes",uint64_t(0)));
  const double rate=phase=="transferring"?task.value("speed_bytes_per_second",0.0):0.0;
  return {{"remoteJobId",task.at("job_id")},{"bytes",done},{"total",total},{"unit",deleting?"items":"bytes"},{"rate",rate},{"eta",rate>0&&total>done?(total-done)/rate:-1.0},{"elapsed",task.value("elapsed_ms",0.0)/1000.0},{"phase",phase=="preparing"?"Preparing Library operation":phase=="measuring"?"Checking files":phase=="transferring"?"Moving game to selected disk":deleting?"Deleting game files":phase=="finalizing"?"Finishing move and removing source":phase=="completed"?"Library operation completed":"Library operation stopped"}};
}
}
