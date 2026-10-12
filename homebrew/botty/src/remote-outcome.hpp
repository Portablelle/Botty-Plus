#pragma once
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>

namespace botty {
struct RemoteTransferError:std::runtime_error {
  long status;
  RemoteTransferError(long code,const std::string& message):std::runtime_error(message),status(code){}
};
// One classification and reduction define the policy for workers/coordinator/
// lifecycle: fatal range rejection > temporary contention > other failures.
class RemoteBatchOutcome {
public:
  enum class Kind {success=0,other=1,busy=2,invalidRange=3};
private:
  Kind kind_=Kind::success;
  std::exception_ptr error_;
public:
  static RemoteBatchOutcome failure(std::exception_ptr error){
    RemoteBatchOutcome result;if(!error)return result;result.kind_=Kind::other;result.error_=error;
    try{std::rethrow_exception(error);}catch(const RemoteTransferError& e){if(e.status==416)result.kind_=Kind::invalidRange;else if(e.status==429)result.kind_=Kind::busy;}catch(...){}
    return result;
  }
  bool succeeded() const noexcept{return kind_==Kind::success;}
  bool cancelSiblings() const noexcept{return kind_==Kind::other||kind_==Kind::invalidRange;}
  bool pause() const noexcept{return kind_==Kind::invalidRange;}
  bool busy() const noexcept{return kind_==Kind::busy;}
  int connectionsAfterFailure(int current) const noexcept{return busy()?std::max(1,current/2):current;}
  void include(const RemoteBatchOutcome& other){if(static_cast<int>(other.kind_)>static_cast<int>(kind_)){kind_=other.kind_;error_=other.error_;}}
  void rethrow() const {if(error_)std::rethrow_exception(error_);}
};
}
