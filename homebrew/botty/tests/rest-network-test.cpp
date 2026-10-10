#include "rest-network.hpp"
#include <cassert>
#include <cstdio>
#include <functional>
struct FakeServer {
  int serves=0,binds=0,stops=0,error=163;
  std::function<bool(int)> handler;
  void set_listen_error_handler(std::function<bool(int)> value){handler=std::move(value);}
  bool listen_after_bind(){++serves;return handler(error); }
  int last_listen_error(){return error;}
  bool stopRequested=false;
  bool listen_stop_requested(){return stopRequested;}
  void stop(){++stops;stopRequested=true;}
  bool rebind_after_error(const char*,int){return ++binds>=3;}
};
int main(){
  FakeServer server;int waits=0,reports=0;
  assert(botty::serveWithRestRecovery(server,"127.0.0.1",8088,[]{return false;},[&](int delay){assert(delay==++waits);return true;},[&](int error){assert(error==163);++reports;}));
  assert(server.serves==1&&server.binds==3&&reports==1);
  FakeServer cancelled;
  assert(botty::serveWithRestRecovery(cancelled,"127.0.0.1",8088,[]{return false;},[](int){return false;},[](int){}));
  assert(cancelled.binds==0&&cancelled.serves==1);
  FakeServer explicitStop;
  assert(botty::serveWithRestRecovery(explicitStop,"127.0.0.1",8088,[]{return false;},[&](int){explicitStop.stop();return true;},[](int){}));
  assert(explicitStop.binds==0);
  FakeServer fatal;fatal.error=EBADF;
  assert(!botty::serveWithRestRecovery(fatal,"127.0.0.1",8088,[]{return false;},[](int){assert(false);return true;},[](int){assert(false);}));
  assert(fatal.binds==0);
  FakeServer stopped;
  assert(!botty::serveWithRestRecovery(stopped,"127.0.0.1",8088,[]{return true;},[](int){assert(false);return true;},[](int){assert(false);}));
  assert(stopped.binds==0);
  puts("HTTP rest-network recovery, backoff, cancellation and fatal errors passed");
}
