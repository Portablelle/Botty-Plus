// SPDX-License-Identifier: GPL-3.0-or-later
#include "forwarded_launch.h"
#include "settings_store.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>
int main() {
    char flag[]="--botty-config", romFlag[]="--rom", rom[]="/data/botty/emulation/switch/roms/Game.xci";
    char* args[]={romFlag,rom,flag};
    auto parsed=Eden::ParseForwardedArgs(3,args);
    assert(parsed.configuration && parsed.rom==rom && !parsed.exit_after_game);
    char* global[]={flag};assert(Eden::ParseForwardedArgs(1,global).configuration);
    assert(!Eden::ParseForwardedArgs(0,nullptr).configuration);
    const std::string batch=".botty-batch-"+std::string(32,'a')+"/Game.xci";
    assert(Eden::ValidRomFilename(batch));
    assert(Eden::ValidRomFilename("Collection/Game.xci"));
    assert(!Eden::ValidRomFilename("Collection/../Game.xci"));
    assert(!Eden::ValidRomFilename("Collection//Game.xci"));
    for(const std::string& bad:std::vector<std::string>{"../Game.xci", "/Game.xci", ".botty-batch-"+std::string(31,'a')+"/Game.xci", ".botty-batch-"+std::string(32,'z')+"/Game.xci", ".botty-batch-"+std::string(32,'a')+"/../Game.xci"})
        assert(!Eden::ValidRomFilename(bad));
    char folder[]="/tmp/botty-engine-config-XXXXXX";assert(mkdtemp(folder));
    const std::string file=std::string(folder)+"/settings.json";
    Eden::Preferences prefs; prefs.output=2; prefs.resolution=4; prefs.refresh=1; prefs.volume=55; prefs.frame_gen_method=1;
    assert(Eden::SavePreferences(prefs,file));
    Eden::GameSettings game;game.resolution=1;game.volume=80; game.frame_gen_method=0; game.speed_limit=4;
    constexpr uint64_t title=0x0100BDE00862A000;
    assert(Eden::SaveGameSettings(title,game,file));
    const auto general=Eden::LoadPreferences(file),specific=Eden::PreferencesFor(title,file);
    assert(general.frame_gen_method==1);
    Eden::PerformanceSettings speed; speed.speed_limit=2;
    assert(Eden::SavePerformance(0,speed,file));
    assert(Eden::LoadPerformance(0,file).speed_limit==2);
    assert(specific.frame_gen_method==0 && Eden::LoadPerformance(title,file).speed_limit==4);
    assert(general.output==2 && general.resolution==4 && general.refresh==1 && general.volume==55);
    assert(specific.output==2 && specific.resolution==1 && specific.volume==80);
    assert(Eden::PreferencesFor(title+1,file).resolution==4);
    for (const auto& relative : {batch, std::string("Collection/Game.xci")}) {
        const auto history=Eden::RelativeRomHistory("/roms/"+relative,"/roms");
        assert(history==relative);
        assert(Eden::SaveLastGame(history,file) && Eden::LoadLastGame(file)==relative);
        assert(Eden::SaveRecentGame(history,file));
    }
    assert(Eden::LoadRecentGames(file).size()==2);
    assert(Eden::RelativeRomHistory("/outside/Game.xci","/roms").empty());
    std::filesystem::remove_all(folder);
    std::puts("Configuration forwarding, owned batch filenames, 2160p persistence and per-game overrides PASS");
}
