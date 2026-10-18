#include "emulation.hpp"
#include "switch-preparation.hpp"
#include <cassert>
#include <fstream>
using namespace botty;
int main(){
  const auto temp=fs::temp_directory_path()/("botty-eden-test-"+randomId());
  const auto data=temp/"eden",install=temp/"app";
  fs::create_directories(data/"roms");fs::create_directories(data/"updates");fs::create_directories(data/"config");fs::create_directories(install/"sce_sys");
  writeJson(install/"sce_sys/param.json",{{"titleId","PPSA99071"},{"contentVersion","01.004.000"}});
  {std::ofstream out(data/"roms/Game.xci",std::ios::binary);out<<std::string(256,'\0')<<"HEAD"<<"fixture";}
  {std::ofstream out(data/"updates/Update.nsp",std::ios::binary);out<<"PFS0fixture";}
  fs::create_directories(install/"emulation");
  {std::ofstream out(install/"emulation/eboot.bin");out<<"fixture";}
  writeJson(install/"emulation-runtime.json",{{"schema",1}});
  Emulation emulator(data,install);auto state=emulator.state();assert(state.at("installed")==true);assert(state.at("entries").size()==2);assert(state.at("error")=="");
  const auto malformed=data/"roms/Game.xci.botty.json";
  writeJson(malformed,{{"verified",true},{"kind","roms"},{"verifiedFileSize",fs::file_size(data/"roms/Game.xci")},{"verifiedMtime",fs::last_write_time(data/"roms/Game.xci").time_since_epoch().count()},{"titles",json::array()},{"versionLabel",1}});
  const auto resilient=emulator.state();assert(resilient.at("entries").size()==1 && resilient.at("error")!="");
  writeJson(malformed,{{"name","Downloaded fixture"},{"type","game"}});
  assert(Emulation::entry(data,data/"roms/Game.xci","download").at("name")=="Downloaded fixture");
  fs::remove(malformed);
  const auto id=state.at("entries").at(0).at("id").get<std::string>();assert(id.size()==64);assert(emulator.resolve(id).at("kind")=="roms");
  bool missing=false;try{emulator.launchPath(id);}catch(const std::exception&){missing=true;}assert(missing);
  fs::create_directories(data/"keys");fs::create_directories(data/"firmware");
  {std::ofstream out(data/"keys/prod.keys");out<<"test fixture, not keys";}
  {std::ofstream out(data/"firmware/test.nca");out<<"test fixture";}
  writeJson(data/"private-runtime.json",{{"schema",1}});
  assert(emulator.launchPath(id)==fs::canonical(data/"roms/Game.xci").string());
  const auto original=Emulation::sourceIdentity(data/"roms/Game.xci");
  {std::ofstream out(data/"roms/Game.xci",std::ios::binary|std::ios::app);out<<"changed";}
  assert(Emulation::sourceIdentity(data/"roms/Game.xci")!=original);
  fs::create_symlink(data/"updates/Update.nsp",data/"roms/link.nsp");assert(emulator.state().at("entries").size()==2);
  {std::ofstream out(data/"roms/Invalid.nsp");out<<"invalid";}
  state=emulator.state();assert(state.at("entries").size()==2);assert(state.at("error")!="");
  writeJson(data/"config/prosperoeden.json",{{"game_files","relative"}});assert(emulator.state().at("entries").empty());
  const auto external=temp/"external";fs::create_directories(external/"roms");
  writeJson(data/"config/prosperoeden.json",{{"game_files",external.string()}});assert(emulator.root()==external);assert(emulator.state().at("entries").empty());
  bool refused=false;try{emulator.resolve(id);}catch(const std::exception&){refused=true;}assert(refused);
  // Resume an interrupted empty destination reservation, then reject a missing
  // published member even if the on-disk transaction says complete.
  const auto stage=data/".botty-preparation"/"fixture";const std::string batch(32,'a');
  fs::create_directories(stage/"content/roms");fs::create_directories(stage/"content/updates");
  {std::ofstream out(stage/"content/roms/Fixture.nsp");out<<"fixture";}
  const auto file=stage/"content/roms/Fixture.nsp";
  const json receipt={{"schema",1},{"platform","switch"},{"files",json::array({{{"name","roms/Fixture.nsp"},{"size",7},{"identity",Emulation::sourceIdentity(file)}}})}};
  writeJson(stage/"prepared.json",receipt);
  fs::create_directories(data/"roms"/(".botty-batch-"+batch));
  assert(publishSwitchBatch(stage,data,batch)==receipt);
  const auto published=data/"roms"/(".botty-batch-"+batch)/"Fixture.nsp";
  fs::rename(published,temp/"retained.nsp");
  refused=false;try{publishSwitchBatch(stage,data,batch);}catch(const std::exception&){refused=true;}assert(refused);
  fs::rename(temp/"retained.nsp",published);assert(publishSwitchBatch(stage,data,batch)==receipt);
  fs::remove_all(temp);
}
