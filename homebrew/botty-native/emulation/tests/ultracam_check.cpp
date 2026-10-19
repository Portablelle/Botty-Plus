// SPDX-License-Identifier: GPL-3.0-or-later
#include "ultracam.h"
#include <cassert>
#include <iostream>
int main(int argc,char** argv){
 assert(argc==3);namespace fs=std::filesystem;using namespace Eden::UltraCam;
 const fs::path source=argv[1],root=argv[2],mod=root/"mods"/"Botty UltraCam";fs::create_directories(root);
 fs::create_directories(root/"case"/"01007ef00011e000");assert(Root((root/"case").string())==root/"case"/"01007ef00011e000"/kName);
 const auto settings=(root/"settings.json").string();auto values=kDefaults;values[0]=1;values[2]=0;std::string error;
 assert(!Save(kTitle,"1.5.0",values,error,source,mod,settings));assert(!fs::exists(mod));
 assert(!Save(kTitle+1,"1.6.0",values,error,source,mod,settings));assert(!fs::exists(mod));
 assert(Eden::SaveModEnabled(kTitle,"Other mod",false,settings));
 assert(Save(kTitle,"1.6.0",values,error,source,mod,settings));assert(Load(mod,settings)==values);
 std::string ini;assert(Read(mod/"romfs/UltraCam/maxlastbreath.ini",8192,ini));assert(ini.find("MaxFramerate = 60")!=std::string::npos);assert(ini.find("Width = 1280")!=std::string::npos);
 assert(Eden::SaveModsEnabled(kTitle,false,settings));assert(!Save(kTitle,"1.6.0",values,error,source,mod,settings));assert(!Eden::LoadModsEnabled(kTitle,settings));assert(Load(mod,settings)[Enabled]==1);assert(Eden::SaveModsEnabled(kTitle,true,settings));
 const auto off=Eden::LoadDisabledMods(kTitle,settings);assert(std::find(off.begin(),off.end(),"Other mod")!=off.end());
 values[0]=0;assert(Save(kTitle,"1.7.0",values,error,source,mod,settings));assert(Load(mod,settings)[0]==0);
 values[0]=1;assert(Save(kTitle,"1.6.0",values,error,source,mod,settings));
 std::string originalIni,originalSettings;
 assert(Read(mod/"romfs/UltraCam/maxlastbreath.ini",8192,originalIni));assert(Read(settings,1024*1024,originalSettings));
 auto different=values;different[1]=1;
 assert(!Save(kTitle,"1.6.0",different,error,source,mod,settings,[&](std::string_view phase){if(phase=="published")fs::create_directory(settings+".tmp");}));
 fs::remove(settings+".tmp");
 std::string restored;assert(Read(settings,1024*1024,restored)&&restored==originalSettings);assert(Read(mod/"romfs/UltraCam/maxlastbreath.ini",8192,restored)&&restored==originalIni);assert(Load(mod,settings)==values);
 Write(mod/"romfs/UltraCam/maxlastbreath.ini","owner edit");assert(!Save(kTitle,"1.6.0",values,error,source,mod,settings));assert(Read(mod/"romfs/UltraCam/maxlastbreath.ini",8192,restored)&&restored=="owner edit");auto disabled=values;disabled[Enabled]=0;assert(Save(kTitle,"1.6.0",disabled,error,source,mod,settings));assert(Load(mod,settings)[Enabled]==0);Write(mod/"romfs/UltraCam/maxlastbreath.ini",originalIni);assert(Save(kTitle,"1.6.0",values,error,source,mod,settings));
 auto benchmark=values;benchmark[13]=2;assert(Ini(benchmark).find("Benchmark = 3")!=std::string::npos);
 const auto npdm=mod/"exefs/main.npdm";Write(npdm,"changed by owner");assert(!Save(kTitle,"1.6.0",values,error,source,mod,settings));std::string changed;assert(Read(npdm,100,changed)&&changed=="changed by owner");
 const auto unmanaged=root/"unmanaged";fs::create_directories(unmanaged);Write(unmanaged/"keep.txt","user mod");assert(!Save(kTitle,"1.6.0",values,error,source,unmanaged,settings));assert(Read(unmanaged/"keep.txt",100,changed)&&changed=="user mod");
 const auto linked=root/"linked";fs::create_directory_symlink(unmanaged,linked);assert(!Save(kTitle,"1.6.0",values,error,source,linked,settings));
 auto invalid=values;invalid[1]=100;assert(!Save(kTitle,"1.6.0",invalid,error,source,root/"invalid",settings));assert(!fs::exists(root/"invalid"));
 std::cout<<"UltraCam: verified install, per-game INI, dependency gate, disable after upgrade, preservation and symlink refusal PASS\n";
}
