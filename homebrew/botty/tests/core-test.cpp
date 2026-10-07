#include "core.hpp"
#include "unicode.hpp"
#include "progress.hpp"
#include "operations.hpp"
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
  fs::create_directories(root/"delete/sub");
  {std::ofstream file(root/"delete/sub/member.part");file<<"download";}
  downloadedFiles(root/"delete",{"sub/member.part","missing/member"},false);
  Operations operations;uint64_t completed=0;
  {DeletionScope task(operations,"fixture","Fixture deletion");
   const auto report=task.reporter();
   downloadedFiles(root/"delete",{"sub/member.part","missing/member"},true,[&](uint64_t done,uint64_t total,const std::string& file){assert(total==2&&done>=completed);completed=done;report(done,total,file);assert(operations.state().at("bytes")==done);});task.complete();}
  assert(completed==2&&operations.state().at("status")=="completed");
  {DeletionScope failed(operations,"fixture","Failed deletion");}
  assert(operations.state().at("status")=="failed");
  fs::create_directories(root/"progress-tree/sub");{std::ofstream f(root/"progress-tree/sub/a");f<<"keep count";}
  completed=0;deleteGameDirectory(root,"progress-tree",[&](uint64_t done,uint64_t total,const std::string&){assert(total==3&&done>=completed);completed=done;});
  assert(completed==3&&!fs::exists(root/"progress-tree"));
  assert(!fs::exists(root/"delete/sub/member.part"));
  fs::create_directory_symlink(root/"app",root/"delete/link");
  fails([&]{downloadedFiles(root/"delete",{"link/eboot.bin"},true);});
  fails([&]{downloadedFiles(root/"delete",{"../outside"},true);});
  fails([&]{downloadedFiles(root/"delete",{"sub"},true);});
  const auto fixtures=fs::path("tests/fixtures");
  std::atomic<bool> cancelled{false};
  fails([&]{extractRar(fixtures/"app.rar",root/"cancelled",[&](const Progress&p){if(p.phase=="Checking archive headers")cancelled=true;},"",[&]{return cancelled.load();},1);});
  // This phase is reported before member paths are created. A byte-based
  // request races a small member on tmpfs and cannot guarantee no output.
  assert(cancelled&&!fs::exists(root/"cancelled/Demo/eboot.bin"));
  assert(fs::exists(fixtures/"app.rar"));
  extractRar(fixtures/"app.rar",root/"app",[](const Progress&){});
  assert(readText(root/"app/Demo/eboot.bin").size()==7600);
  // Resume trusts neither file size nor the old job progress: CRC verifies
  // existing output, including the final CRC of a member split over 163 volumes.
  const auto resume=root/"resume";fs::copy(root/"app",resume,fs::copy_options::recursive);
  const auto keptTime=fs::last_write_time(resume/"Demo/sce_sys/param.json");
  {std::ofstream file(resume/"Demo/eboot.bin",std::ios::binary);file<<std::string(7600,'x');}
  uint64_t reusedBytes=0;
  extractRar(fixtures/"app.rar",resume,[&](const Progress& p){if(p.phase=="Verifying existing files for resume")reusedBytes=p.bytes;},"",{},0,true);
  assert(reusedBytes==fs::file_size(resume/"Demo/sce_sys/param.json"));
  assert(fs::last_write_time(resume/"Demo/sce_sys/param.json")==keptTime);
  assert(readText(resume/"Demo/eboot.bin")==readText(root/"app/Demo/eboot.bin"));
  fs::create_directories(root/"resume-multi");fs::copy_file(fixtures/"multipart-expected.bin",root/"resume-multi/content.bin");
  const auto multiTime=fs::last_write_time(root/"resume-multi/content.bin");
  extractRar(fixtures/"multipart/sample.rar",root/"resume-multi",[](const Progress&){},"",{},1,true);
  assert(fs::last_write_time(root/"resume-multi/content.bin")==multiTime);
  {std::ofstream file(resume/"unrelated");file<<"keep";}
  fails([&]{extractRar(fixtures/"app.rar",resume,[](const Progress&){},"",{},0,true);});
  assert(readText(resume/"unrelated")=="keep");fs::remove(resume/"unrelated");
  fs::remove(resume/"Demo/eboot.bin");fs::create_symlink(root/"app/Demo/eboot.bin",resume/"Demo/eboot.bin");
  fails([&]{extractRar(fixtures/"app.rar",resume,[](const Progress&){},"",{},0,true);});
  assert(readText(root/"app/Demo/eboot.bin").size()==7600);
  fails([&]{extractRar(fixtures/"solid.rar",resume,[](const Progress&){},"",{},0,true);});
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
  const auto outside=root/"keep-outside";fs::create_directories(outside);std::ofstream(outside/"keep")<<"keep";
  fs::create_directory_symlink(outside,app/"unsafe-link");
  fails([&]{deleteLibraryGame(paths,moved);});assert(fs::exists(app/"eboot.bin"));assert(fs::exists(outside/"keep"));
  fs::remove(app/"unsafe-link");
  auto forged=moved;forged["destination"]=paths.library.string();fails([&]{deleteLibraryGame(paths,forged);});
  forged=moved;forged["content"]["titleId"]="PPSA99071";fails([&]{deleteLibraryGame(paths,forged);});
  forged=moved;forged["status"]="ready";fails([&]{deleteLibraryGame(paths,forged);});
  fs::rename(app,root/"held-game");fs::create_directory_symlink(root/"held-game",app);
  fails([&]{deleteLibraryGame(paths,moved);});fs::remove(app);fs::rename(root/"held-game",app);
  // Missing metadata after an interrupted removal must not prevent retry.
  fs::remove(app/"sce_sys/param.json");deleteLibraryGame(paths,moved);
  assert(!fs::exists(app));assert(fs::exists(outside/"keep"));
  assert(fs::exists(paths.extracted/id/"Demo/eboot.bin"));
  deleteLibraryGame(paths,moved); // Already removed externally: idempotent cleanup.
  std::cout<<"Core tests passed: real extraction, 163-volume r/s transition, missing volumes, CRC errors, traversal, symlinks, classification and no-overwrite moves.\n";
 }catch(...){fs::remove_all(root);throw;}
 fs::remove_all(root);
}
