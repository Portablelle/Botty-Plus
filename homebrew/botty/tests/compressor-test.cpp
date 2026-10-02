#include "httplib.h"
#include "compressor.hpp"
#include <cassert>
#include <fstream>
#include <thread>
using namespace botty;
int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const auto root=fs::temp_directory_path()/ ("botty-compressor-"+randomId());
  Paths paths(root,root/"library");fs::create_directories(root/"compressor");
  const auto source=paths.library/"PPSA23732-app";fs::create_directories(source/"sce_sys");
  writeJson(source/"sce_sys/param.json",{{"titleId","PPSA23732"}});
  std::ofstream(source/"eboot.bin")<<"original";
  json job={{"id",std::string(32,'a')},{"status","moved"},{"destination",source.string()},{"content",{{"kind","folder"},{"titleId","PPSA23732"},{"destination","PPSA23732-app"}}}};
  auto rejects=[](auto fn){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}assert(failed);};
  Compressor disabled;disabled.init(paths);rejects([&]{disabled.start(job);});
  writeJson(root/"compressor/enabled.json",{{"mode","library-1.2"}});
  std::ofstream(root/"compressor/token")<<std::string(64,'a');
  httplib::Server server;std::atomic<int> posts{0};std::atomic<bool> complete{false},lost{false},wrong{false};
  const auto epoch=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  server.set_pre_routing_handler([](const auto& req,auto& res){assert(req.get_param_value("token")==std::string(64,'a'));res.set_header("Content-Type","application/json");return httplib::Server::HandlerResponse::Unhandled;});
  server.Get("/api/status",[](const auto&,auto& res){res.set_content(R"({"ok":true,"bottyWorker":"library-1.2"})","application/json");});
  server.Get("/api/gc/job",[&](const auto&,auto& res){res.set_content(json({{"ok",true},{"busy",posts.load()>0&&!complete},{"activeId",wrong?"op-9":"op-1"},{"phase","compressing"},{"copiedBytes",5},{"totalBytes",10}}).dump(),"application/json");});
  server.Post("/api/gc/compress",[&](const auto& req,auto& res){
    ++posts;assert(req.get_param_value("sourcePath")==source.string());assert(req.get_param_value("deletePolicy")=="keep");assert(req.get_param_value("format")=="exfat");
    if(lost){res.status=503;res.set_content(R"({"ok":false})","application/json");return;}
    res.set_content(R"({"ok":true,"id":"op-1"})","application/json");
  });
  server.Get("/api/gc/history",[&](const auto&,auto& res){res.set_content(json({{"ok",true},{"history",json::array({{{"id","op-1"},{"sourcePath",source.string()},{"createdAt",epoch},{"status",complete?"success":"running"},{"result",complete?"copy-created-unverified":""},{"outputPath",(root/"copy.ffpfsc").string()}}})}}).dump(),"application/json");});
  server.Post("/api/gc/job/cancel",[](const auto&,auto& res){res.set_content(R"({"ok":true})","application/json");});
  const int port=server.bind_to_any_port("127.0.0.1");std::thread thread([&]{server.listen_after_bind();});
  Compressor c;c.init(paths,port);
  auto bad=job;bad["content"]["titleId"]="PPSA31246";rejects([&]{c.start(bad);});assert(posts==0);
  fs::create_symlink(root,source/"escape");rejects([&]{c.start(job);});fs::remove(source/"escape");assert(posts==0);
  auto started=c.start(job);assert(started["status"]=="running"&&c.busy());rejects([&]{c.start(job);});rejects([&]{c.requireIdle();});assert(posts==1);
  c.poll();assert(c.state()["bytes"]==5);wrong=true;rejects([&]{c.cancel(std::string(32,'a'));});wrong=false;c.cancel(std::string(32,'a'));
  Compressor recovered;recovered.init(paths,port);assert(recovered.busy());complete=true;recovered.poll();assert(recovered.busy());assert(recovered.state()["status"]=="waiting-close");rejects([&]{recovered.start(job);});assert(posts==1);
  assert(readText(source/"eboot.bin")=="original");
  fs::remove(root/"compressor/state.json");fs::remove(root/"compressor/games.json");Compressor uncertain;uncertain.init(paths,port);lost=true;
  rejects([&]{uncertain.start(job);});assert(uncertain.state()["status"]=="uncertain"&&uncertain.busy());
  Compressor afterCrash;afterCrash.init(paths,port);assert(afterCrash.busy());rejects([&]{afterCrash.start(job);});assert(posts==2);
  const auto id=std::string(32,'a');
  json ready={{"jobId",id},{"status","ready"},{"verified",true},{"originalKept",false},{"titleId","PPSA23732"},{"output",(root/"compressor/output/PPSA23732.ffpfsc").string()}};
  writeJson(root/"compressor/state.json",ready);writeJson(root/"compressor/games.json",json{{id,ready}});
  Compressor deletion;deletion.init(paths,port);assert(deletion.state()["deletionSupported"]==true);
  auto queued=deletion.requestGameDeletion(id);assert(queued["status"]=="waiting-close"&&queued["deleteGameRequested"]==true&&deletion.busy());
  rejects([&]{deletion.requestGameDeletion(id);});rejects([&]{deletion.requestRestore(id);});
  queued["status"]="deleting-game";queued["gameDeletionStarted"]=true;writeJson(root/"compressor/state.json",queued);
  Compressor interruptedDelete;interruptedDelete.init(paths,port);assert(interruptedDelete.state()["status"]=="uncertain"&&interruptedDelete.busy());
  rejects([&]{interruptedDelete.requestGameDeletion(id);});rejects([&]{interruptedDelete.requestRestore(id);});
  // Retry cleans only the identified failed copy, before submitting new work.
  const auto output=root/"compressor/output";fs::create_directories(output);
  const auto temporary=output/".PPSA23732.ffpfsc.gc-compress.tmp";
  const auto hashes=fs::path(temporary.string()+".vhash");
  const auto other=output/".PPSA99999.ffpfsc.gc-compress.tmp";
  std::ofstream(other)<<"other game";
  json failed={{"jobId",id},{"status","failed"},{"originalKept",true},{"titleId","PPSA23732"},{"source",source.string()}};
  auto seed=[&](const json& rec){writeJson(root/"compressor/state.json",rec);writeJson(root/"compressor/games.json",json{{id,rec}});};
  auto retry=[&]{Compressor next;next.init(paths,port);return next.start(job);};
  auto partial=[&]{std::ofstream(temporary)<<"unfinished";std::ofstream(hashes)<<"partial hashes";};
  lost=false;complete=true;
  for(const auto* status:{"failed","cancelled"}) {
    failed["status"]=status;seed(failed);partial();const auto before=posts.load();
    assert(retry()["status"]=="running");assert(posts==before+1);
    assert(!fs::exists(temporary)&&!fs::exists(hashes));
    assert(readText(other)=="other game"&&readText(source/"eboot.bin")=="original");
  }
  seed(failed);partial();complete=false;const auto before=posts.load();
  rejects(retry);assert(fs::exists(temporary)&&fs::exists(hashes)&&posts==before);complete=true;
  // A completed image, unknown owner, changed source or recovery state is kept.
  const auto image=output/"PPSA23732.ffpfsc";std::ofstream(image)<<"finished";
  rejects(retry);assert(fs::exists(temporary)&&readText(image)=="finished");fs::remove(image);
  for(const auto& change:std::vector<json>{{{"source","/different/source"}},{{"titleId","PPSA99999"}},{{"jobId",std::string(32,'b')}},{{"originalKept",false}},{{"verified",true}},{{"originalPath",(root/"backup").string()}},{{"status","uncertain"}}}) {
    auto rec=failed;rec.update(change);seed(rec);rejects(retry);assert(fs::exists(temporary)&&fs::exists(hashes)&&posts==before);
  }
  seed(failed);const auto backup=root/"compressor/originals"/id;fs::create_directories(backup);
  rejects(retry);assert(fs::exists(temporary)&&fs::exists(hashes));fs::remove(backup);
  const auto finalHashes=fs::path(image.string()+".vhash");std::ofstream(finalHashes)<<"committed hashes";
  rejects(retry);assert(fs::exists(temporary)&&readText(finalHashes)=="committed hashes");fs::remove(finalHashes);
  seed(json{{"status","idle"}});rejects(retry);assert(fs::exists(temporary));
  seed(failed);fs::remove(hashes);fs::create_symlink(source/"eboot.bin",hashes);
  rejects(retry);assert(fs::exists(temporary)&&readText(source/"eboot.bin")=="original");fs::remove(hashes);
  fs::create_directory(hashes);rejects(retry);assert(fs::exists(temporary));fs::remove(hashes);
  // Interrupted cleanup is idempotent: one temporary may already be absent.
  assert(retry()["status"]=="running");assert(!fs::exists(temporary));
  seed(failed);std::ofstream(hashes)<<"hashes only";assert(retry()["status"]=="running");assert(!fs::exists(hashes));
  assert(readText(other)=="other game"&&readText(source/"eboot.bin")=="original");
  server.stop();thread.join();fs::remove_all(root);curl_global_cleanup();
}
