#include "core.hpp"
#include "unicode.hpp"
#include "progress.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <atomic>
#include <thread>
using namespace botty;
template<typename F>void fails(F operation){bool caught=false;try{operation();}catch(const std::exception&){caught=true;}assert(caught);}
template<class Wide>void unicodeTests(){
 const std::string text=u8"ASCII/é日本/😀.bin";
 const auto wide=utf8ToWide<Wide>(text);assert(wideToUtf8(wide.c_str(),wide.size()+1)==text);
 for(const std::string bad:{std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),std::string("\xe2\x82"),std::string("x\0y",3)})fails([&]{utf8ToWide<Wide>(bad);});
 const Wide high[]={Wide(0xd800),0},low[]={Wide(0xdc00),0},unterminated[]={Wide('x')};
 fails([&]{wideToUtf8(high,2);});fails([&]{wideToUtf8(low,2);});fails([&]{wideToUtf8(unterminated,1);});
 const Wide pair[]={Wide(0xd83d),Wide(0xde00),0};assert(wideToUtf8(pair,3)==u8"😀");
}
void progressTests(){
 ProgressSchedule schedule;const auto start=ProgressSchedule::Clock::time_point{};
 unsigned publishes=0,checkpoints=0;
 for(unsigned ms=0;ms<=60000;++ms){const auto d=schedule.update(start+std::chrono::milliseconds(ms),"Extracting");publishes+=d.publish;checkpoints+=d.checkpoint;}
 assert(publishes==241);assert(checkpoints==7);
 auto done=schedule.update(start+std::chrono::milliseconds(60001),"Extraction verified");assert(done.publish&&done.checkpoint);
 auto repeated=schedule.update(start+std::chrono::milliseconds(60002),"Extraction verified");assert(!repeated.publish&&!repeated.checkpoint);
 ProgressSchedule phases;assert(phases.update(start,"Checking archive headers").checkpoint);
 assert(phases.update(start,"Extracting and checking CRC").checkpoint);
}
int main(){
 progressTests();
 {
  ExtractionEstimate e;auto t=ExtractionEstimate::Clock::time_point{};
  e.update(t,0,100000);assert(e.eta==-1);
  e.update(t+std::chrono::seconds(1),1000,100000);assert(e.rate==1000&&e.eta==-1);
  e.update(t+std::chrono::seconds(5),5000,100000);assert(e.rate==1000&&e.eta==95);
  e.update(t+std::chrono::seconds(10),5000,100000);assert(e.rate==500&&e.eta==190);
  e.update(t+std::chrono::seconds(11),100000,100000);assert(e.eta==0);
  e.update(t+std::chrono::seconds(12),0,0);assert(e.rate==0&&e.eta==-1);
 }

 unicodeTests<char16_t>();unicodeTests<char32_t>();unicodeTests<wchar_t>();
 const auto root=fs::temp_directory_path()/("botty-test-"+randomId());fs::create_directories(root);
 try{
  for(const auto& path:{"../escape","/outside","x/../outside","C:/outside","a\\..\\b"})fails([&]{safeRelative(path);});
  assert(safeRelative("Demo/sce_sys/param.json")=="Demo/sce_sys/param.json");
  fs::create_directories(root/"inside");fs::create_directory_symlink(root/"inside",root/"link");
  fails([&]{containedExisting(root,root/"link");});
  const auto fixtures=fs::path("tests/fixtures");
  std::atomic<bool> cancelled{false};
  fails([&]{extractRar(fixtures/"app.rar",root/"cancelled",[&](const Progress&p){if(p.bytes>0)cancelled=true;},"",[&]{return cancelled.load();},1);});
  assert(cancelled&&!fs::exists(root/"cancelled/Demo/eboot.bin"));
  assert(fs::exists(fixtures/"app.rar"));
  extractRar(fixtures/"app.rar",root/"app",[](const Progress&){});
  assert(readText(root/"app/Demo/eboot.bin").size()==7600);
  bool usedParallel=false;
  extractRar(fixtures/"solid.rar",root/"solid",[&](const Progress&p){if(p.phase.find("2 workers")!=std::string::npos)usedParallel=true;},"",{},2);
  assert(!usedParallel&&readText(root/"solid/one.bin")==std::string(1024,'a'));
  bool defaultThree=false;
  extractRar(fixtures/"app.rar",root/"default-three",[&](const Progress&p){if(p.phase.find("3 workers")!=std::string::npos)defaultThree=true;});
  assert(defaultThree&&readText(root/"default-three/Demo/eboot.bin")==readText(root/"app/Demo/eboot.bin"));
  uint64_t lastBytes=0;bool twoWorkers=false;
  extractRar(fixtures/"app.rar",root/"parallel",[&](const Progress&p){assert(p.bytes>=lastBytes&&p.bytes<=p.total);lastBytes=p.bytes;if(p.phase.find("2 workers")!=std::string::npos)twoWorkers=true;},"",{},2);
  assert(twoWorkers&&lastBytes==7624);assert(readText(root/"parallel/Demo/eboot.bin")==readText(root/"app/Demo/eboot.bin"));
  bool threeWorkers=false;
  extractRar(fixtures/"app.rar",root/"three",[&](const Progress&p){if(p.phase.find("3 workers")!=std::string::npos)threeWorkers=true;},"",{},3);
  assert(threeWorkers&&readText(root/"three/Demo/eboot.bin")==readText(root/"app/Demo/eboot.bin"));
  extractRar(fixtures/"multipart/sample.rar",root/"parallel-multi",[](const Progress&){},"",{},2);
  assert(readText(root/"parallel-multi/content.bin")==readText(fixtures/"multipart-expected.bin"));
  std::atomic<bool> parallelCancel{false};
  fails([&]{extractRar(fixtures/"app.rar",root/"parallel-cancel",[&](const Progress&p){if(p.bytes)parallelCancel=true;},"",[&]{return parallelCancel.load();},2);});
  std::thread invalid([&]{fails([&]{extractRar(fixtures/"bad-crc.rar",root/"parallel-bad",[](const Progress&){},"",{},2);});});
  extractRar(fixtures/"app.rar",root/"parallel-isolated",[](const Progress&){},"",{},2);invalid.join();
  assert(readText(root/"parallel-isolated/Demo/eboot.bin")==readText(root/"app/Demo/eboot.bin"));
  const auto found=classify(root/"app");assert(found["kind"]=="folder"&&found["destination"]=="PPSA12345-app");
  extractRar(fixtures/"multipart/sample.rar",root/"multi",[](const Progress&){});
  assert(readText(root/"multi/content.bin")==readText(fixtures/"multipart-expected.bin"));
  fails([&]{extractRar(fixtures/"traversal.rar",root/"traversal",[](const Progress&){});});
  fails([&]{extractRar(fixtures/"absolute.rar",root/"absolute",[](const Progress&){});});
  assert(!fs::exists(root/"escape.txt"));
  fails([&]{extractRar(fixtures/"bad-crc.rar",root/"bad-crc",[](const Progress&){});});
  const auto missing=root/"missing";fs::copy(fixtures/"multipart",missing,fs::copy_options::recursive);fs::remove(missing/"sample.r48");
  fails([&]{extractRar(missing/"sample.rar",root/"missing-out",[](const Progress&){});});
  Paths paths(root/"data",root/"library");fs::create_directories(paths.extracted);fs::create_directories(paths.jobs);
  std::string id=randomId();fs::rename(root/"app",paths.extracted/id);
  const auto privateApp=paths.extracted/id/"Demo";
  fs::permissions(privateApp,fs::perms::owner_all);
  fs::permissions(privateApp/"eboot.bin",fs::perms::owner_read|fs::perms::owner_write);
  json job={{"id",id},{"status","ready"}};auto moved=movePrepared(paths,job);
  assert(moved["status"]=="moved");assert(fs::exists(paths.library/"PPSA12345-app/sce_sys/param.json"));
  const auto app=paths.library/"PPSA12345-app";
  assert((fs::status(app).permissions()&fs::perms::all)==static_cast<fs::perms>(0755));
  assert((fs::status(app/"eboot.bin").permissions()&fs::perms::all)==static_cast<fs::perms>(0755));
  assert((fs::status(app/"sce_sys").permissions()&fs::perms::all)==static_cast<fs::perms>(0755));
  assert((fs::status(app/"sce_sys/param.json").permissions()&fs::perms::all)==static_cast<fs::perms>(0644));
  id=randomId();extractRar(fixtures/"app.rar",paths.extracted/id,[](const Progress&){});
  fails([&]{movePrepared(paths,json{{"id",id},{"status","ready"}});});
  assert(fs::exists(paths.extracted/id/"Demo/eboot.bin"));
  std::cout<<"Core tests passed: real extraction, 163-volume r/s transition, missing volumes, CRC errors, traversal, symlinks, classification and no-overwrite moves.\n";
 }catch(...){fs::remove_all(root);throw;}
 fs::remove_all(root);
}
