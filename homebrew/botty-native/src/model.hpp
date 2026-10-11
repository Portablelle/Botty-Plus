// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <array>
#include <string_view>
#include "version.hpp"
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
    // Display order of the Discover ranking chips.
    static constexpr std::array<const char*,3> exploreSorts{"newest","seeders","completed"};
    static constexpr std::array<const char*,3> exploreLabels{"Newest","Most seeded","Most grabbed"};
    // Browse -> follow -> collect -> manage. Circle never quits; Close lives in System.
    enum Tab : unsigned { discover=0, activity=1, library=2, system=3 };
    static constexpr unsigned tabCount=4, libraryColumns=7, systemItems=3;
    unsigned tab=discover, selected=0, count=0, filter=0, detailPage=0, exploreSort=0;
    unsigned detailButton=0, buttonCount=0;
    bool details=false, searchResults=false;
    // Inline System confirmation; Cancel stays focused until moved explicitly.
    bool updateDialog=false, confirmUpdate=false;
    enum class Action { none, retry, quit, menu, add, explore, update, installUpdate, search, get, compare, run };
    Action press(unsigned edge) noexcept {
        if (updateDialog) {
            if (edge & Buttons::circle) { updateDialog=false; return Action::none; }
            if (edge & (Buttons::left|Buttons::right)) confirmUpdate=!confirmUpdate;
            if (edge & Buttons::cross) {
                updateDialog=false;
                if (confirmUpdate) return Action::installUpdate;
            }
            return Action::none;
        }
        if (edge & (Buttons::l1|Buttons::r1)) {
            tab=(tab+((edge&Buttons::r1)?1:tabCount-1))%tabCount;
            selected=filter=detailPage=detailButton=0;details=false;return Action::none;
        }
        if (edge & Buttons::circle) {
            if(details)details=false;
            else if(tab==discover&&searchResults){searchResults=false;selected=0;}
            return Action::none;
        }
        if(details){
            if((edge&Buttons::right)&&detailButton+1<buttonCount)++detailButton;
            if((edge&Buttons::left)&&detailButton)--detailButton;
            if(edge&Buttons::down)++detailPage;
            if((edge&Buttons::up)&&detailPage)--detailPage;
            if(edge&Buttons::options)return Action::menu;
            if((edge&Buttons::cross)&&buttonCount)return Action::run;
            return Action::none;
        }
        if(tab==discover){
            if(edge&Buttons::square)return Action::search;
            if(searchResults){
                if((edge&Buttons::down)&&selected+1<count)++selected;
                if((edge&Buttons::up)&&selected)--selected;
                return (edge&Buttons::cross)&&count?Action::get:Action::none;
            }
            if(edge&Buttons::triangle){exploreSort=(exploreSort+1)%3;selected=0;return Action::explore;}
            if((edge&Buttons::right)&&selected+1<count)++selected;
            if((edge&Buttons::left)&&selected)--selected;
            if((edge&Buttons::options)&&count)return Action::compare;
            return (edge&Buttons::cross)&&count?Action::get:Action::none;
        }
        if(tab==system){
            if((edge&Buttons::down)&&selected+1<systemItems)++selected;
            if((edge&Buttons::up)&&selected)--selected;
            if(edge&Buttons::triangle)return Action::retry;
            if(edge&Buttons::cross)return selected==0?Action::update:selected==1?Action::retry:Action::quit;
            return Action::none;
        }
        if(edge & Buttons::options)return count?Action::menu:Action::none;
        if(tab==library){
            if(edge&Buttons::triangle){filter=(filter+1)%4;selected=0;}
            if((edge&Buttons::right)&&selected+1<count)++selected;
            if((edge&Buttons::left)&&selected)--selected;
            if((edge&Buttons::down)&&selected+libraryColumns<count)selected+=libraryColumns;
            if((edge&Buttons::up)&&selected>=libraryColumns)selected-=libraryColumns;
        }else {
            if(edge&Buttons::square)return Action::add;
            if(edge&Buttons::triangle)return Action::retry;
            if(edge&(Buttons::left|Buttons::right)){filter=(filter+((edge&Buttons::right)?1:3))%4;selected=0;}
            if((edge&Buttons::down)&&selected+1<count)++selected;
            if((edge&Buttons::up)&&selected)--selected;
        }
        if((edge&Buttons::cross)&&count){details=true;detailPage=detailButton=0;}
        return Action::none;
    }
};
Probe parseHealth(std::string_view body) noexcept;
const char* probeText(Probe state) noexcept;
}
