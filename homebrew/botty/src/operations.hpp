#pragma once
#include "core.hpp"
#include "progress.hpp"
#include <mutex>
namespace botty {
// Separate from the catalog lock: a destructive request can be observed while it runs.
class Operations {
  mutable std::mutex mutex_;
  json task_={{"status","idle"}};
  ExtractionEstimate estimate_;
  std::chrono::steady_clock::time_point started_{},updated_{};
public:
  void start(const std::string& id,const std::string& name) {
    std::lock_guard<std::mutex> g(mutex_);started_=std::chrono::steady_clock::now();estimate_=ExtractionEstimate{};updated_=started_;
    task_={{"id",id},{"name",name},{"kind","deletion"},{"status","running"},{"phase","Checking files before deletion"},{"unit","items"},{"bytes",0},{"total",0},{"rate",0},{"eta",-1}};
  }
  void update(uint64_t done,uint64_t total,const std::string& file) {
    std::lock_guard<std::mutex> g(mutex_);const auto now=std::chrono::steady_clock::now();
    if(task_.value("total",uint64_t(0))!=total)estimate_=ExtractionEstimate{};
    updated_=now;estimate_.update(now,done,total);task_["bytes"]=done;task_["total"]=total;task_["file"]=file;task_["phase"]="Deleting files and folders";task_["rate"]=estimate_.rate;task_["eta"]=estimate_.eta;
  }
  void finish(bool success) {std::lock_guard<std::mutex> g(mutex_);task_["elapsed"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count();task_["status"]=success?"completed":"failed";task_["phase"]=success?"Deletion completed":"Deletion failed; check the request result before retrying";task_["eta"]=-1;task_["rate"]=0;}
  json state() const {std::lock_guard<std::mutex> g(mutex_);auto out=task_;if(out.value("status","")=="running")out["elapsed"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count();if(std::chrono::steady_clock::now()-updated_>std::chrono::seconds(5)){out["rate"]=0;out["eta"]=-1;}return out;}
};
class DeletionScope {
  Operations& operations_;bool completed_=false;
public:
  DeletionScope(Operations& operations,const std::string& id,const std::string& name):operations_(operations){operations_.start(id,name);}
  ~DeletionScope(){operations_.finish(completed_);}
  DeleteProgress reporter(){return [this](uint64_t done,uint64_t total,const std::string& file){operations_.update(done,total,file);};}
  void complete(){completed_=true;}
};
}
