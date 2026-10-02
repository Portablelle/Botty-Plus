#include "compression-library.hpp"
#include <cassert>
#include <pthread.h>
#include <iostream>
using namespace botty;
#ifdef __PS5__
#undef assert
#define assert(condition) do { if(!(condition))throw std::runtime_error("Assertion failed: " #condition); } while(false)
#endif
void runTests(){
#ifdef __PS5__
 const fs::path base="/data/botty/compressor/selftests";
#else
 const auto base=fs::temp_directory_path();
#endif
 const auto root=base/("botty-library-compression-"+randomId());Paths paths(root/"data",root/"library");
 const auto source=paths.library/"Example Game",mounted=root/"runtime/PPSA12345",shadow=root/"shadow",apps=root/"apps",original=paths.root/"compressor/originals"/std::string(32,'a');
 unsigned rejectionCase=0;auto rejects=[&](auto fn){++rejectionCase;bool rejected=false;try{fn();}catch(...){rejected=true;}if(!rejected)throw std::runtime_error("Expected rejection at case "+std::to_string(rejectionCase));};
 fs::create_directories(source/"sce_sys");fs::create_directories(mounted);writeJson(source/"sce_sys/param.json",{{"titleId","PPSA12345"}});std::ofstream(source/"eboot.bin")<<"exact original contents";
 fs::copy(source,mounted,fs::copy_options::recursive);compareCompression(source,mounted);
 struct SmallStack {fs::path source,mounted;bool passed=false;} small{source,mounted,false};
 pthread_attr_t attr;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstacksize(&attr,65536));pthread_t smallThread;
 assert(!pthread_create(&smallThread,&attr,+[](void* p)->void*{auto& v=*static_cast<SmallStack*>(p);try{compareCompression(v.source,v.mounted);v.passed=true;}catch(...){}return nullptr;},&small));
 pthread_attr_destroy(&attr);pthread_join(smallThread,nullptr);assert(small.passed);
 std::ofstream(mounted/"eboot.bin")<<"different bytes_______";rejects([&]{compareCompression(source,mounted);});assert(readText(source/"eboot.bin")=="exact original contents");
 fs::copy_file(source/"eboot.bin",mounted/"eboot.bin",fs::copy_options::overwrite_existing);
#ifndef __PS5__
 fs::create_symlink(source,mounted/"link");rejects([&]{compareCompression(source,mounted);});fs::remove(mounted/"link");
#endif
 fs::create_directories(paths.root/"compressor/output");const auto image=paths.root/"compressor/output/PPSA12345.ffpfsc";std::ofstream(image)<<"isolated mock image";
 fs::create_directories(apps/"PPSA12345");std::ofstream(apps/"PPSA12345/mount.lnk")<<source.string();
 httplib::Server server;std::atomic<bool> selected{false},mount{false},busy{true};
 server.Post(R"(/api/v1/(.*))",[&](const auto& req,auto& res){auto input=json::parse(req.body);json answer={{"status",0}};
  if(req.path=="/api/v1/games/unmount"||req.path=="/api/v1/games/mount"){if(busy){res.status=409;answer["status"]=16;}else mount=req.path=="/api/v1/games/mount";}
  else if(req.path=="/api/v1/games/info")answer.update({{"path",selected?image.string():source.string()},{"source_type",selected?"image":"folder"},{"image_backed",selected.load()}});
  else if(req.path=="/api/v1/manual/add"){assert(input.at("path")==image.string());selected=true;}
  else if(req.path=="/api/v1/manual/remove")selected=false;
  res.set_content(answer.dump(),"application/json");
 });
 #ifdef __PS5__
 const int port=15917;assert(server.bind_to_port("127.0.0.1",port));
#else
 const int port=server.bind_to_any_port("127.0.0.1");assert(port>0);
