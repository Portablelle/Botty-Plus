// SPDX-License-Identifier: GPL-3.0-or-later
#include "botw_profile.h"
#include <cassert>
#include <iostream>

int main(int argc,char** argv) {
 assert(argc==4);
 namespace fs=std::filesystem;
 namespace P=Eden::BotwProfile;
 using namespace Eden::UltraCam;
 const fs::path source160=argv[1],source190=argv[2],root=argv[3];
 const auto legacy=root/"mods"/"01007EF00011E000"/kName;
 const auto modern=legacy.parent_path()/P::kName190;
 const auto settings=(root/"settings.json").string();
 auto values=kDefaults;values[Enabled]=1;std::string error,oldIni,oldSettings;
 assert(Save(kTitle,"1.6.0",values,error,source160,legacy,settings));
 assert(Read(legacy/"romfs/UltraCam/maxlastbreath.ini",8192,oldIni));
 assert(P::Compatible(kTitle,"1.6.0")&&P::Compatible(kTitle,"1.9.0"));
 assert(!P::Compatible(kTitle,"1.8.2")&&!P::Compatible(kTitle+1,"1.9.0"));
 assert(!P::Save190(kTitle,"1.6.0",values,error,source190,modern,settings));
 assert(!fs::exists(modern));
 assert(Eden::SaveModEnabled(kTitle,"Owner's mod",false,settings));
 assert(P::Save190(kTitle,"1.9.0",values,error,source190,modern,settings));
 assert(P::Load190(modern,settings)==values&&Load(legacy,settings)==values);
 std::string kept;assert(Read(legacy/"romfs/UltraCam/maxlastbreath.ini",8192,kept)&&kept==oldIni);
 auto off=Eden::LoadDisabledMods(kTitle,settings);
 assert(std::find(off.begin(),off.end(),"Owner's mod")!=off.end());
 auto gated=off;P::Gate(kTitle,"1.9.0",gated,true);
 assert(std::find(gated.begin(),gated.end(),kName)!=gated.end());
 assert(std::find(gated.begin(),gated.end(),P::kName190)==gated.end());
 assert(std::find(gated.begin(),gated.end(),P::kManual190)!=gated.end());
 gated=off;P::Gate(kTitle,"1.6.0",gated,true);
 assert(std::find(gated.begin(),gated.end(),P::kName190)!=gated.end());
 assert(std::find(gated.begin(),gated.end(),kName)==gated.end());
 gated=off;P::Gate(kTitle,"",gated,true);
 assert(std::find(gated.begin(),gated.end(),kName)!=gated.end());
 assert(std::find(gated.begin(),gated.end(),P::kName190)!=gated.end());
 gated=off;P::Gate(kTitle+1,"1.9.0",gated,true);assert(gated==off);
 assert(Read(settings,1024*1024,oldSettings));
 auto changed=values;changed[TargetFps]=1;
 assert(!P::Save190(kTitle,"1.9.0",changed,error,source190,modern,settings,
  [&](std::string_view phase){if(phase=="published")fs::create_directory(settings+".tmp");}));
 fs::remove(settings+".tmp");assert(Read(settings,1024*1024,kept)&&kept==oldSettings);
 assert(P::Load190(modern,settings)==values);
 const auto patch=modern/"exefs"/(std::string(P::kMain190)+".pchtxt");
 std::string original;assert(Read(patch,16384,original));Write(patch,"owner edit");
 assert(!P::Save190(kTitle,"1.9.0",changed,error,source190,modern,settings));
 assert(Read(patch,16384,kept)&&kept=="owner edit");
 auto disabled=values;disabled[Enabled]=0;
 assert(P::Save190(kTitle,"1.9.0",disabled,error,source190,modern,settings));
 assert(!P::Load190(modern,settings)[Enabled]);Write(patch,original);
 assert(P::Save190(kTitle,"1.9.0",values,error,source190,modern,settings));
 assert(Eden::SaveModsEnabled(kTitle,false,settings));
 assert(!P::Save190(kTitle,"1.9.0",values,error,source190,modern,settings));
 assert(Eden::SaveModsEnabled(kTitle,true,settings));
 changed=values;changed[FreeCamera]=1;
 assert(!P::Save190(kTitle,"1.9.0",changed,error,source190,modern,settings));
 Write(modern/"extra.txt","owner file");
 assert(!P::Save190(kTitle,"1.9.0",values,error,source190,modern,settings));
 fs::remove(modern/"extra.txt");
 const auto linked=root/"linked";fs::create_directory_symlink(modern,linked);
 assert(!P::Save190(kTitle,"1.9.0",values,error,source190,linked,settings));
 const auto corrupt=root/"corrupt";fs::create_directories(corrupt);Write(corrupt/"patches.json","bad");
 assert(!P::Save190(kTitle,"1.9.0",values,error,corrupt,modern,settings));
 assert(P::Save190(kTitle,"1.8.2",disabled,error,corrupt,modern,settings));
 assert(P::Load190(modern,settings)[Enabled]==0);
 assert(Load(legacy,settings)==values); // switching back restores the original profile.
 Eden::Settings::Json cases=Eden::Settings::Json::array();
 for(int fps=0;fps<kDefinitions[TargetFps].count;++fps)
  for(int distance=0;distance<kDefinitions[Distance].count;++distance)
   for(int fov=0;fov<kDefinitions[Fov].count;++fov){
    auto v=kDefaults;v[TargetFps]=fps;v[Distance]=distance;v[Fov]=fov;
    cases.push_back({{"fps",std::stoi(Value(v,TargetFps))},{"distance",std::stoi(Value(v,Distance))},
     {"fov",std::stoi(Value(v,Fov))},{"files",P::PatchFiles(source190,v)}});
   }
 Write(root/"patch-cases.json",cases.dump());
 std::cout<<"BOTW profiles: version gates, independent 1.6/1.9 settings, conflicts, rollback, preservation and 160 patch variants PASS\n";
}
