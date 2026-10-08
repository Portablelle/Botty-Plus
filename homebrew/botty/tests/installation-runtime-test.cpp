#include "installation-runtime.hpp"
#include <cassert>
#include <future>
#include <iostream>

using namespace botty;
template<class F> void fails(F fn,int expected){try{fn();assert(false);}catch(const std::system_error& error){assert(error.code().value()==expected);}}
int main() {
  const auto deadline=[] {return std::chrono::steady_clock::now()+std::chrono::seconds(1);};
  int waits=0;
  fails([&]{installationAwaitSocket(0,POLLOUT,deadline(),[&](pollfd*,nfds_t,int timeout){++waits;assert(timeout>0&&timeout<=1000);return 0;});},ETIMEDOUT);assert(waits==1);
  fails([&]{installationAwaitSocket(0,POLLOUT,std::chrono::steady_clock::now(),[&](pollfd*,nfds_t,int){assert(false);return 0;});},ETIMEDOUT);
  installationAwaitSocket(0,POLLOUT,deadline(),[](pollfd* state,nfds_t,int){state->revents=POLLOUT;return 1;});
  InstallationSocket listener{::socket(AF_INET,SOCK_STREAM,0)};assert(listener.fd>=0);
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!::bind(listener.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)));assert(!::listen(listener.fd,1));
  socklen_t size=sizeof(address);assert(!::getsockname(listener.fd,reinterpret_cast<sockaddr*>(&address),&size));const auto port=ntohs(address.sin_port);
  assert(!installationPortClosed(port));InstallationSocket accepted{::accept(listener.fd,nullptr,nullptr)};assert(accepted.fd>=0);
  InstallationSocket client{installationConnectLoopback(port,deadline())};InstallationSocket peer{::accept(listener.fd,nullptr,nullptr)};assert(peer.fd>=0);
  installationSend(client.fd,"fixture",deadline());char buffer[8]{};assert(::recv(peer.fd,buffer,sizeof(buffer),0)==7);assert(std::string(buffer,7)=="fixture");
  ::close(listener.fd);listener.fd=-1;assert(installationPortClosed(port));fails([&]{InstallationSocket refused{installationConnectLoopback(port,deadline())};},ECONNREFUSED);
  int pair[2];assert(!::socketpair(AF_UNIX,SOCK_STREAM,0,pair));InstallationSocket sender{pair[0]},receiver{pair[1]};assert(!::fcntl(sender.fd,F_SETFL,O_NONBLOCK));
  fails([&]{installationSend(sender.fd,std::string(8*1024*1024,'x'),std::chrono::steady_clock::now()+std::chrono::milliseconds(20));},ETIMEDOUT);
  ::close(receiver.fd);receiver.fd=-1;fails([&]{installationSend(sender.fd,"closed",deadline());},EPIPE);
  std::atomic<bool> ready{false},stopping{false},inspected{false};auto startup=std::async(std::launch::async,[&]{installationAwaitHttpReady(ready,stopping);inspected=true;});
  assert(startup.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);assert(!inspected);ready=true;startup.get();assert(inspected);
  ready=false;stopping=true;bool cancelled=false;try{installationAwaitHttpReady(ready,stopping);}catch(const std::runtime_error&){cancelled=true;}assert(cancelled);
  const auto root=fs::temp_directory_path()/("botty-retire-"+nativeTransactionId());fs::create_directory(root);::chmod(root.c_str(),0700);
  const std::string transaction(32,'a'),capability(64,'b');json handoff={{"transaction",transaction},{"capability",capability},{"status","takeover"},{"running",{{"manager",{{"pid",getpid()}}}}}};
  nativeWrite(root/"installation/handoff.json",handoff.dump());int checks=0;const auto owns=[&](const json&){++checks;return true;};
  assert(installationRetireAuthorized(root,transaction,capability,owns));assert(checks==1);
  handoff["running"]["manager"]["pid"]=getpid()+1;nativeWrite(root/"installation/handoff.json",handoff.dump());assert(!installationRetireAuthorized(root,transaction,capability,owns));assert(checks==1);
  handoff["running"]["manager"]["pid"]=getpid();nativeWrite(root/"installation/handoff.json",handoff.dump());assert(!installationRetireAuthorized(root,transaction,capability,[](const json&){return false;}));
  fs::remove_all(root);std::cout<<"Installation socket deadlines, refusal/success, SIGPIPE, HTTP readiness and retirement PID tests passed\n";
}
