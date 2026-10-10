#pragma once
#include <cerrno>
namespace botty {
inline bool restNetworkInterrupted(int error) {
  // 163 is the PS5 network-suspend error observed in FW 13.00 runtime logs.
  return error==163||error==ENETDOWN||error==ENETRESET||error==ENETUNREACH||error==EHOSTUNREACH||error==ECONNABORTED;
}
// Caller owns one server and its singleton lock. Rebind the listener only;
// never repeat an HTTP command, relaunch a payload or restart a background job.
template<class Server,class Stop,class Wait,class Report>
bool serveWithRestRecovery(Server& server,const char* host,int port,Stop stopping,Wait wait,Report report) {
  bool cancelled=false;
  server.set_listen_error_handler([&](int error){
    if(stopping()||server.listen_stop_requested()||!restNetworkInterrupted(error))return false;
    report(error);int delay=1;
    for(;;){
      if(stopping()||server.listen_stop_requested()||!wait(delay)){cancelled=true;return false;}
      if(stopping()||server.listen_stop_requested()){cancelled=true;return false;}
      if(server.rebind_after_error(host,port))return true;
      delay=delay<10?delay+1:10;
    }
  });
  // The callback runs inside the accept loop, while existing request workers
  // keep their ownership and may finish later. Clear its stack captures on exit.
  const bool served=server.listen_after_bind();
  server.set_listen_error_handler({});
  return served||cancelled;
}
}
