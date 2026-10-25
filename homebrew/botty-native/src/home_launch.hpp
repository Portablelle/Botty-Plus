// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "actions.hpp"
#include <cstring>
namespace botty {
enum class HomeLaunch { none, valid, invalid };
inline HomeLaunch parseHomeLaunch(int argc,char** argv,Command& command) noexcept {
    if(argc<=0||!argv)return HomeLaunch::none;
    // Native launch arguments may start at argv[0]; host processes include the
    // executable name. Do not interpret other system startup arguments as a game.
    int at=-1;
    for(int i=0;i<argc;++i)if(argv[i]&&std::strcmp(argv[i],"--botty-home-game")==0){at=i;break;}
    if(at<0)return HomeLaunch::none;
    if(at>1||argc!=at+2||!argv[at+1]||std::strlen(argv[at+1])!=64)return HomeLaunch::invalid;
    for(const char* p=argv[at+1];*p;++p)if(!((*p>='a'&&*p<='f')||(*p>='0'&&*p<='9')))return HomeLaunch::invalid;
    command=Command{};command.operation=Operation::emulationLaunch;
    std::memcpy(command.id.data(),argv[at+1],64);return HomeLaunch::valid;
}
}
