// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <array>
#include <string_view>
namespace botty {
// string_view::substr pulls in exception support absent from the native runtime.
inline std::string_view slice(std::string_view s, std::size_t pos,
                             std::size_t count=std::string_view::npos) noexcept {
    if(pos>s.size())return {};
    if(count>s.size()-pos)count=s.size()-pos;
    return {s.data()+pos,count};
}
enum class Probe { checking, ready, legacy, unavailable, rejected, incompatible, malformed, workerError, transmissionUnavailable };
struct Buttons {
    static constexpr unsigned up=0x10, down=0x40, left=0x80, right=0x20,
        cross=0x4000, circle=0x2000, square=0x8000, triangle=0x1000, l1=0x400, r1=0x800, options=8;
};
struct Input {
    unsigned previous=0, direction=0;
    std::uint64_t nextRepeat=0;
    bool armed=false;
    // Require release after startup, reconnect, or focus discontinuity.
    unsigned update(unsigned buttons, bool connected, std::uint64_t now) noexcept {
        if (!connected) { previous=direction=0; armed=false; return 0; }
        if (!armed) { if (!buttons) armed=true; previous=buttons; return 0; }
        unsigned edges=buttons & ~previous;
        const unsigned nav=buttons & (Buttons::up|Buttons::down|Buttons::left|Buttons::right);
        if (nav!=direction) { direction=nav; nextRepeat=now+400000; }
        else if (nav && now>=nextRepeat) { edges|=nav; nextRepeat=now+140000; }
        previous=buttons;
        return edges;
    }
};
// Preserve short presses contained between two renders. Consume one event per
// UI step so a buffered Select cannot open and confirm a dialog in one frame.
struct InputEvents {
    Input input;
    std::array<unsigned,128> events{};
    unsigned head=0,count=0,buttons=0;
    bool connected=false;
    std::uint64_t timestamp=0;
    void reset() noexcept {input=Input{};head=count=buttons=0;connected=false;timestamp=0;}
    void ingest(unsigned value,bool live,std::uint64_t stamp,std::uint64_t now) noexcept {
        if(stamp<=timestamp)return;timestamp=stamp;
        if(!live){head=count=buttons=0;connected=false;input.update(0,false,now);return;}
        connected=true;buttons=value;const auto edge=input.update(value,true,now);
        if(edge){if(count==events.size()){head=count=0;input.update(0,false,now);return;}events[(head+count)%events.size()]=edge;++count;}
    }
    unsigned next(std::uint64_t now) noexcept {
        if(count){const auto event=events[head];head=(head+1)%events.size();--count;return event;}
        return input.update(buttons,connected,now);
    }
};
struct Model {
    // Browse -> find -> download -> prepare -> collect -> connect.
    static constexpr std::array<unsigned,6> tabOrder{5,4,0,1,2,3};
    unsigned tab=5, selected=0, count=0, filter=0, detailPage=0, exploreSort=0;
    bool details=false;
    bool quitDialog=false, confirmQuit=false;
    enum class Action { none, retry, quit, menu, add, explore };
    Action press(unsigned edge) noexcept {
        if (quitDialog) {
            if (edge & Buttons::circle) { quitDialog=false; return Action::none; }
            if (edge & (Buttons::left|Buttons::right)) confirmQuit=!confirmQuit;
            if (edge & Buttons::cross) {
                if (confirmQuit) return Action::quit;
                quitDialog=false;
            }
            return Action::none;
        }
        if (edge & Buttons::circle) {
            if(details)details=false;else {quitDialog=true;confirmQuit=false;}
            return Action::none;
        }
        if (edge & (Buttons::l1|Buttons::r1)) {
            unsigned index=0;while(index<tabOrder.size()&&tabOrder[index]!=tab)++index;
            tab=tabOrder[(index+((edge&Buttons::r1)?1:5))%6];selected=0;details=false;return Action::none;
        }
        if(tab==5&&(edge&Buttons::triangle)){exploreSort=(exploreSort+1)%3;selected=0;return Action::explore;}
        if(edge & Buttons::options)return Action::menu;
        if(edge & Buttons::triangle)return Action::retry;
        if(edge & Buttons::square)return Action::add;
        if(tab==3) {
            if(edge&(Buttons::up|Buttons::down|Buttons::left|Buttons::right))selected=1-selected;
            if(edge&Buttons::cross){if(selected==0)return Action::retry;quitDialog=true;confirmQuit=false;}
        }else if(tab==5){
            if((edge&Buttons::right)&&selected+1<count)++selected;
            if((edge&Buttons::left)&&selected)--selected;
            if((edge&Buttons::down)&&selected+6<count)selected+=6;
            if((edge&Buttons::up)&&selected>=6)selected-=6;
            if((edge&Buttons::cross)&&count)details=true;
        }else if(tab==2&&!details) {
            if((edge&Buttons::right)&&selected+1<count)++selected;
            if((edge&Buttons::left)&&selected)--selected;
            if((edge&Buttons::down)&&selected+3<count)selected+=3;
            if((edge&Buttons::up)&&selected>=3)selected-=3;
            if((edge&Buttons::cross)&&count){details=true;detailPage=0;}
        }else if(details) {
            if(edge&Buttons::down)++detailPage;
            if((edge&Buttons::up)&&detailPage)--detailPage;
        }else {
            if(tab==0&&(edge&(Buttons::left|Buttons::right))){filter=(filter+((edge&Buttons::right)?1:2))%3;selected=0;}
            if((edge&Buttons::down)&&selected+1<count)++selected;
            if((edge&Buttons::up)&&selected)--selected;
            if((edge&Buttons::cross)&&count){details=true;detailPage=0;}
        }
        return Action::none;
    }
};
Probe parseHealth(std::string_view body) noexcept;
const char* probeText(Probe state) noexcept;
}