#endif
 std::thread thread([&]{server.listen_after_bind();});
 struct StopServer {httplib::Server& server;std::thread& thread;~StopServer(){server.stop();if(thread.joinable())thread.join();}} stopServer{server,thread};
 CompressionLibrary library(paths,port,shadow,apps,root/"runtime");assert(!library.idle("PPSA12345"));busy=false;library.api("games/unmount",{{"title_id","PPSA12345"}});assert(library.idle("PPSA12345"));
 json rec={{"titleId","PPSA12345"},{"jobId",std::string(32,'a')},{"source",source.string()},{"output",image.string()}};std::vector<std::string> phases;
 auto save=[&]{phases.push_back(rec.at("status"));writeJson(root/"journal.json",rec);};
 library.activate(rec,save,{});assert(rec["status"]=="ready"&&rec["verified"]==true&&rec["originalKept"]==true);assert(!fs::exists(source)&&fs::exists(original)&&!mount);assert(phases.front()=="activating");
 std::ofstream(mounted/"eboot.bin")<<"corrupted after validation";rejects([&]{library.removeOriginal(rec,save,{});});assert(fs::exists(original));
 fs::copy_file(original/"eboot.bin",mounted/"eboot.bin",fs::copy_options::overwrite_existing);
 library.restore(rec,save);assert(fs::exists(source)&&!fs::exists(original)&&fs::exists(image));assert(rec["status"]=="restored");
 library.activate(rec,save,{});library.removeOriginal(rec,save,{});assert(!fs::exists(original)&&fs::exists(image)&&rec["originalKept"]==false);rejects([&]{library.removeOriginal(rec,save,{});});
 // Compressed-only deletion never needs the original or a torrent archive.
 const auto outside=root/"saves/keep.dat";fs::create_directories(outside.parent_path());std::ofstream(outside)<<"save data";
 std::ofstream(image.string()+".vhash")<<"hash fixture";
 std::ofstream(apps/"PPSA12345/mount_img.lnk")<<image.string()<<"\n";
 busy=true;rejects([&]{library.removeGame(rec,save);});assert(fs::exists(image));busy=false;
 auto bad=rec;bad["output"]=outside.string();rejects([&]{library.removeGame(bad,[]{});});assert(fs::exists(outside));
 #ifndef __PS5__
 fs::rename(image,image.string()+".kept");fs::create_symlink(outside,image);
 rejects([&]{library.removeGame(rec,save);});fs::remove(image);fs::rename(image.string()+".kept",image);
 #endif
 selected=false;rejects([&]{library.removeGame(rec,save);});assert(fs::exists(image));selected=true;
 library.removeGame(rec,save);assert(rec["status"]=="deleted"&&!fs::exists(image)&&!fs::exists(image.string()+".vhash"));
 assert(!fs::exists(apps/"PPSA12345/mount_img.lnk")&&readText(outside)=="save data");
 rejects([&]{library.removeGame(rec,save);});
 // A retained original is also removed, but unrelated data remains untouched.
 fs::copy(mounted,source,fs::copy_options::recursive);std::ofstream(image)<<"second mock image";
 std::ofstream(apps/"PPSA12345/mount.lnk")<<source.string();
 rec={{"titleId","PPSA12345"},{"jobId",std::string(32,'a')},{"source",source.string()},{"output",image.string()}};
 library.activate(rec,save,{});assert(fs::exists(original));
 library.removeGame(rec,save);assert(!fs::exists(original)&&!fs::exists(image)&&readText(outside)=="save data");
 server.stop();thread.join();deleteGameDirectory(root.parent_path(),root.filename());
}

int main(){
 try {runTests();
#ifdef __PS5__
  writeJson("/data/botty/compressor/selftest-121-result.json",{{"passed",true},{"test","isolated production compression-library operations"}});
#endif
  return 0;
 }catch(const std::exception& e){
#ifdef __PS5__
  writeJson("/data/botty/compressor/selftest-121-result.json",{{"passed",false},{"error",e.what()}});
#else
  std::cerr<<e.what()<<'\n';
#endif
  return 1;
 }
}
