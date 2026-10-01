// Botty Native preview. SPDX-License-Identifier: GPL-3.0-or-later
#include "renderer.hpp"
#include "model.hpp"
#include "probe.hpp"
#include "platform.hpp"
#include <array>
#include <cstddef>
#include <cstdio>
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
botty::Connection connection;
botty::Catalog catalog;
botty::Workflow workflow;
botty::ActionResult actionResult;
bool showResult=false;
std::uint64_t displayRevision=0;
int pad=-1;
bool connected=false;
std::uint64_t retryPad=0;
unsigned currentButtons=0;
constexpr Color rgb(unsigned r,unsigned g,unsigned b) { return static_cast<Color>(0xff000000U|r|(g<<8)|(b<<16)); }
constexpr Color background=rgb(12,17,24), card=rgb(24,33,45);
constexpr Color muted=rgb(148,163,184), ink=rgb(232,239,248), accent=rgb(156,237,197);
constexpr Color border=rgb(41,54,70), selected=rgb(32,47,60);
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
        if(line>=page*12&&line<page*12+12)c.label(140,354+(line-page*12)*38,botty::slice(text,0,n),26,color);
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
void drawCatalog(Canvas& c) noexcept {
    char text[256],size[48],rate[48],upload[48],downloaded[48],eta[64];
    botty::formatBytes(catalog.freeBytes,size,sizeof(size));
    std::snprintf(text,sizeof(text),"%s free",size);if(catalog.valid)c.label(1450,260,text,24,muted);
    if(model.tab==0) {
        const char* filters[]={"All","Active","Completed"};
        for(unsigned i=0;i<3;++i){c.rounded(96+i*210,252,190,46,12,model.filter==i?selected:background);c.label(115+i*210,258,filters[i],24,model.filter==i?accent:muted);}
    }else c.label(96,260,model.tab==1?"Extraction jobs":"Prepared and moved content",26,muted);
    if(!catalog.valid){c.label(96,365,connection.status==botty::Probe::checking?"Loading your downloads...":"Unable to load Botty data. Press Options to retry.",30,ink);return;}
    if(model.tab==0&&!catalog.transmissionReady){c.label(96,322,"Transmission offline - start it from the portal.",24,accent);}
    const auto* entry=botty::entryAt(catalog,model.tab,model.filter,model.selected);
    if(model.details&&entry) {
        c.rounded(96,332,1728,526,24,card);unsigned line=0;
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
        wrapped(c,entry->phase.data(),line,model.detailPage,muted);
        if(entry->kind[0])wrapped(c,entry->kind.data(),line,model.detailPage,accent);
        if(entry->destination[0]){wrapped(c,"Destination",line,model.detailPage,muted);wrapped(c,entry->destination.data(),line,model.detailPage,ink);}
        wrapped(c,entry->error.data(),line,model.detailPage,accent);
        if(model.tab==2){wrapped(c,"Library folder",line,model.detailPage,muted);wrapped(c,catalog.library.data(),line,model.detailPage,ink);}
        if(model.tab==0&&entry->fileCount){std::snprintf(text,sizeof(text),"Files (%u) - long lists are abbreviated",entry->fileCount);wrapped(c,text,line,model.detailPage,muted);wrapped(c,entry->files.data(),line,model.detailPage,ink);}
        const unsigned pages=line?(line-1)/12+1:1;
        if(model.detailPage>=pages){model.detailPage=pages-1;++displayRevision;}
        std::snprintf(text,sizeof(text),"Page %u / %u  -  Up / down to scroll",model.detailPage+1,pages);c.label(96,884,text,22,muted);
    }else if(!model.count) {
        c.label(96,390,model.tab==0?"No torrents in this view.":model.tab==1?"No extractions yet.":"No prepared or moved content yet.",34,ink);
        c.label(96,454,model.tab==0?"Press Square to add a magnet link.":"Completed extraction results will appear here.",26,muted);
    }else {
        const unsigned first=(model.selected/5)*5;
        for(unsigned row=0;row<5&&first+row<model.count;++row){const auto* e=botty::entryAt(catalog,model.tab,model.filter,first+row);const unsigned y=360+row*110;const bool focus=model.selected==first+row;
            c.rounded(96,y,1728,100,16,focus?selected:card);if(focus)c.rounded(96,y,6,100,3,accent);
            std::string_view name=e->name.data();unsigned length=static_cast<unsigned>(name.size());while(length&&c.text_width(botty::slice(name,0,length),28)>1570)--length;
            c.label(122,y+10,botty::slice(name,0,length),28,ink);if(length<name.size())c.label(1730,y+10,"...",28,muted);
            botty::formatBytes(e->total,size,sizeof(size));botty::formatBytes(e->download,rate,sizeof(rate));
            if(model.tab==0){botty::formatBytes(e->bytes,downloaded,sizeof(downloaded));botty::formatETA(*e,eta,sizeof(eta));
                std::snprintf(text,sizeof(text),"%s  /  %.0f%%  /  %s of %s  /  %s/s  /  %s",e->status.data(),e->progress*100,downloaded,size,rate,eta);
            }
            else {
                botty::formatBytes(e->bytes,downloaded,sizeof(downloaded));botty::formatETA(*e,eta,sizeof(eta));
                if(e->active)std::snprintf(text,sizeof(text),"Extracting  /  %.1f%%  /  %s of %s  /  %s/s  /  %s",e->progress*100,downloaded,size,rate,eta);
                else std::snprintf(text,sizeof(text),"%s  /  %.0f%%  /  %s of %s%s",e->status.data(),e->progress*100,downloaded,size,e->error[0]?"  /  Needs attention":"");
            }
            c.label(122,y+43,text,20,e->error[0]?accent:muted);
            if(model.tab==0){peers(*e,text,sizeof(text));c.label(122,y+68,text,20,muted);}
            c.rounded(1510,y+82,280,5,2,border);if(e->progress>0)c.rectangle(1510,y+82,static_cast<unsigned>(280*e->progress),5,accent);
        }
        std::snprintf(text,sizeof(text),"%u / %u",model.selected+1,model.count);c.label(96,932,text,22,muted);
    }
    if(catalog.truncated)c.label(500,932,"Showing the first 256 torrents and extraction jobs.",22,accent);
}
void shortLabel(Canvas& c,unsigned x,unsigned y,std::string_view text,unsigned size,unsigned width,Color color) noexcept {
    auto n=text.size();while(n&&c.text_width(botty::slice(text,0,n),size)>width-30)--n;
    c.label(x,y,botty::slice(text,0,n),size,color);if(n<text.size())c.label(x+width-28,y,"...",size,color);
}
void drawWorkflow(Canvas& c) noexcept {
    using Panel=botty::Workflow::Panel;using Op=botty::Operation;
    c.shade(180);c.rounded(96,292,1728,654,26,card);
    const auto* target=workflow.target(catalog);
    if(workflow.panel==Panel::menu){
        c.label(140,322,"Actions",40,ink);shortLabel(c,140,378,workflow.targetName.data(),26,1620,muted);
        for(unsigned i=0;i<workflow.optionCount;++i){auto op=workflow.options[i];const bool enabled=!*botty::unavailable(op,target,catalog);const bool focus=workflow.selected==i;
            c.rounded(140,430+i*72,1640,60,12,focus?selected:background);c.label(166,442+i*72,botty::operationLabel(op),28,enabled?(focus?accent:ink):muted);
            if(!enabled)c.label(1450,447+i*72,"Unavailable",22,muted);
        }
        const char* reason=workflow.notice[0]?workflow.notice.data():botty::unavailable(workflow.options[workflow.selected],target,catalog);
        shortLabel(c,140,838,reason,24,1640,accent);
        c.label(140,899,"Cross: Select    Circle: Cancel",22,muted);
    }else if(workflow.panel==Panel::archives){
        c.label(140,322,"Choose an archive",40,ink);
        if(target){const unsigned first=(workflow.archiveIndex/6)*6;
            for(unsigned i=first;i<target->archiveCount&&i<first+6;++i){const unsigned y=396+(i-first)*64;c.rounded(140,y,1640,54,12,i==workflow.archiveIndex?selected:background);shortLabel(c,166,y+10,catalog.archives[target->archiveStart+i].data(),24,1580,i==workflow.archiveIndex?accent:ink);}
            if(target->archivesOmitted)c.label(140,816,"Some archive names exceed the list limit.",22,accent);
        }
        shortLabel(c,140,853,workflow.notice.data(),24,1640,accent);c.label(140,901,"Up / down: Choose    Cross: Select    Circle: Cancel",22,muted);
    }else if(workflow.panel==Panel::keyboard){
        const bool password=workflow.command.operation==Op::extract;
        c.label(140,316,workflow.unicodeInput?"Unicode code point (hex)":password?"Archive password (optional)":"Add a magnet link",40,ink);
        const auto text=std::string_view(workflow.unicodeInput?workflow.codepoint.data():workflow.command.text.data());std::array<char,128> visible{};
        if(password&&!workflow.passwordVisible&&!workflow.unicodeInput){const unsigned n=text.size()>90?90:static_cast<unsigned>(text.size());for(unsigned i=0;i<n;++i)visible[i]='*';}
        else {auto tail=botty::slice(text,text.size()>90?text.size()-90:0);for(unsigned i=0;i<tail.size();++i)visible[i]=tail[i];}
        c.rounded(140,382,1640,76,12,background);shortLabel(c,162,402,visible.data(),26,1580,ink);
        char counter[100];std::snprintf(counter,sizeof(counter),"%zu / %u characters   %s",text.size(),workflow.unicodeInput?6:password?1024:16384,text.size()>90?"(showing end)":"");c.label(140,464,counter,20,muted);
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
        const char* explanation="This request will be sent to Transmission.";
        if(workflow.command.operation==Op::verify)explanation="Transmission will recheck downloaded pieces. Extraction waits until verification finishes.";
        if(workflow.command.operation==Op::extract){explanation="Extract on this PS5. Original archive volumes are kept for seeding.";shortLabel(c,140,448,workflow.command.archive.data(),24,1640,accent);}
        if(workflow.command.operation==Op::move){explanation="Move verified content. Existing files will not be replaced. ShadowMount may need a scan.";char destination[1100];std::snprintf(destination,sizeof(destination),"%s/%s",catalog.library.data(),target?target->destination.data():"");shortLabel(c,140,448,destination,24,1640,accent);}
        if(workflow.command.operation==Op::cancel)explanation="Stop extraction at the next safe point. Partial files and original archives are kept.";
        if(workflow.command.operation==Op::dismiss){const auto status=target?std::string_view(target->status.data()):std::string_view{};explanation=status=="failed"||status=="cancelled"||status=="interrupted"?"Remove this row and delete its partial files. Original downloads and archives are kept.":"Hide this row from Extracted. Files are kept; ready and moved content stays in Library.";}
        if(workflow.command.operation==Op::remove)explanation="Delete this extraction and any partial output. Original downloads and archives are kept.";
        shortLabel(c,140,532,explanation,24,1640,muted);
        shortLabel(c,140,618,workflow.notice.data(),24,1640,accent);
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
    const unsigned edge=pollPad(now);if(edge)++displayRevision;
    if(showResult){if(edge&(botty::Buttons::cross|botty::Buttons::circle))showResult=false;}
    else if(workflow.panel!=botty::Workflow::Panel::closed){
        if(workflow.press(edge,catalog,network.busy())){
            if(!network.submit(workflow.command)){actionResult.status=botty::ActionResult::Status::failed;std::snprintf(actionResult.message.data(),actionResult.message.size(),"Network is busy or unavailable. Please try again.");showResult=true;}
            workflow.command.text.fill(0);
        }
    }else {
        const auto action=model.press(edge);
        if(action==botty::Model::Action::retry)network.retry();
        if(action==botty::Model::Action::quit){if(!network.busy())return false;model.quitDialog=false;actionResult.status=botty::ActionResult::Status::failed;std::snprintf(actionResult.message.data(),actionResult.message.size(),"Wait for the pending request before quitting.");showResult=true;}
        if(action==botty::Model::Action::menu)workflow.open(model.tab<3?botty::entryAt(catalog,model.tab,model.filter,model.selected):nullptr,model.tab,catalog);
        if(action==botty::Model::Action::add)workflow.add();
    }
    std::array<char,96> focused{};
    if(model.tab<3){const auto* old=botty::entryAt(catalog,model.tab,model.filter,model.selected);if(old)focused=old->id;}
    const auto previousRevision=catalog.revision;
    const auto resultRevision=actionResult.revision;
    botty::ActionResult receivedResult=actionResult;
    (void)network.read(connection,&catalog,&receivedResult);
    if(receivedResult.revision!=resultRevision){actionResult=receivedResult;showResult=true;++displayRevision;}
    static bool wasBusy=false;if(wasBusy!=network.busy()){wasBusy=network.busy();++displayRevision;}
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
    static auto previousPanel=botty::Workflow::Panel::closed;
    if(previousPanel!=workflow.panel){previousPanel=workflow.panel;
        const char* panels[]={"Workflow: closed","Workflow: menu","Workflow: archive chooser","Workflow: keyboard","Workflow: confirmation"};
        botty::platform::log(panels[static_cast<unsigned>(previousPanel)]);
    }
    const auto status=connection.status;
    if(!c.needs_update(displayRevision))return true;
    const bool online=status==botty::Probe::ready;
    const char* state=online?"Connected":status==botty::Probe::checking?"Connecting":
        status==botty::Probe::legacy?"Update required":status==botty::Probe::transmissionUnavailable?"Transmission offline":"Offline";
    const char* detail=status==botty::Probe::legacy?"Update Botty from the portal to show your login details.":
        status==botty::Probe::unavailable?"Open the Botty portal and select Start session.":
        status==botty::Probe::transmissionUnavailable?"Start Transmission from the Botty portal.":
        status==botty::Probe::rejected?"Access rejected. Retry to reconnect.":
        status==botty::Probe::incompatible?"Update Botty from the portal.":
        status==botty::Probe::workerError?"Close and reopen Botty.":
        status==botty::Probe::malformed?"Invalid response from Botty. Retry to reconnect.":"";
    c.clear(background);
    c.rounded(96,70,64,64,19,accent);downloadIcon(c,104,78,background);
    c.label(180,69,"Botty",44,ink);
    statusDot(c,1438,96,online?accent:muted);c.label(1468,81,state,24,online?accent:muted);
    const char* tabs[]={"Torrents","Extracted","Library","Connections"};
    for(unsigned i=0;i<4;++i){c.rounded(96+i*432,166,408,58,15,model.tab==i?accent:card);c.label(124+i*432,177,tabs[i],28,model.tab==i?background:ink);}
    if(model.tab<3)drawCatalog(c);
    else {
    c.label(96,257,"Connect from Mac or iPhone",40,ink);
    c.rounded(96,339,1728,416,26,card);
    c.label(140,377,"Address",24,muted);
    const char* url=online?(connection.url[0]?connection.url.data():"Network address unavailable"):
        status==botty::Probe::checking?"Connecting...":"Unavailable";
    c.label(140,421,url,60,ink);
    c.rectangle(140,526,1640,1,border);
    c.label(140,563,"Username",24,muted);
    c.label(140,610,online?connection.username.data():"-",44,ink);
    c.label(960,563,"Password",24,muted);
    const std::string_view password=connection.password.data();
    c.label(960,610,online?password:std::string_view("-"),password.size()>6?28:44,accent);
    if(online && password.size()>6)c.label(960,681,"Short password applies next session.",20,muted);
    if(!online)c.label(96,784,detail,24,muted);
    const unsigned xs[2]={96,578};
    for(unsigned i=0;i<2;++i) {
        c.rounded(xs[i],850,446,80,20,model.selected==i?accent:card);
        c.label(xs[i]+32,872,i==0?"Retry connection":"Quit app",28,model.selected==i?background:ink);
    }
    }
    key(c,96,988,"X");c.label(146,993,"Select",20,muted);
    key(c,263,988,"O");c.label(313,993,"Back",20,muted);
    key(c,440,988,"L1 / R1",108);c.label(562,993,"Tabs",20,muted);
    c.label(710,993,"Options: Actions",20,muted);
    c.label(1030,993,"Square: Add   Triangle: Refresh",20,muted);
    c.label(1570,992,"00.003.002",20,muted);
    if(network.busy())c.label(420,90,"Sending request...",24,accent);
    if(workflow.panel!=botty::Workflow::Panel::closed)drawWorkflow(c);
    if(showResult){
        c.shade(180);c.rounded(96,292,1728,654,26,card);
        c.label(140,306,actionResult.status==botty::ActionResult::Status::success?"Request confirmed":actionResult.status==botty::ActionResult::Status::uncertain?"Check the current state":"Request failed",36,ink);
        unsigned line=1;wrapped(c,actionResult.message.data(),line,0,accent);c.label(140,884,"Cross / Circle: Continue",24,muted);
    }
    if(model.quitDialog) {
        c.shade(170);
        c.rounded(488,334,1120,408,30,border);
        c.rounded(490,336,1116,404,28,card);
        c.label(540,382,"Quit Botty?",44,ink);
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
    // A fresh per-launch log stays bounded; no access to /data or credentials.
    const int fd=sceKernelOpen("/download0/botty-native-network.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0)(void)sceKernelClose(fd);
    botty::platform::log("Botty Native 00.003.002 - main entered");
    const int user=sceUserServiceInitialize(nullptr);
    botty::platform::log(user==0?"User service initialized":"User service initialization returned nonzero");
    const int padResult=scePadInit();
    botty::platform::log(padResult==0?"Pad initialized":"Pad initialization returned nonzero");
    botty::platform::log(ps5::demo::load_font()?"Manrope font loaded":"Font unavailable - bitmap fallback");
    (void)network.start();
    ps5::demo::run(draw,"Botty Native preview ready");
    network.stop();
    if(pad>=0)(void)scePadClose(pad);
    botty::platform::log("Resources released - requesting application exit");
    (void)sceSystemServiceLoadExec("exit",nullptr);
    // Returning from main is unsafe in this native runtime if exit is rejected.
    botty::platform::log("Exit returned - close application from PS menu");
    for(;;)botty::platform::sleep(1000000);
}
