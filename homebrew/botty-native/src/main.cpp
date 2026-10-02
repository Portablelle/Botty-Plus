// Botty+. SPDX-License-Identifier: GPL-3.0-or-later
#include "renderer.hpp"
#include "model.hpp"
#include "probe.hpp"
#include "platform.hpp"
#include <array>
#include <cstddef>
#include <cstdio>
#include <algorithm>
#ifdef BOTTY_HOST_PREVIEW
#include <cstdlib>
#endif
#include <fcntl.h>
#include <sys/types.h>
extern "C" {
#include "../vendor/ps5-pad.h"
int sceUserServiceGetInitialUser(int*);
int scePadRead(int,PS5_PadData*,int);
int sceSystemServiceLoadExec(const char*,const char**);
int sceKernelOpen(const char*,int,mode_t);
int sceKernelClose(int);
}
static_assert(sizeof(PS5_PadData)==120);
static_assert(offsetof(PS5_PadData,timestamp)==80);
namespace {
using ps5::demo::Canvas;
using ps5::demo::Color;
botty::Model model;
botty::InputEvents input;
bool discardPadBatch=false;
botty::Network network;
botty::Artwork artwork;
botty::ArtworkPage covers;
botty::Connection connection;
botty::Catalog catalog;
botty::Workflow workflow;
botty::ActionResult actionResult;
bool showResult=false;
botty::Network::Deletion deletion=botty::Network::Deletion::idle;
std::array<char,512> deletionName{};
std::uint64_t deletionStarted=0;
double deletionBytes=0;
bool deletingLibrary=false;
std::uint64_t displayRevision=0;
int pad=-1;
bool connected=false;
std::uint64_t retryPad=0;
unsigned currentButtons=0;
constexpr Color rgb(unsigned r,unsigned g,unsigned b) { return static_cast<Color>(0xff000000U|r|(g<<8)|(b<<16)); }
constexpr Color background=rgb(9,13,27), card=rgb(18,25,44);
constexpr Color muted=rgb(165,181,207), ink=rgb(244,247,255), accent=rgb(111,231,255);
constexpr Color border=rgb(47,62,91), selected=rgb(29,48,73);
constexpr Color coral=rgb(255,146,119), lemon=rgb(241,226,132);
constexpr Color violet=rgb(171,148,255), success=rgb(123,239,190), warning=rgb(255,191,119);
void shortLabel(Canvas&,unsigned,unsigned,std::string_view,unsigned,unsigned,Color) noexcept;
void surface(Canvas& c,unsigned x,unsigned y,unsigned w,unsigned h,bool focus=false) noexcept {
    if(focus){c.rounded(x-4,y-4,w+8,h+8,20,rgb(33,82,108));c.rounded(x-2,y-2,w+4,h+4,18,accent);}
    c.rounded(x,y,w,h,16,focus?selected:card);
}
Color sectionColor() noexcept {
    constexpr Color colors[]={coral,lemon,success,accent,violet,accent};return colors[model.tab];
}
void titleLines(Canvas& c,unsigned x,unsigned y,std::string_view value,unsigned size,unsigned width,unsigned lines,Color color) noexcept {
    for(unsigned line=0;line<lines&&!value.empty();++line){
        auto n=value.size();while(n&&c.text_width(botty::slice(value,0,n),size)>width)n--;
        if(n<value.size()){
            auto word=n;while(word&&value[word]!=' ')--word;if(word)n=word;
        }
        if(!n)return;
        if(line+1==lines){shortLabel(c,x,y+line*(size+8),value,size,width,color);return;}
        c.label(x,y+line*(size+8),botty::slice(value,0,n),size,color);value.remove_prefix(n);
        while(!value.empty()&&value.front()==' ')value.remove_prefix(1);
    }
}
void gamePattern(Canvas& c,unsigned x,unsigned y,unsigned w,unsigned h,unsigned seed) noexcept {
    constexpr Color tops[]={rgb(67,44,99),rgb(31,75,90),rgb(99,49,54),rgb(41,68,107),rgb(67,77,40),rgb(87,46,93)};
    constexpr Color lights[]={violet,accent,coral,rgb(123,175,255),lemon,rgb(247,148,210)};
    c.gradient(x,y,w,h,tops[seed%6],background);
    const unsigned radius=std::min(w,h)/3;
    c.circle(x+w*3/5,y+h*2/5,radius,lights[seed%6]);
    c.circle(x+w*3/5-radius/4,y+h*2/5-radius/5,radius-3,tops[seed%6]);
    for(unsigned i=0;i<5;++i){const unsigned yy=y+h*2/3+i*8;if(yy<y+h)c.rectangle(x+w/10,yy,w*4/5,1,lights[seed%6]);}
}
void heading(Canvas& c,const char* title,const char* subtitle) noexcept {
    c.label(96,235,title,60,ink);c.label(98,311,subtitle,24,muted);
}
void progressBar(Canvas& c,unsigned x,unsigned y,unsigned width,double value,Color color) noexcept {
    c.rounded(x,y,width,6,3,border);
    const unsigned filled=static_cast<unsigned>(width*std::clamp(value,0.0,1.0));
    if(filled)c.rounded(x,y,filled,6,3,color);
}
void downloadIcon(Canvas& c,unsigned x,unsigned y,Color color) noexcept {
    c.rounded(x+20,y,8,29,4,color);
    for(unsigned row=0;row<14;++row)c.rectangle(x+10+row,y+23+row,28-row*2,1,color);
    c.rounded(x+2,y+43,44,5,2,color);
}
void statusDot(Canvas& c,unsigned x,unsigned y,Color color) noexcept {c.rounded(x,y,12,12,6,color);}
void key(Canvas& c,unsigned x,unsigned y,const char* label,unsigned width=36) noexcept {
    c.rounded(x,y,width,34,10,border);c.label(x+11,y+3,label,20,ink);
}
void openPad() noexcept {
    int user=-1;
    if(sceUserServiceGetInitialUser(&user)==0)
        pad=scePadOpen(user,PS5_PAD_PORT_TYPE_STANDARD,0,nullptr);
    botty::platform::log(pad>=0?"Controller handle opened":"Controller not available");
}
unsigned pollPad(std::uint64_t now) noexcept {
    if(pad<0&&now>=retryPad) {openPad();retryPad=now+2000000;}
    if(pad<0){input.reset();return 0;}
    std::array<PS5_PadData,64> records{};
    const int count=scePadRead(pad,records.data(),static_cast<int>(records.size()));
    if(count<0) {
        (void)scePadClose(pad);pad=-1;connected=false;currentButtons=0;input.reset();
        return 0;
    }
    if(count>0&&count<=static_cast<int>(records.size())) {
        // Pad history may be newest-first. Replay every fresh packet in time order.
        for(int i=1;i<count;++i){auto item=records[i];int j=i;while(j>0&&records[j-1].timestamp>item.timestamp){records[j]=records[j-1];--j;}records[j]=item;}
        const int first=discardPadBatch?count-1:0;discardPadBatch=false;
        for(int i=first;i<count;++i){const auto& packet=records[i];
            if(packet.timestamp<=input.timestamp)continue;
            const bool next=packet.connected!=0;
            if(next!=connected)botty::platform::log(next?"Controller connected":"Controller disconnected");
            connected=next;currentButtons=packet.buttons;
            if(packet.leftStick.x<80)currentButtons|=botty::Buttons::left;
            if(packet.leftStick.x>176)currentButtons|=botty::Buttons::right;
            if(packet.leftStick.y<80)currentButtons|=botty::Buttons::up;
            if(packet.leftStick.y>176)currentButtons|=botty::Buttons::down;
            input.ingest(currentButtons,connected,packet.timestamp,now);
        }
    }
    return input.next(now);
}

void wrapped(Canvas& c,std::string_view text,unsigned& line,unsigned page,Color color) noexcept {
    while(!text.empty()) {
        unsigned n=text.size()>120?120:static_cast<unsigned>(text.size());
        const auto newline=text.find('\n');if(newline<n)n=static_cast<unsigned>(newline);
        while(n&&c.text_width(botty::slice(text,0,n),26)>1630)--n;
        if(n<text.size()) {while(n&&(static_cast<unsigned char>(text[n])&0xc0)==0x80)--n;}
        if(line>=page*12&&line<page*12+12)c.label(140,426+(line-page*12)*38,botty::slice(text,0,n),26,color);
        ++line;text.remove_prefix(n);
        if(!text.empty()&&text.front()=='\n')text.remove_prefix(1);
    }
}
void peerCount(int value,char* out,unsigned size) noexcept {
    if(value<0)std::snprintf(out,size,"Unknown");else std::snprintf(out,size,"%d",value);
}
void peers(const botty::Entry& e,char* out,unsigned size) noexcept {
    char count[24];peerCount(e.peers,count,sizeof(count));std::snprintf(out,size,e.peers<0?"Peers: %s":"Peers: %s connected",count);
}
std::array<char,96> artworkId(const botty::Entry& entry,unsigned tab) noexcept {
    if(tab==5)return entry.id;
    std::array<char,96> id{};
    std::snprintf(id.data(),id.size(),"%c:%.93s",tab==0?'t':tab==4?'s':'j',entry.id.data());return id;
}
bool hasCover(const botty::Entry& entry,unsigned tab,unsigned slot) noexcept {
    return slot<covers.ready.size()&&covers.ready[slot]&&covers.ids[slot]==artworkId(entry,tab);
}
void drawCatalog(Canvas& c) noexcept {
    char text[256],size[48],rate[48],upload[48],downloaded[48],eta[64];
    const char* titles[]={"Your downloads.","The preparation room.","Your collection."};
    const char* subtitles[]={"Track every download. Keep your next adventure moving.","Unpacking, verifying and preparing your games.","Your prepared games, together in one place."};
    c.label(96,238,titles[model.tab],60,ink);c.label(98,316,subtitles[model.tab],24,muted);
    botty::formatBytes(catalog.freeBytes,size,sizeof(size));
    if(catalog.valid&&model.tab!=2){c.rounded(1470,246,354,98,18,card);c.label(1500,258,size,32,sectionColor());c.label(1500,302,"FREE ON YOUR PS5",20,muted);}
    if(model.tab==2)(void)c.illustration(2,1520,220,208,208);
    if(model.tab==0) {
        const char* filters[]={"All","Active","Completed"};
        for(unsigned i=0;i<3;++i){c.rounded(96+i*184,356,168,40,12,model.filter==i?coral:card);c.label(116+i*184,361,filters[i],24,model.filter==i?background:muted);}
        c.label(674,365,"Left / right to filter",20,muted);
    }else c.label(98,361,model.tab==1?"EXTRACTION QUEUE":"YOUR COLLECTION",20,violet);
    if(!catalog.valid){
        surface(c,96,436,1728,340);c.label(140,485,connection.status==botty::Probe::checking?"Syncing your downloads":"Your session is offline",44,ink);
        c.label(140,565,connection.status==botty::Probe::checking?"Connecting to Botty on your PS5...":"Open the Botty portal and select Start session.",28,muted);
        c.label(140,667,"Triangle  /  Retry connection",24,accent);return;
    }
    if(deletion!=botty::Network::Deletion::idle)c.label(1040,365,"Waiting for deletion to finish...",22,accent);
    else if(catalog.stale)c.label(1040,365,"Last update shown. Reconnecting...",22,warning);
    else if(model.tab==0&&!catalog.transmissionReady)c.label(1040,365,"rTorrent not responding. Retrying...",22,warning);
    const auto* entry=botty::entryAt(catalog,model.tab,model.filter,model.selected);
    if(model.details&&entry) {
        surface(c,96,408,1728,506);unsigned line=0;
        wrapped(c,entry->name.data(),line,model.detailPage,ink);
        wrapped(c,entry->status.data(),line,model.detailPage,accent);
        botty::formatBytes(entry->total,size,sizeof(size));botty::formatBytes(entry->download,rate,sizeof(rate));botty::formatBytes(entry->upload,upload,sizeof(upload));
        std::snprintf(text,sizeof(text),"%.0f%%  /  %s",entry->progress*100,size);wrapped(c,text,line,model.detailPage,muted);
        if(model.tab==0){
            botty::formatBytes(entry->bytes,downloaded,sizeof(downloaded));botty::formatETA(*entry,eta,sizeof(eta));
            std::snprintf(text,sizeof(text),"Downloaded %s of %s  /  %s",downloaded,size,eta);wrapped(c,text,line,model.detailPage,ink);
            std::snprintf(text,sizeof(text),"Download: %s/s    Upload: %s/s",rate,upload);wrapped(c,text,line,model.detailPage,muted);
            peers(*entry,text,sizeof(text));wrapped(c,text,line,model.detailPage,ink);
            char incoming[24],outgoing[24];peerCount(entry->downloadingPeers,incoming,sizeof(incoming));peerCount(entry->uploadingPeers,outgoing,sizeof(outgoing));
            std::snprintf(text,sizeof(text),"Downloading from %s peers  /  Uploading to %s peers",incoming,outgoing);wrapped(c,text,line,model.detailPage,muted);
            if(entry->peers<0)wrapped(c,"Update Botty service to 0.1.2 to show peer counts.",line,model.detailPage,muted);
        }
        if(model.tab!=0){
            botty::formatBytes(entry->bytes,downloaded,sizeof(downloaded));
            std::snprintf(text,sizeof(text),"Extracted %s of %s",downloaded,size);wrapped(c,text,line,model.detailPage,ink);
            if(entry->active){botty::formatETA(*entry,eta,sizeof(eta));std::snprintf(text,sizeof(text),"Extraction: %s/s  /  %s",rate,eta);wrapped(c,text,line,model.detailPage,ink);}
        }
        if(model.tab==2&&entry->compressed)wrapped(c,std::string_view(entry->compressionState.data())!="ready"?"Compression pending - check details":entry->originalKept?"Compressed copy - original retained":"Compressed copy - original deleted",line,model.detailPage,accent);
        if(model.tab==2&&std::string_view(catalog.compressionJob.data())==entry->id.data()) {
            wrapped(c,"Library compression",line,model.detailPage,accent);
            wrapped(c,catalog.compressionPhase.data(),line,model.detailPage,ink);
            wrapped(c,catalog.compressionError.data(),line,model.detailPage,warning);
            if(catalog.compressionBusy&&catalog.compressionTotal>0){
                botty::formatBytes(catalog.compressionBytes,downloaded,sizeof(downloaded));botty::formatBytes(catalog.compressionTotal,size,sizeof(size));
                std::snprintf(text,sizeof(text),"Compression: %s / %s",downloaded,size);wrapped(c,text,line,model.detailPage,ink);
            }
            if(catalog.compressedSize>0){botty::formatBytes(catalog.compressedSize,size,sizeof(size));std::snprintf(text,sizeof(text),"Compressed copy: %s",size);wrapped(c,text,line,model.detailPage,ink);}
        }
        wrapped(c,entry->phase.data(),line,model.detailPage,muted);
        if(entry->kind[0])wrapped(c,entry->kind.data(),line,model.detailPage,accent);
        if(entry->destination[0]){wrapped(c,"Destination",line,model.detailPage,muted);wrapped(c,entry->destination.data(),line,model.detailPage,ink);}
        wrapped(c,entry->error.data(),line,model.detailPage,accent);
        if(model.tab==2){wrapped(c,"Library folder",line,model.detailPage,muted);wrapped(c,catalog.library.data(),line,model.detailPage,ink);}
        if(model.tab==0&&entry->fileCount){std::snprintf(text,sizeof(text),"Files (%u) - long lists are abbreviated",entry->fileCount);wrapped(c,text,line,model.detailPage,muted);wrapped(c,entry->files.data(),line,model.detailPage,ink);}
        const unsigned pages=line?(line-1)/12+1:1;
        if(model.detailPage>=pages){model.detailPage=pages-1;++displayRevision;}
        std::snprintf(text,sizeof(text),"Page %u / %u  -  Up / down to scroll",model.detailPage+1,pages);c.label(96,932,text,22,muted);
    }else if(!model.count) {
        surface(c,96,430,1728,400);downloadIcon(c,144,486,accent);
        c.label(228,476,model.tab==0?"A fresh start.":model.tab==1?"Nothing to unpack yet.":"Your next game belongs here.",44,ink);
        c.label(228,550,model.tab==0?"Add a magnet with Square, or browse Search and Explore.":"Completed extraction results will appear here.",28,muted);
        c.label(228,674,"L1 / R1  /  Browse tabs",24,accent);
    }else if(model.tab==2){
        const unsigned first=(model.selected/6)*6;
        for(unsigned i=first;i<model.count&&i<first+6;++i){
            const auto* e=botty::entryAt(catalog,2,0,i);const unsigned slot=i-first;
            const unsigned x=96+(slot%3)*584,y=444+(slot/3)*250;const bool focus=i==model.selected;
            surface(c,x,y,560,232,focus);
            if(hasCover(*e,2,slot))c.gameCase(x+12,y+10,140,210,covers.pixels[slot]);
            else {gamePattern(c,x+12,y+10,140,210,i);c.label(x+32,y+24,"PS5",28,ink);c.rectangle(x+12,y+156,140,64,background);c.label(x+22,y+173,"No artwork",20,muted);}
            const auto status=std::string_view(e->status.data());
            c.rounded(x+176,y+18,174,32,9,focus?success:background);
            c.label(x+190,y+23,status=="moved"?"IN LIBRARY":status=="ready"?"PREPARED":"PREPARING",20,focus?background:success);
            titleLines(c,x+176,y+68,e->name.data(),28,360,3,ink);
            c.label(x+176,y+196,focus?"Cross: Details":"Verified content",20,focus?accent:muted);
        }
        std::snprintf(text,sizeof(text),"%u games  /  Page %u  /  Arrows to browse",model.count,first/6+1);c.label(96,946,text,20,muted);
    }else {
        const unsigned first=(model.selected/5)*5;unsigned y=418;
        for(unsigned row=0;row<5&&first+row<model.count;++row){
            const auto* e=botty::entryAt(catalog,model.tab,model.filter,first+row);const bool focus=model.selected==first+row;
            const auto status=std::string_view(e->status.data());const bool done=e->complete||status=="ready"||status=="moved";
            const Color tone=e->error[0]?warning:done?success:sectionColor();
            botty::formatBytes(e->total,size,sizeof(size));botty::formatBytes(e->download,rate,sizeof(rate));
            botty::formatBytes(e->bytes,downloaded,sizeof(downloaded));botty::formatETA(*e,eta,sizeof(eta));
            if(focus){
                c.rounded(92,y-4,1144,184,24,ink);c.rounded(96,y,1136,176,20,tone);
                const unsigned textX=hasCover(*e,model.tab,row)?244:126;
                if(hasCover(*e,model.tab,row))c.gameCase(112,y+12,108,152,covers.pixels[row]);
                c.label(textX,y+16,e->error[0]?"NEEDS ATTENTION":e->status.data(),20,background);
                shortLabel(c,textX,y+47,e->name.data(),32,1036-textX,background);
                std::snprintf(text,sizeof(text),"%.0f%%",e->progress*100);c.label(1060,y+26,text,44,background);
                std::snprintf(text,sizeof(text),"%s of %s",downloaded,size);c.label(textX,y+104,text,24,background);
                if(e->active&&!done)std::snprintf(text,sizeof(text),"%s/s  /  %s",rate,eta);
                else if(model.tab==0)peers(*e,text,sizeof(text));else std::snprintf(text,sizeof(text),"%s",done?"Verified and ready":"Options for actions");
                shortLabel(c,618,y+104,text,24,578,background);
                const unsigned barWidth=1198-textX;
                c.rounded(textX,y+153,barWidth,7,3,rgb(77,76,79));
                const unsigned filled=static_cast<unsigned>(barWidth*std::clamp(e->progress,0.0,1.0));
                if(filled)c.rounded(textX,y+153,filled,7,3,background);
                y+=188;
            }else {
                c.rounded(96,y,1136,72,14,card);c.rounded(116,y+20,6,30,3,tone);
                shortLabel(c,146,y+5,e->name.data(),28,880,ink);
                std::snprintf(text,sizeof(text),"%s  /  %s of %s",e->error[0]?"Needs attention":e->status.data(),downloaded,size);shortLabel(c,146,y+40,text,20,860,muted);
                std::snprintf(text,sizeof(text),"%.0f%%",e->progress*100);c.label(1100,y+10,text,28,tone);
                progressBar(c,1040,y+52,160,e->progress,tone);y+=84;
            }
        }
        (void)c.illustration(model.tab,1290,350,520,520);
        c.label(1300,841,model.tab==0?"Keep the queue moving.":"From archive to adventure.",28,sectionColor());
        unsigned active=0;double throughput=0;
        for(unsigned i=0;i<botty::entryCount(catalog,model.tab,0);++i){const auto* e=botty::entryAt(catalog,model.tab,0,i);if(e->active&&!e->complete){++active;throughput+=e->download;}}
        botty::formatBytes(throughput,rate,sizeof(rate));std::snprintf(text,sizeof(text),"%u active  /  %s/s",active,rate);c.label(1300,886,text,24,ink);
        c.label(1300,930,model.tab==0?"Continues when you close Botty+.":"Original archives are preserved.",20,muted);
        std::snprintf(text,sizeof(text),"%u of %u  /  Up / down to browse",model.selected+1,model.count);c.label(96,950,text,20,muted);
    }
    if(catalog.truncated)c.label(500,932,"Showing the first 256 torrents and extraction jobs.",22,accent);
}
void shortLabel(Canvas& c,unsigned x,unsigned y,std::string_view text,unsigned size,unsigned width,Color color) noexcept {
    auto n=text.size();while(n&&c.text_width(botty::slice(text,0,n),size)>width-30)--n;
    c.label(x,y,botty::slice(text,0,n),size,color);if(n<text.size())c.label(x+width-28,y,"...",size,color);
}
void drawWorkflow(Canvas& c) noexcept {
    using Panel=botty::Workflow::Panel;using Op=botty::Operation;
    c.shade(210);surface(c,96,292,1728,654);c.rounded(140,300,64,4,2,accent);
    const auto* target=workflow.target(catalog);
    if(workflow.panel==Panel::menu){
        c.label(140,322,"Actions",40,ink);shortLabel(c,140,378,workflow.targetName.data(),26,1620,muted);
        for(unsigned i=0;i<workflow.optionCount;++i){auto op=workflow.options[i];const bool enabled=!*botty::unavailable(op,target,catalog);const bool focus=workflow.selected==i;
            surface(c,140,430+i*72,1640,60,focus);c.label(166,442+i*72,botty::operationLabel(op),28,enabled?(focus?accent:ink):muted);
            if(!enabled)c.label(1450,447+i*72,"Unavailable",22,muted);
        }
        const char* reason=workflow.notice[0]?workflow.notice.data():botty::unavailable(workflow.options[workflow.selected],target,catalog);
        shortLabel(c,140,838,reason,24,1640,accent);
        c.label(140,899,"Cross: Select    Circle: Cancel",22,muted);
    }else if(workflow.panel==Panel::archives){
        c.label(140,322,"Choose an archive",40,ink);
        if(target){const unsigned first=(workflow.archiveIndex/6)*6;
            for(unsigned i=first;i<target->archiveCount&&i<first+6;++i){const unsigned y=396+(i-first)*64;surface(c,140,y,1640,54,i==workflow.archiveIndex);shortLabel(c,166,y+10,catalog.archives[target->archiveStart+i].data(),24,1580,i==workflow.archiveIndex?accent:ink);}
            if(target->archivesOmitted)c.label(140,816,"Some archive names exceed the list limit.",22,accent);
        }
        shortLabel(c,140,853,workflow.notice.data(),24,1640,accent);c.label(140,901,"Up / down: Choose    Cross: Select    Circle: Cancel",22,muted);
    }else if(workflow.panel==Panel::keyboard){
        const bool password=workflow.command.operation==Op::extract;
        c.label(140,316,workflow.unicodeInput?"Unicode code point (hex)":password?"Archive password (optional)":workflow.command.operation==Op::search?"Search games":"Add a magnet link",40,ink);
        const auto text=std::string_view(workflow.unicodeInput?workflow.codepoint.data():workflow.command.text.data());std::array<char,128> visible{};
        if(password&&!workflow.passwordVisible&&!workflow.unicodeInput){const unsigned n=text.size()>90?90:static_cast<unsigned>(text.size());for(unsigned i=0;i<n;++i)visible[i]='*';}
        else {auto tail=botty::slice(text,text.size()>90?text.size()-90:0);for(unsigned i=0;i<tail.size();++i)visible[i]=tail[i];}
        c.rounded(140,382,1640,76,12,background);shortLabel(c,162,402,visible.data(),26,1580,ink);
        char counter[100];std::snprintf(counter,sizeof(counter),"%zu / %u characters   %s",text.size(),workflow.unicodeInput?6:workflow.command.operation==Op::search?200:password?1024:16384,text.size()>90?"(showing end)":"");c.label(140,464,counter,20,muted);
        auto keys=botty::Workflow::keys(workflow.keyPage);
        for(unsigned i=0;i<40;++i){const unsigned x=140+(i%10)*164,y=508+(i/10)*60;const bool focus=workflow.selected==i;
            c.rounded(x,y,150,50,10,focus?accent:background);c.label(x+62,y+9,botty::slice(keys,i,1),28,focus?background:ink);}
        const char* controls[]={"Space","Backspace","Clear","ABC / Symbols","Done"};
        for(unsigned i=0;i<5;++i){const bool focus=workflow.selected>=40&&(workflow.selected-40)/2==i;const unsigned x=140+i*328;
            c.rounded(x,758,314,60,12,focus?accent:background);c.label(x+24,776,controls[i],24,focus?background:ink);}
        shortLabel(c,140,836,workflow.notice[0]?workflow.notice.data():password&&!workflow.command.text[0]?"No password? Select Done to continue.":"",24,1640,accent);
        c.label(140,895,password?"L1/R1: Keys   Square: Backspace   Triangle: Show/hide   Options: Unicode   Circle: Cancel":"L1/R1: Keys   Square: Backspace   Options: Unicode   Circle: Cancel",22,muted);
    }else if(workflow.panel==Panel::confirm){
        char title[128];std::snprintf(title,sizeof(title),"%s?",botty::operationLabel(workflow.command.operation));c.label(140,322,title,40,ink);
        shortLabel(c,140,392,workflow.command.operation==Op::add?workflow.command.text.data():workflow.targetName.data(),28,1640,ink);
        const char* explanation="This request will be sent to rTorrent.";
        if(workflow.command.operation==Op::grab||workflow.command.operation==Op::exploreGrab)explanation="Download, extract and prepare in Library automatically. Original torrents are kept for seeding.";
        if(workflow.command.operation==Op::compress)explanation="Create a compressed copy and keep the original. Close Botty+ when prompted to finish mounting and verification. APR games require an existing index.";
        if(workflow.command.operation==Op::restoreOriginal)explanation="Restore the retained original as the playable game. The compressed image and archives are kept. Close Botty+ to finish.";
        if(workflow.command.operation==Op::removeOriginal)explanation="Confirm you have tested the compressed game. Delete only its uncompressed backup after another full verification. The compressed game, saves and archives are kept. Close Botty+ to finish.";
        if(workflow.command.operation==Op::cancelCompression)explanation="Request cancellation and keep the original game. Wait until the compression worker stops.";
        if(workflow.command.operation==Op::removeLibrary)explanation="Permanently delete the installed game files. Torrent and archives are kept. Close the game and remove it from the home screen first.";
        if(workflow.command.operation==Op::removeTorrent)explanation="Permanently delete this torrent and its downloaded files, including archives. Library games are kept.";
        if(workflow.command.operation==Op::verify)explanation="rTorrent will recheck downloaded pieces. Extraction waits until verification finishes.";
        if(workflow.command.operation==Op::extract){explanation="Extract on this PS5. Original archive volumes are kept for seeding.";shortLabel(c,140,448,workflow.command.archive.data(),24,1640,accent);}
        if(workflow.command.operation==Op::move){explanation="Move verified content. Existing files will not be replaced. ShadowMount may need a scan.";char destination[1100];std::snprintf(destination,sizeof(destination),"%s/%s",catalog.library.data(),target?target->destination.data():"");shortLabel(c,140,448,destination,24,1640,accent);}
        if(workflow.command.operation==Op::cancel)explanation="Stop extraction at the next safe point. Partial files and original archives are kept.";
        if(workflow.command.operation==Op::dismiss){const auto status=target?std::string_view(target->status.data()):std::string_view{};explanation=status=="failed"||status=="cancelled"||status=="interrupted"?"Remove this row and delete its partial files. Original downloads and archives are kept.":"Hide this row from Extracted. Files are kept; ready and moved content stays in Library.";}
        if(workflow.command.operation==Op::remove)explanation="Delete this extraction and any partial output. Original downloads and archives are kept.";
        shortLabel(c,140,532,explanation,24,1640,muted);
        if(workflow.command.operation==Op::removeLibrary||workflow.command.operation==Op::removeTorrent){
            char estimate[160];botty::formatDeletionEstimate(target?(workflow.command.operation==Op::removeLibrary?target->total:target->bytes):0,estimate,sizeof(estimate));
            c.label(140,602,estimate,24,muted);
            c.label(140,645,"Large games take time to remove. Please wait for confirmation.",24,muted);
            shortLabel(c,140,690,workflow.notice.data(),22,1640,accent);
        }else shortLabel(c,140,618,workflow.notice.data(),24,1640,accent);
        for(unsigned i=0;i<2;++i){const bool chosen=workflow.confirm==(i==1);c.rounded(140+i*840,742,800,80,16,chosen?accent:background);c.label(180+i*840,766,i==0?"Cancel":botty::operationLabel(workflow.command.operation),28,chosen?background:ink);}
        c.label(140,892,"Left / right: Choose    Cross: Confirm    Circle: Cancel",22,muted);
    }
}
bool draw(Canvas& c) noexcept {
    const auto now=botty::platform::now();
    if(c.take_resumed()) {
        input.reset();discardPadBatch=true;network.retry();
        botty::platform::log("VideoOut resumed - refreshing local service");
    }
    static bool exploreRequested=false,quietExplore=false,exploreRefresh=false,exploreVisited=false;
    static const char* exploreSorts[]={"seeders","completed","newest"};
    const unsigned oldTab=model.tab;
    const unsigned edge=pollPad(now);if(edge)++displayRevision;
    if(deletion!=botty::Network::Deletion::idle){/* Keep duplicate actions out while deleting. */}
    else if(showResult){if(edge&(botty::Buttons::cross|botty::Buttons::circle))showResult=false;}
    else if(workflow.panel!=botty::Workflow::Panel::closed){
        if(workflow.press(edge,catalog,network.busy())){
            if(!network.submit(workflow.command)){actionResult.status=botty::ActionResult::Status::failed;std::snprintf(actionResult.message.data(),actionResult.message.size(),"Network is busy or unavailable. Please try again.");showResult=true;}
            else if(workflow.command.operation==botty::Operation::removeTorrent||workflow.command.operation==botty::Operation::removeLibrary){deletingLibrary=workflow.command.operation==botty::Operation::removeLibrary;deletionName=workflow.targetName;deletionStarted=now;const auto* target=workflow.target(catalog);deletionBytes=target?(deletingLibrary?target->total:target->bytes):0;}
            workflow.command.text.fill(0);
        }
    }else {
        const auto action=model.press(edge);
        if(action==botty::Model::Action::retry)network.retry();
        if(action==botty::Model::Action::quit){if(!network.busy())return false;model.quitDialog=false;actionResult.status=botty::ActionResult::Status::failed;std::snprintf(actionResult.message.data(),actionResult.message.size(),"Wait for the pending request before quitting.");showResult=true;}
        if(model.tab==4&&!model.quitDialog){
            if(action==botty::Model::Action::add||action==botty::Model::Action::menu||action==botty::Model::Action::retry)workflow.search();
            if(model.details){model.details=false;if(model.selected<catalog.resultCount&&!catalog.results[model.selected].complete)workflow.grab(catalog.results[model.selected]);}
        }
        if(model.tab==5&&!model.quitDialog){
            if(action==botty::Model::Action::explore||action==botty::Model::Action::add||action==botty::Model::Action::retry||action==botty::Model::Action::menu){exploreRequested=true;exploreRefresh=action!=botty::Model::Action::explore;}
            if(model.details){model.details=false;if(!catalog.exploreBusy&&!catalog.exploreAdding&&std::string_view(catalog.exploreSort.data())==exploreSorts[model.exploreSort]&&model.selected<catalog.exploreCount)workflow.grab(catalog.exploreResults[model.selected],true);}
        }
        if(action==botty::Model::Action::menu&&model.tab<4)workflow.open(model.tab<3?botty::entryAt(catalog,model.tab,model.filter,model.selected):nullptr,model.tab,catalog);
        if(action==botty::Model::Action::add&&model.tab<4)workflow.add();
    }
    std::array<char,96> focused{};
    if(model.tab<3){const auto* old=botty::entryAt(catalog,model.tab,model.filter,model.selected);if(old)focused=old->id;}
    const auto previousRevision=catalog.revision;
    const auto resultRevision=actionResult.revision;
    botty::ActionResult receivedResult=actionResult;
    (void)network.read(connection,&catalog,&receivedResult);
    if(receivedResult.revision!=resultRevision){actionResult=receivedResult;showResult=!quietExplore||receivedResult.status!=botty::ActionResult::Status::success;quietExplore=false;++displayRevision;}
    static bool wasBusy=false;if(wasBusy!=network.busy()){wasBusy=network.busy();++displayRevision;}
    const auto nextDeletion=network.deletion();
    if(deletion!=nextDeletion){deletion=nextDeletion;++displayRevision;}
    static std::uint64_t deletionSecond=0;
    if(deletion!=botty::Network::Deletion::idle&&(now-deletionStarted)/1000000!=deletionSecond){deletionSecond=(now-deletionStarted)/1000000;++displayRevision;}
    if(previousRevision!=catalog.revision)++displayRevision;
    if(model.tab<3) {
        model.count=catalog.valid?botty::entryCount(catalog,model.tab,model.filter):0;
        if(previousRevision!=catalog.revision&&focused[0]) {
            bool found=false;
            for(unsigned i=0;i<model.count;++i)if(std::string_view(botty::entryAt(catalog,model.tab,model.filter,i)->id.data())==focused.data()){model.selected=i;found=true;break;}
            if(!found)model.details=false;
        }
        if(model.selected>=model.count)model.selected=model.count?model.count-1:0;
    }
    if(model.tab==4){model.count=catalog.resultCount;if(model.selected>=model.count)model.selected=model.count?model.count-1:0;}
    if(model.tab==5){
        if((!exploreVisited||oldTab!=5)&&!catalog.exploreSort[0])exploreRequested=true;
        exploreVisited=true;
        if(exploreRequested&&catalog.valid&&catalog.exploreSupported&&!catalog.exploreBusy&&!catalog.exploreAdding&&!network.busy()&&workflow.panel==botty::Workflow::Panel::closed&&!showResult){
            botty::Command command;command.operation=botty::Operation::explore;command.refresh=exploreRefresh;std::snprintf(command.text.data(),command.text.size(),"%s",exploreSorts[model.exploreSort]);
            if(network.submit(command)){exploreRequested=false;quietExplore=true;}
        }
        model.count=std::string_view(catalog.exploreSort.data())==exploreSorts[model.exploreSort]?catalog.exploreCount:0;
        if(model.selected>=model.count)model.selected=model.count?model.count-1:0;
    }
    {
        botty::CoverIds ids{};
        if(model.tab==5){const unsigned first=(model.selected/6)*6;for(unsigned i=0;i<6&&first+i<model.count;++i)ids[i]=catalog.exploreResults[first+i].id;}
        else if(catalog.catalogArtworkSupported&&model.tab!=3){
            const unsigned perPage=model.tab<2?5:6,first=(model.selected/perPage)*perPage;
            for(unsigned i=0;i<perPage&&first+i<model.count;++i){
                const auto* entry=model.tab==4?&catalog.results[first+i]:botty::entryAt(catalog,model.tab,model.filter,first+i);
                if(entry)ids[i]=artworkId(*entry,model.tab);
            }
        }
        artwork.request(ids);if(artwork.read(covers))++displayRevision;
#ifdef BOTTY_HOST_PREVIEW
        // Explicit cover fixtures belong only to the matching real game title.
        for(unsigned i=0;i<6&&ids[i][0];++i){
            const unsigned perPage=model.tab<2?5:6,index=(model.selected/perPage)*perPage+i;
            const auto* entry=model.tab==5?&catalog.exploreResults[index]:model.tab==4?&catalog.results[index]:botty::entryAt(catalog,model.tab,model.filter,index);
            if(entry&&std::string_view(entry->name.data())=="Elden Ring - PS5"&&covers.ids[i]!=ids[i]){
                FILE* file=std::fopen("build/preview-cover.rgb","rb");if(file){covers.ready[i]=std::fread(covers.pixels[i].data(),1,covers.pixels[i].size(),file)==covers.pixels[i].size();std::fclose(file);covers.ids[i]=ids[i];}
            }
        }
#endif
    }
    static auto previousPanel=botty::Workflow::Panel::closed;
    if(previousPanel!=workflow.panel){previousPanel=workflow.panel;
        const char* panels[]={"Workflow: closed","Workflow: menu","Workflow: archive chooser","Workflow: keyboard","Workflow: confirmation"};
        botty::platform::log(panels[static_cast<unsigned>(previousPanel)]);
    }
    const auto status=connection.status;
    if(!c.needs_update(displayRevision))return true;
    const bool online=status==botty::Probe::ready;
    const char* state=deletion!=botty::Network::Deletion::idle?"Deleting files":catalog.stale?"Reconnecting":online?"Connected":status==botty::Probe::checking?"Connecting":
        status==botty::Probe::legacy?"Update required":status==botty::Probe::transmissionUnavailable?"Reconnecting":"Offline";
    const char* detail=status==botty::Probe::legacy?"Update Botty from the portal to show your login details.":
        status==botty::Probe::unavailable?"Open the Botty portal and select Start session.":
        status==botty::Probe::transmissionUnavailable?"rTorrent is not responding. Retrying automatically.":
        status==botty::Probe::rejected?"Access rejected. Retry to reconnect.":
        status==botty::Probe::incompatible?"Update Botty from the portal.":
        status==botty::Probe::workerError?"Close and reopen Botty.":
        status==botty::Probe::malformed?"Invalid response from Botty. Retry to reconnect.":"";
    c.backdrop(model.tab!=5);
    c.rounded(96,64,58,58,17,coral);c.label(104,73,"B+",28,background);
    c.label(174,62,"Botty+",44,ink);c.rounded(346,80,56,28,8,border);c.label(356,80,"1.2",20,ink);
    c.rounded(1488,69,336,48,24,card);
    statusDot(c,1510,87,online?success:warning);c.label(1536,77,state,24,online?success:warning);
    const char* tabs[]={"Downloads","Extracted","Library","Connections","Search","Explore"};
    c.rectangle(96,209,1728,1,border);
    for(unsigned i=0;i<6;++i){const unsigned x=96+i*288,tab=botty::Model::tabOrder[i];
        if(model.tab==tab)c.rounded(x,153,272,52,14,sectionColor());
        c.label(x+22,162,tabs[tab],28,model.tab==tab?background:muted);
    }
    if(model.tab<3)drawCatalog(c);
    else if(model.tab==4){
        c.label(96,246,"Find something",60,ink);c.label(96,318,"worth playing.",60,violet);
        c.rounded(96,428,518,500,24,violet);
        c.label(128,460,"YOUR SEARCH",20,background);
        titleLines(c,128,510,catalog.searchQuery[0]?catalog.searchQuery.data():"What are you playing next?",44,446,3,background);
        c.rounded(128,728,454,70,16,background);c.label(160,750,"Square: New search",28,ink);
        c.label(128,830,"Download and prepare",24,background);c.label(128,866,"with a single confirmation.",24,background);
        const unsigned first=(model.selected/6)*6;
        char count[100];std::snprintf(count,sizeof(count),"%u RESULTS  /  MOST SEEDED FIRST",catalog.resultCount);c.label(672,260,count,24,muted);
        for(unsigned i=first;i<catalog.resultCount&&i<first+6;++i){const auto& e=catalog.results[i];const unsigned y=330+(i-first)*96;const bool focus=i==model.selected;
            surface(c,670,y,1154,84,focus);
            if(hasCover(e,4,i-first))c.gameCase(684,y+7,44,66,covers.pixels[i-first]);
            shortLabel(c,748,y+10,e.name.data(),28,922,ink);
            char line[160],size[48];botty::formatBytes(e.total,size,sizeof(size));std::snprintf(line,sizeof(line),"%s   /   %d seeds   /   %d peers",size,e.peers,e.downloadingPeers);c.label(748,y+49,line,20,muted);
            c.label(1720,y+48,e.complete?"ADDED":"GET",20,e.complete?success:accent);
        }
        if(!catalog.resultCount){c.label(710,420,catalog.searchBusy?"Searching the catalog...":"The next adventure awaits.",32,ink);c.label(710,480,"Enter a game name with Square.",24,muted);}
        const char* state=!catalog.searchSupported?"Update the Botty service to enable search.":catalog.searchBusy?"Searching IPTorrents...":catalog.searchAdding?"Adding torrent...":catalog.searchError[0]?catalog.searchError.data():catalog.searchNotice[0]?catalog.searchNotice.data():catalog.searchQuery[0]&&!catalog.resultCount?"No results. Try another name.":"IPTorrents / Console/PS3  -  Cross: Download and prepare";
        shortLabel(c,670,939,state,20,1154,muted);
    }else if(model.tab==5){
        const unsigned first=(model.selected/6)*6,slot=model.selected-first;
        const char* sorts[]={"Most seeded","Most completed","Newest"};
        c.label(96,248,"DISCOVER  /  PS5",24,coral);
        if(model.count){
            const auto& e=catalog.exploreResults[model.selected];
            titleLines(c,92,288,e.name.data(),60,1140,2,ink);
            char line[128],size[48];botty::formatBytes(e.total,size,sizeof(size));
            std::snprintf(line,sizeof(line),"%s    /    %d seeds    /    %d completed",size,e.peers,e.completedCount);c.label(98,439,line,24,muted);
            c.rounded(96,492,380,64,18,coral);key(c,114,507,"X");c.label(166,506,"Get this game",28,background);
            c.label(506,500,"Download. Unpack. Prepare.",24,ink);c.label(506,531,"Botty handles the queue for you.",20,muted);
            // The selected cover owns the right side; the shelf remains navigable.
            c.rounded(1392,246,368,544,18,border);
            if(covers.ready[slot]&&covers.ids[slot]==e.id)c.gameCase(1400,254,352,528,covers.pixels[slot]);
            else {gamePattern(c,1400,254,352,528,model.selected);c.label(1440,300,"PS5",44,ink);titleLines(c,1430,600,e.name.data(),28,288,3,ink);}

            c.label(1392,812,"THE NEXT ONE IS YOURS.",24,coral);
            c.label(1392,858,"01  Download",24,ink);c.label(1392,890,"02  Extract & verify",24,muted);c.label(1392,922,"03  Ready in Library",24,muted);
        }else {
            c.label(96,296,"Find your next",60,ink);c.label(96,364,"great adventure.",60,coral);
        }
        for(unsigned i=0;i<3;++i){const unsigned x=96+i*252;c.rounded(x,585,236,42,12,model.exploreSort==i?ink:card);c.label(x+16,591,sorts[i],24,model.exploreSort==i?background:muted);}
        c.label(870,596,"Triangle: Sort",20,muted);
        for(unsigned i=first;i<model.count&&i<first+6;++i){const auto& e=catalog.exploreResults[i];const unsigned at=i-first,x=96+at*202,y=i==model.selected?646:656;const bool focus=i==model.selected;
            c.rounded(x-4,y-4,180,266,14,focus?coral:border);
            if(covers.ready[at]&&covers.ids[at]==e.id)c.gameCase(x,y,172,258,covers.pixels[at]);
            else {gamePattern(c,x,y,172,258,i);c.label(x+14,y+16,"PS5",24,ink);c.rectangle(x,y+140,172,118,background);titleLines(c,x+14,y+146,e.name.data(),20,146,3,ink);}
            if(focus)c.rounded(x+62,y+268,48,4,2,coral);
        }
        const char* state=!catalog.exploreSupported?"Update the Botty service to enable Explore.":catalog.exploreAdding?"Adding torrent...":catalog.exploreBusy||exploreRequested||quietExplore?(model.count?"Refreshing PS5 games...":"Loading PS5 games..."):catalog.exploreError[0]?catalog.exploreError.data():!model.count?"No new PS5 games found in this selection.":"";
        if(!model.count){surface(c,96,656,1200,258);shortLabel(c,136,696,state,28,1120,accent);c.label(136,770,"Square: Refresh   /   L1 or R1: Change tab",24,muted);}
        char footer[220];std::snprintf(footer,sizeof(footer),"%u games  /  Page %u  /  Artwork: Steam, Wikipedia, PlayStation",model.count,model.count?first/6+1:0);
        shortLabel(c,96,947,*state&&model.count?state:footer,20,1240,muted);
    }else {
        heading(c,"Connect to your console.","Manage downloads from your Mac, iPhone or any browser on your network.");
        c.rounded(96,386,1728,202,24,accent);c.label(136,413,"01  /  OPEN THIS ADDRESS",20,background);
        const char* url=online?(connection.url[0]?connection.url.data():"Network address unavailable"):status==botty::Probe::checking?"Connecting...":"Unavailable";
        shortLabel(c,136,463,url,60,1640,background);
        surface(c,96,610,842,172);surface(c,962,610,862,172);
        c.label(136,635,"02  /  USERNAME",20,muted);c.label(136,681,online?connection.username.data():"-",44,ink);
        c.label(1002,635,"03  /  PASSWORD",20,muted);
        const std::string_view password=connection.password.data();
        shortLabel(c,1002,681,online?password:std::string_view("-"),password.size()>6?28:44,770,accent);
        if(online&&password.size()>6)c.label(1002,745,"Short password applies next session.",20,muted);
        if(!online)shortLabel(c,96,805,detail,24,1728,warning);
        const unsigned xs[2]={96,578};
        for(unsigned i=0;i<2;++i){surface(c,xs[i],858,446,70,model.selected==i);c.label(xs[i]+32,877,i==0?"Retry connection":"Quit app",28,model.selected==i?accent:ink);}
    }
    c.rectangle(96,982,1728,1,border);
    key(c,96,1000,"X");c.label(146,1004,model.tab==5||model.tab==4?"Get game":model.details?"Select":"Open",20,ink);
    key(c,292,1000,"O");c.label(342,1004,"Back",20,muted);
    key(c,446,1000,"L1 / R1",108);c.label(566,1004,"Tabs",20,muted);
    c.label(720,1004,model.tab==5||model.tab==2?"Arrows: Browse":model.tab==4?"Square: Search":"Options: Actions",20,muted);
    c.label(1070,1004,model.tab==5?"Square: Refresh":model.tab==4?"Up / down: Browse":model.tab==2?"Options: Actions":model.tab==3?"Triangle: Retry":"Square: Add   Triangle: Refresh",20,muted);
    c.label(1620,1004,"01.002.000",20,muted);
    if(network.busy()&&deletion==botty::Network::Deletion::idle)c.label(1070,81,"Sending request...",24,accent);
    if(workflow.panel!=botty::Workflow::Panel::closed)drawWorkflow(c);
    if(deletion!=botty::Network::Deletion::idle){
        c.shade(210);surface(c,96,292,1728,654);c.rounded(140,300,64,4,2,accent);
        c.label(140,332,deletingLibrary?"Deleting game...":"Deleting torrent & files...",40,ink);
        shortLabel(c,140,420,deletionName.data(),28,1640,ink);
        c.label(140,510,deletion==botty::Network::Deletion::checking?"Waiting for completion. Large games may take several minutes.":deletingLibrary?"Removing installed game files. This may take a while.":"Removing downloaded files. Large downloads may take a while.",26,muted);
        c.label(140,568,deletingLibrary?"Torrent and archives are kept. Please wait.":"Library games are kept. Please wait before making another change.",26,muted);
        char estimate[160];botty::formatDeletionEstimate(deletionBytes,estimate,sizeof(estimate));c.label(140,616,estimate,24,muted);
        char elapsed[80];std::snprintf(elapsed,sizeof(elapsed),"Elapsed: %llu s",static_cast<unsigned long long>(deletionSecond));
        c.label(140,672,elapsed,26,accent);
        c.rounded(140,742,1640,8,4,border);c.rounded(140+(deletionSecond%8)*205,742,205,8,4,accent);
        c.label(140,838,"The list will refresh automatically when Botty responds.",24,muted);
    }
    if(showResult){
        c.shade(210);surface(c,96,292,1728,654);c.rounded(140,300,64,4,2,accent);
        c.label(140,306,actionResult.status==botty::ActionResult::Status::success?"Request confirmed":actionResult.status==botty::ActionResult::Status::uncertain?"Check the current state":"Request failed",36,ink);
        unsigned line=0;wrapped(c,actionResult.message.data(),line,0,accent);c.label(140,884,"Cross / Circle: Continue",24,muted);
    }
    if(model.quitDialog) {
        c.shade(170);
        c.rounded(488,334,1120,408,30,border);
        c.rounded(490,336,1116,404,28,card);
        c.label(540,382,"Quit Botty+?",44,ink);
        c.label(540,459,"Downloads and extractions will continue.",24,muted);
        c.rounded(540,588,480,78,18,!model.confirmQuit?accent:selected);
        c.rounded(1050,588,504,78,18,model.confirmQuit?accent:selected);
        c.label(580,610,"Keep app open",28,!model.confirmQuit?background:ink);
        c.label(1090,610,"Quit app",28,model.confirmQuit?background:ink);
        c.label(540,695,"Left / right to choose. Cross to confirm. Circle to go back.",20,muted);
    }
    return true;
}
}
int main() {
#ifdef BOTTY_HOST_PREVIEW
    const char* previewMode=std::getenv("BOTTY_PREVIEW_MODE");
    model.tab=previewMode&&std::string_view(previewMode)=="explore"?5:previewMode&&std::string_view(previewMode)=="search"?4:0;
#endif
    // A fresh per-launch log stays bounded; no access to /data or credentials.
    const int fd=sceKernelOpen("/download0/botty-native-network.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0)(void)sceKernelClose(fd);
    botty::platform::log("Botty+ 01.002.000 - main entered");
    const int user=sceUserServiceInitialize(nullptr);
    botty::platform::log(user==0?"User service initialized":"User service initialization returned nonzero");
    const int padResult=scePadInit();
    botty::platform::log(padResult==0?"Pad initialized":"Pad initialization returned nonzero");
    botty::platform::log(ps5::demo::load_font()?"Manrope font loaded":"Font unavailable - bitmap fallback");
    (void)ps5::demo::load_backdrop();
    ps5::demo::load_illustrations();
    (void)network.start();
#ifndef BOTTY_HOST_PREVIEW
    artwork.start();
#endif
    ps5::demo::run(draw,"Botty+ ready");
    artwork.stop();
    network.stop();
    if(pad>=0)(void)scePadClose(pad);
    botty::platform::log("Resources released - requesting application exit");
    (void)sceSystemServiceLoadExec("exit",nullptr);
    // Returning from main is unsafe in this native runtime if exit is rejected.
    botty::platform::log("Exit returned - close application from PS menu");
    for(;;)botty::platform::sleep(1000000);
}
