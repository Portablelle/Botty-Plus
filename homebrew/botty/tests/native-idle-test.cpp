#include "httplib.h"
#include "compressor.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
using namespace botty;
template<class F> void rejects(F fn){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}assert(failed);}
int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const auto root=fs::temp_directory_path()/("botty-native-idle-"+randomId());Paths paths(root,root/"library");fs::create_directories(root/"compressor");
  Compressor absent;absent.init(paths);rejects([&]{absent.confirmNativeUpdateIdle();});
  writeJson(root/"compressor/enabled.json",{{"mode","library-1.2"}});std::ofstream(root/"compressor/token")<<std::string(64,'a');
  httplib::Server server;std::atomic<int> mode{0},posts{0};
  server.Post("/api/gc/job/cancel",[&](const auto&,auto&){++posts;});
  server.Get("/api/status",[&](const auto&,auto& res){res.set_content(json({{"ok",true},{"bottyWorker",mode==5?"unknown":"library-1.3"}}).dump(),"application/json");});
  server.Get("/api/gc/job",[&](const auto&,auto& res){json data={{"ok",true}};if(mode==0)data["busy"]=false;else if(mode==2)data["busy"]="false";else if(mode==3)data["busy"]=true;else if(mode==4){res.status=503;data["ok"]=false;}res.set_content(data.dump(),"application/json");});
  const int port=server.bind_to_any_port("127.0.0.1");std::thread thread([&]{server.listen_after_bind();});
  Compressor idle;idle.init(paths,port);idle.confirmNativeUpdateIdle();
  for(int i=1;i<=5;++i){mode=i;rejects([&]{idle.confirmNativeUpdateIdle();});}mode=0;
  for(const char* status:{"starting","running","uncertain","waiting-close","activating","verifying","deleting-original","deleting-game","unknown-status"}) {
    writeJson(root/"compressor/state.json",{{"status",status}});Compressor pending;pending.init(paths,port);rejects([&]{pending.confirmNativeUpdateIdle();});
  }
  writeJson(root/"compressor/state.json",{{"status","ready"}});writeJson(root/"compressor/games.json",{{std::string(32,'a'),{{"status","uncertain"}}}});
  Compressor protectedRecord;protectedRecord.init(paths,port);rejects([&]{protectedRecord.confirmNativeUpdateIdle();});assert(posts==0);
  bool needsRecovery=false;try{protectedRecord.confirmNativeUpdateIdle();}catch(const std::logic_error&){needsRecovery=true;}assert(needsRecovery);
  server.stop();thread.join();Compressor unavailable;unavailable.init(paths,port);rejects([&]{unavailable.confirmNativeUpdateIdle();});
  fs::remove_all(root);std::cout<<"Native idle gate tests passed\n";
}
