#include <sys/socket.h>
#include <atomic>
#include <cerrno>
#include <thread>
#include <cassert>
#include <cstdio>
static std::atomic<bool> interruptAccept{false};
static int botty_test_accept(int fd,sockaddr* address,socklen_t* length) {
  if(interruptAccept.exchange(false)){errno=163;return -1;}
  return ::accept(fd,address,length);
}
// Exercise the PS5 accept path with one injected suspend error on a real socket.
#define __PS5__
#define accept botty_test_accept
#include "httplib.h"
#undef accept
#undef __PS5__
#include "rest-network.hpp"
int main(){
  httplib::Server server;std::atomic<bool> stopping{false};std::atomic<int> recoveries{0};
  std::atomic<bool> busy{false},unblock{false};
  server.Get("/blocked",[&](const auto&,auto& res){busy=true;while(!unblock)std::this_thread::sleep_for(std::chrono::milliseconds(1));res.set_content("finished","text/plain");});
  server.Get("/health",[](const auto&,auto& res){res.set_content("same manager","text/plain");});
  // httplib defaults to SO_REUSEPORT on Linux, which cannot collide with a
  // SO_REUSEADDR socket. Use SO_REUSEADDR on both so the occupied port makes the
  // first rebind fail with EADDRINUSE on Linux and BSD alike.
  server.set_socket_options([](socket_t sock){int reuse=1;setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));});
  int port=server.bind_to_any_port("127.0.0.1");assert(port>0);
  int occupied=-1;std::atomic<int> waits{0};
  std::thread worker([&]{assert(botty::serveWithRestRecovery(server,"127.0.0.1",port,[&]{return stopping.load();},[&](int){
    // The second wait only happens after the first rebind failed on the occupied port.
    if(++waits==2){::close(occupied);occupied=-1;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));return true;
  },[&](int error){
    assert(error==163);++recoveries;
    occupied=::socket(AF_INET,SOCK_STREAM,0);assert(occupied>=0);int reuse=1;
    assert(!setsockopt(occupied,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!::bind(occupied,reinterpret_cast<sockaddr*>(&address),sizeof(address)));assert(!::listen(occupied,1));
  }));});
  server.wait_until_ready();
  httplib::Client client("127.0.0.1",port);client.set_connection_timeout(1);client.set_read_timeout(1);
  auto before=client.Get("/health");assert(before&&before->status==200);
  std::thread blockedRequest([&]{httplib::Client connection("127.0.0.1",port);connection.set_read_timeout(10);(void)connection.Get("/blocked");});
  const auto busyDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!busy&&std::chrono::steady_clock::now()<busyDeadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(busy);
  interruptAccept=true;(void)client.Get("/health");
  bool ready=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(std::chrono::steady_clock::now()<deadline){
    auto response=client.Get("/health");if(response&&response->status==200){ready=true;assert(response->body=="same manager");break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(ready&&recoveries==1&&waits>=2&&!unblock);unblock=true;blockedRequest.join();stopping=true;server.stop();worker.join();
  // Stop while a fatal accept error has released the listener but an HTTP
  // worker still keeps is_running_ true during drain.
  httplib::Server draining;std::atomic<bool> entered{false},release{false};
  draining.Get("/block",[&](const auto&,auto& res){entered=true;while(!release)std::this_thread::sleep_for(std::chrono::milliseconds(1));res.set_content("done","text/plain");});
  int drainPort=draining.bind_to_any_port("127.0.0.1");assert(drainPort>0);
  std::thread listener([&]{draining.listen_after_bind();});draining.wait_until_ready();
  std::thread request([&]{httplib::Client connection("127.0.0.1",drainPort);connection.set_read_timeout(5);(void)connection.Get("/block");});
  const auto drainDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!entered&&std::chrono::steady_clock::now()<drainDeadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(entered);
  interruptAccept=true;httplib::Client trigger("127.0.0.1",drainPort);trigger.set_read_timeout(1);(void)trigger.Get("/block");
  while(draining.last_listen_error()!=163&&std::chrono::steady_clock::now()<drainDeadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(draining.last_listen_error()==163&&draining.is_running()&&!draining.is_accepting());draining.stop();release=true;request.join();listener.join();
  puts("HTTP survives a PS5 suspend accept error and serves again without manager restart");
}
