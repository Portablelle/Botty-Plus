// Botty+. SPDX-License-Identifier: GPL-3.0-or-later
#include "renderer.hpp"
#include "model.hpp"
#include "probe.hpp"
#include "platform.hpp"
#include "keyboard.hpp"
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
using Model=botty::Model;
using Op=botty::Operation;
using Panel=botty::Workflow::Panel;
using Row=botty::Workflow::Row;
botty::Model model;
botty::InputEvents input;
bool discardPadBatch=false;
botty::Network network;
botty::Artwork artwork;
botty::ArtworkPage covers;
botty::Connection connection;
botty::Catalog catalog;
botty::Workflow workflow;
// Lists the Details buttons with the same rules as Quick actions; never submits.
botty::Workflow detailActions;
botty::NativeKeyboard nativeKeyboard;
enum class TextEntryState { idle, editing, ready, accepted, capacity, fallback };
TextEntryState textEntryState=TextEntryState::idle;
botty::ActionResult actionResult;
// Success and notices expire; failures stay until Circle dismisses them.
struct Toast {
    enum class Kind { success, notice, error } kind=Kind::success;
    bool visible=false;
    std::uint64_t until=0;
    std::array<char,160> title{};
    std::array<char,512> message{};
    std::array<char,96> cover{};
} toast;
botty::Network::Deletion deletion=botty::Network::Deletion::idle;
std::uint64_t displayRevision=0;
int pad=-1;
bool connected=false;
std::uint64_t retryPad=0;
unsigned currentButtons=0;
// The request in flight, described for its toast.
Op sentOperation=Op::none;
std::array<char,512> sentName{};
std::array<char,96> sentCover{};
std::array<char,256> detailNotice{};
std::array<std::array<char,201>,3> recentSearches{};
constexpr Color rgb(unsigned r,unsigned g,unsigned b) { return static_cast<Color>(0xff000000U|r|(g<<8)|(b<<16)); }
// Paper tokens: velvet, seat, raised, line, screen, dust, faint and one ember accent.
constexpr Color bg=rgb(7,8,11), surface=rgb(18,20,26), raised=rgb(27,30,38), line=rgb(38,42,52);
constexpr Color ink=rgb(244,242,237), muted=rgb(154,160,172), faint=rgb(94,100,112);
constexpr Color accent=rgb(255,91,46), accentInk=rgb(26,7,0);
constexpr Color success=rgb(70,217,138), warning=rgb(255,178,62), danger=rgb(255,77,94), dangerInk=rgb(26,3,6);
constexpr unsigned bold=800, regular=600;
// R3 refreshes saved Discover results; Square is reserved for search there.
constexpr unsigned r3Button=PS5_PAD_BUTTON_R3;

int lineTop(int box,unsigned size,unsigned lineHeight) noexcept {return box+(static_cast<int>(lineHeight)-static_cast<int>(size*139/100))/2;}
void text(Canvas& c,int x,int box,std::string_view value,unsigned size,unsigned lineHeight,Color color,unsigned weight=regular,int tracking=0) noexcept {
    const int y=lineTop(box,size,lineHeight);
    if(x>=0&&y>=0)c.label(static_cast<unsigned>(x),static_cast<unsigned>(y),value,size,color,weight,tracking);
}
// Centers the line box of one label inside a container of the given height.
void middle(Canvas& c,int x,int box,unsigned height,std::string_view value,unsigned size,Color color,unsigned weight=regular,int tracking=0) noexcept {
    text(c,x,box,value,size,height,color,weight,tracking);
}
unsigned width(Canvas& c,std::string_view value,unsigned size,unsigned weight=regular,int tracking=0) noexcept {return c.text_width(value,size,weight,tracking);}
std::size_t codepointEnd(std::string_view value,std::size_t at) noexcept {
    ++at;while(at<value.size()&&(static_cast<unsigned char>(value[at])&0xc0)==0x80)++at;return at;
}
// Bytes of value, at a code point boundary, whose rendering fits within limit.
std::size_t fitting(Canvas& c,std::string_view value,unsigned size,unsigned weight,int tracking,unsigned limit) noexcept {
    int used=0;std::size_t at=0;
    while(at<value.size()){
        const auto next=codepointEnd(value,at);
        used+=static_cast<int>(c.text_width(botty::slice(value,at,next-at),size,weight))+(at?tracking:0);
        if(used>static_cast<int>(limit))break;
        at=next;
    }
    return at;
}
// Draws text within limit, ending with an ellipsis when shortened. Returns its width.
unsigned fit(Canvas& c,int x,int box,std::string_view value,unsigned size,unsigned lineHeight,unsigned limit,Color color,unsigned weight=regular,int tracking=0) noexcept {
    if(width(c,value,size,weight,tracking)<=limit){text(c,x,box,value,size,lineHeight,color,weight,tracking);return width(c,value,size,weight,tracking);}
    const unsigned ellipsis=width(c,"\xe2\x80\xa6",size,weight);
    std::array<char,600> shortened{};
    auto n=fitting(c,value,size,weight,tracking,limit>ellipsis?limit-ellipsis:0);
    if(n>shortened.size()-4)n=shortened.size()-4;
    while(n&&(static_cast<unsigned char>(value[n])&0xc0)==0x80)--n;
    while(n&&value[n-1]==' ')--n;
    std::copy_n(value.begin(),n,shortened.begin());std::copy_n("\xe2\x80\xa6",3,shortened.begin()+n);
    text(c,x,box,shortened.data(),size,lineHeight,color,weight,tracking);
    return width(c,shortened.data(),size,weight,tracking);
}
// Shows the end of long input, such as magnet links, after an ellipsis.
void fitEnd(Canvas& c,int x,int box,std::string_view value,unsigned size,unsigned lineHeight,unsigned limit,Color color,unsigned weight=regular) noexcept {
    if(width(c,value,size,weight)<=limit){text(c,x,box,value,size,lineHeight,color,weight);return;}
    const unsigned ellipsis=width(c,"\xe2\x80\xa6",size,weight);
    std::size_t start=value.size()>480?value.size()-480:0;
    while(start<value.size()&&(static_cast<unsigned char>(value[start])&0xc0)==0x80)++start;
    while(start<value.size()&&width(c,botty::slice(value,start),size,weight)+ellipsis>limit)start=codepointEnd(value,start);
    std::array<char,600> shortened{};std::copy_n("\xe2\x80\xa6",3,shortened.begin());
    const auto tail=botty::slice(value,start);std::copy_n(tail.begin(),std::min(tail.size(),shortened.size()-4),shortened.begin()+3);
    text(c,x,box,shortened.data(),size,lineHeight,color,weight);
}
// Word-wraps text into at most lines rows; the last row is shortened if needed.
// Without paint it only counts the rows, to size a container first.
unsigned wrap(Canvas& c,int x,int box,std::string_view value,unsigned size,unsigned lineHeight,unsigned limit,unsigned lines,Color color,unsigned weight=regular,bool paint=true) noexcept {
    unsigned used=0;
    while(used<lines){
        while(!value.empty()&&(value.front()==' '||value.front()=='\n'))value.remove_prefix(1);
        if(value.empty())break;
        const auto newline=value.find('\n');
        const auto row=newline==std::string_view::npos?value:botty::slice(value,0,newline);
        const int y=box+static_cast<int>(used*lineHeight);
        if(used+1==lines){if(paint)fit(c,x,y,row,size,lineHeight,limit,color,weight);++used;break;}
        auto n=fitting(c,row,size,weight,0,limit);
        if(n<row.size()){auto space=n;while(space&&row[space]!=' ')--space;if(space)n=space;}
        if(!n)n=codepointEnd(row,0);
        if(paint)text(c,x,y,botty::slice(row,0,n),size,lineHeight,color,weight);
        value.remove_prefix(n);++used;
    }
    return used;
}
void upper(std::string_view value,char* out,unsigned size) noexcept {
    unsigned n=0;for(char ch:value){if(n+1>=size)break;out[n++]=ch>='a'&&ch<='z'?static_cast<char>(ch-32):ch;}out[n]=0;
}
void grouped(long long value,char* out,unsigned size) noexcept {
    if(value<0){std::snprintf(out,size,"Unknown");return;}
    char digits[32];const int n=std::snprintf(digits,sizeof(digits),"%lld",value);unsigned at=0;
    for(int i=0;i<n&&at+2<size;++i){if(i&&(n-i)%3==0)out[at++]=',';out[at++]=digits[i];}
    out[at]=0;
}
// Remaining time without the "ETA" prefix, for compact rows.
void timeLeft(const botty::Entry& e,char* out,unsigned size) noexcept {
    if(e.eta<0||e.eta>315360000){out[0]=0;return;}
    const auto seconds=static_cast<unsigned long long>(e.eta);const char* approx=e.etaEstimated?"~":"";
    if(seconds<60)std::snprintf(out,size,"%s<1 min",approx);
    else if(seconds<3600)std::snprintf(out,size,"%s%llu min",approx,seconds/60);
    else if(seconds<86400)std::snprintf(out,size,"%s%llu h %llu min",approx,seconds/3600,(seconds%3600)/60);
    else std::snprintf(out,size,"%s%llu d %llu h",approx,seconds/86400,(seconds%86400)/3600);
}
// 01.007.000 -> 1.7.0, the public form shown in the header and footer.
void publicVersion(std::string_view native,char* out,unsigned size) noexcept {
    if(!botty::validNativeVersion(native)){std::snprintf(out,size,"%.*s",static_cast<int>(native.size()),native.data());return;}
    const auto part=[&](unsigned at,unsigned digits){unsigned n=0;for(unsigned i=0;i<digits;++i)n=n*10+static_cast<unsigned>(native[at+i]-'0');return n;};
    std::snprintf(out,size,"%u.%u.%u",part(0,2),part(3,3),part(7,3));
}
void storageName(std::string_view id,char* out,unsigned size) noexcept {botty::storageLabel(catalog,id.empty()?std::string_view("internal"):id,out,size);}

enum class Glyph { none, cross, square, triangle, circle, menu, arrows, updown };
unsigned glyphWidth(Glyph g) noexcept {return g==Glyph::none?0:g==Glyph::arrows?40:24;}
// Controller glyphs from the Paper icon paths, drawn in a 24-pixel box.
void glyph(Canvas& c,Glyph g,float x,float y,Color color,float scale=1,float weight=2.6f) noexcept {
    const auto at=[&](float vx,float vy,float wx,float wy){c.stroke(x+vx*scale,y+vy*scale,x+wx*scale,y+wy*scale,weight,color);};
    switch(g){
    case Glyph::cross:at(5,5,19,19);at(19,5,5,19);break;
    case Glyph::square:at(4,4,20,4);at(20,4,20,20);at(20,20,4,20);at(4,20,4,4);break;
    case Glyph::triangle:at(12,4,21,19);at(21,19,3,19);at(3,19,12,4);break;
    case Glyph::circle:c.ring(x+12*scale,y+12*scale,8.5f*scale,weight,color);break;
    case Glyph::menu:at(4,7,20,7);at(4,12,20,12);at(4,17,20,17);break;
    case Glyph::arrows:at(10,5,4,12);at(4,12,10,19);at(30,5,36,12);at(36,12,30,19);break;
    case Glyph::updown:at(6,9,12,3);at(12,3,18,9);at(6,15,12,21);at(12,21,18,15);break;
    case Glyph::none:break;
    }
}
void chevron(Canvas& c,float x,float y,bool right,Color color) noexcept {
    if(right){c.stroke(x+3,y+3,x+11,y+11,3,color);c.stroke(x+11,y+11,x+3,y+19,3,color);}
    else {c.stroke(x+11,y+3,x+3,y+11,3,color);c.stroke(x+3,y+11,x+11,y+19,3,color);}
}
// One footer hint; the primary hint names the Cross action.
unsigned hint(Canvas& c,unsigned x,Glyph g,std::string_view label,bool primary=false,unsigned y=1006) noexcept {
    glyph(c,g,static_cast<float>(x),static_cast<float>(y),primary?ink:muted);
    const unsigned at=x+glyphWidth(g)+(g==Glyph::none?0:10);
    text(c,static_cast<int>(at),static_cast<int>(y),label,20,24,primary?ink:muted,primary?bold:regular);
    return at+width(c,label,20,primary?bold:regular)+36;
}
unsigned keycap(Canvas& c,unsigned x,unsigned y,std::string_view label,unsigned height=32) noexcept {
    const unsigned w=width(c,label,20,bold)+24;
    c.rounded(x,y,w,height,8,line);c.rounded(x+2,y+2,w-4,height-4,6,bg);
    middle(c,static_cast<int>(x+12),static_cast<int>(y),height,label,20,muted,bold);
    return w;
}
// PS5 focus: a 3-pixel screen-white ring separated from the element by a gap.
void focusRing(Canvas& c,unsigned x,unsigned y,unsigned w,unsigned h,unsigned radius,unsigned gap,Color behind) noexcept {
    c.rounded(x-gap-3,y-gap-3,w+2*(gap+3),h+2*(gap+3),radius,ink);
    c.rounded(x-gap,y-gap,w+2*gap,h+2*gap,radius>3?radius-3:0,behind);
}
unsigned badge(Canvas& c,unsigned x,unsigned y,std::string_view label,Color color,Color fill,unsigned alpha) noexcept {
    const unsigned w=width(c,label,20,bold,2)+20;
    c.rounded(x,y,w,30,8,fill,alpha);middle(c,static_cast<int>(x+10),static_cast<int>(y),30,label,20,color,bold,2);
    return w;
}
unsigned eyebrow(Canvas& c,int x,int box,std::string_view value,Color color,bool dot=false,Color dotColor=accent) noexcept {
    char label[160];upper(value,label,sizeof(label));
    if(dot){c.rounded(static_cast<unsigned>(x),static_cast<unsigned>(box+8),8,8,4,dotColor);x+=20;}
    text(c,x,box,label,20,24,color,bold,3);
    return static_cast<unsigned>(x)+width(c,label,20,bold,3);
}
// Filter chip; a nonzero dot color adds a status marker before the label.
unsigned chip(Canvas& c,unsigned x,unsigned y,std::string_view label,bool active,const char* count=nullptr,Color dot=Color{}) noexcept {
    const bool marker=static_cast<std::uint32_t>(dot)!=0;
    unsigned w=44+width(c,label,20,bold)+(count?10+width(c,count,20,bold):0)+(marker?18:0);
    c.rounded(x,y,w,48,24,active?ink:raised);
    unsigned at=x+22;
    if(marker){c.rounded(at,y+20,8,8,4,dot);at+=18;}
    middle(c,static_cast<int>(at),static_cast<int>(y),48,label,20,active?bg:ink,bold);
    if(count)middle(c,static_cast<int>(at+width(c,label,20,bold)+10),static_cast<int>(y),48,count,20,active?faint:muted,bold);
    return w;
}
void progress(Canvas& c,unsigned x,unsigned y,unsigned w,unsigned h,double value,Color color,Color track=line,unsigned trackAlpha=255) noexcept {
    c.rounded(x,y,w,h,h/2,track,trackAlpha);
    const unsigned filled=static_cast<unsigned>(w*std::clamp(value,0.0,1.0));
    if(filled)c.rounded(x,y,std::max(filled,h),h,h/2,color);
}

std::array<char,96> artworkId(const botty::Entry& entry,char source) noexcept {
    if(source=='e')return entry.id;
    std::array<char,96> id{};
    std::snprintf(id.data(),id.size(),"%c:%.93s",source,entry.id.data());return id;
}
const std::array<unsigned char,160*240*3>* coverFor(const std::array<char,96>& id) noexcept {
    if(!id[0])return nullptr;
    for(unsigned i=0;i<covers.ids.size();++i)if(covers.ready[i]&&covers.ids[i]==id)return &covers.pixels[i];
    return nullptr;
}
// Generated artwork for games without a cover.
void gamePattern(Canvas& c,unsigned x,unsigned y,unsigned w,unsigned h,unsigned seed) noexcept {
    constexpr Color tops[]={rgb(58,40,52),rgb(32,46,58),rgb(64,38,30),rgb(36,40,64),rgb(48,52,34),rgb(56,34,58)};
    constexpr Color lights[]={accent,ink,muted,accent,warning,ink};
    c.gradient(x,y,w,h,tops[seed%6],bg);
    const unsigned radius=std::min(w,h)/3;
    c.circle(x+w*3/5,y+h*2/5,radius,lights[seed%6]);
    c.circle(x+w*3/5-radius/4,y+h*2/5-radius/5,radius-3,tops[seed%6]);
    for(unsigned i=0;i<5;++i){const unsigned yy=y+h*2/3+i*8;if(yy<y+h)c.rectangle(x+w/10,yy,w*4/5,1,lights[seed%6]);}
}
void art(Canvas& c,unsigned x,unsigned y,unsigned w,unsigned h,unsigned radius,const std::array<char,96>& id,std::string_view name,unsigned seed,Color behind,unsigned alpha=255) noexcept {
    if(const auto* pixels=coverFor(id)){c.poster(x,y,w,h,*pixels,radius,alpha);return;}
    gamePattern(c,x,y,w,h,seed);
    if(w>=120&&!name.empty()){c.fade(x,y+h/2,w,h/2,bg,0,230,false);wrap(c,static_cast<int>(x+14),static_cast<int>(y+h-96),name,20,26,w-28,3,ink,bold);}
    if(alpha<255)c.rounded(x,y,w,h,0,behind,255-alpha);
    c.clip_corners(x,y,w,h,radius,behind);
}
// Cover art blown up behind the hero, faded into velvet on the left and bottom.
void hero(Canvas& c,const std::array<char,96>& id,unsigned seed,unsigned fadeTop) noexcept {
    if(const auto* pixels=coverFor(id))c.poster(532,0,1388,780,*pixels,0,255,true);
    else gamePattern(c,532,0,1388,780,seed);
    c.fade(532,0,287,780,bg,255,209,true);c.fade(819,0,533,780,bg,209,0,true);
    c.fade(0,fadeTop,1920,780-fadeTop,bg,0,255,false);
    // Keeps the header status legible over bright cover art.
    c.fade(532,0,1388,190,bg,190,0,false);
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

struct Focus { const botty::Entry* entry=nullptr; bool torrent=false; };
Focus focused() noexcept {
    if(model.tab==Model::activity){const auto item=botty::activityAt(catalog,model.filter,model.selected);return {item.entry,item.torrent};}
    if(model.tab==Model::library)return {botty::libraryAt(catalog,model.filter,model.selected),false};
    return {};
}
unsigned workflowTab(const Focus& f) noexcept {return model.tab==Model::library?2:f.torrent?0:1;}
char artSource(const Focus& f) noexcept {return f.torrent?'t':'j';}

// Progress through Download -> Extract -> Library, or the name of another task.
struct Stage { const char* label; Color color; int step; };
Stage stageOf(const botty::Entry& e,bool torrent) noexcept {
    const auto state=botty::activityState(e,torrent);const auto status=std::string_view(e.status.data());
    if(state==botty::ActivityState::attention)return {"Needs you",warning,-1};
    if(torrent){
        if(state==botty::ActivityState::done)return {"Downloaded",success,0};
        return {status=="Paused"?"Paused":status=="Queued"?"Queued":status=="Verifying"?"Verifying":"Download",state==botty::ActivityState::running?accent:muted,0};
    }
    if(e.task){
        const auto kind=std::string_view(e.kind.data());
        const char* label=kind=="transfer"?"Transfer":kind=="compression"?"Compress":kind=="deletion"?"Delete":kind=="restore"?"Restore":"Task";
        if(state==botty::ActivityState::done)return {"Done",success,-1};
        return {label,state==botty::ActivityState::running?accent:muted,-1};
    }
    // Ready content is extracted but not yet moved into the Library.
    if(status=="ready")return {"Ready",success,1};
    if(status=="moving")return {"Library",accent,2};
    if(status=="extracting")return {"Extract",accent,1};
    return {status=="cancelled"?"Cancelled":"Extract",muted,1};
}
void describe(const botty::Entry& e,bool torrent,char* out,unsigned size) noexcept {
    char done[48],total[48],rate[48],left[48];
    botty::formatBytes(e.bytes,done,sizeof(done));botty::formatBytes(e.total,total,sizeof(total));botty::formatBytes(e.download,rate,sizeof(rate));timeLeft(e,left,sizeof(left));
    const auto status=std::string_view(e.status.data());const auto state=botty::activityState(e,torrent);
    if(state==botty::ActivityState::attention&&e.error[0]){std::snprintf(out,size,"%s",e.error.data());return;}
    if(torrent){
        if(e.complete){if(e.upload>0){botty::formatBytes(e.upload,rate,sizeof(rate));std::snprintf(out,size,"Downloaded \xc2\xb7 %s \xc2\xb7 seeding at %s/s",total,rate);}else std::snprintf(out,size,"Downloaded \xc2\xb7 %s",total);return;}
        if(status=="Paused"){std::snprintf(out,size,"Paused \xc2\xb7 %s of %s",done,total);return;}
        if(e.download>0)std::snprintf(out,size,"%s of %s \xc2\xb7 %s/s%s%s%s",done,total,rate,left[0]?" \xc2\xb7 ":"",left,left[0]?" left":"");
        else std::snprintf(out,size,"%s of %s \xc2\xb7 %s",done,total,status=="Queued"?"queued":status=="Verifying"?"verifying files":"waiting for peers");
        return;
    }
    if(e.task){
        const char* phase=e.phase[0]?e.phase.data():e.status.data();
        if(e.items&&e.total>0)std::snprintf(out,size,"%s \xc2\xb7 %.0f of %.0f items%s%s",phase,e.bytes,e.total,left[0]?" \xc2\xb7 ":"",left);
        else if(e.total>0)std::snprintf(out,size,"%s \xc2\xb7 %s of %s%s%s",phase,done,total,left[0]?" \xc2\xb7 ":"",left);
        else std::snprintf(out,size,"%s \xc2\xb7 %.0f s elapsed",phase,e.elapsed);
        return;
    }
    if(status=="extracting"){std::snprintf(out,size,"%s \xc2\xb7 %s/s%s%s%s",e.phase[0]?e.phase.data():"Extracting",rate,left[0]?" \xc2\xb7 ":"",left,left[0]?" left":"");return;}
    if(status=="ready"){std::snprintf(out,size,"Extracted \xc2\xb7 move it to Library from Quick actions");return;}
    if(status=="moving"){std::snprintf(out,size,"Moving to Library");return;}
    std::snprintf(out,size,"%s",status=="cancelled"?"Extraction cancelled \xc2\xb7 files were kept":status.data());
}
// finished marks the current step complete, such as a download waiting to be extracted.
void pipeline(Canvas& c,int x,int box,unsigned size,unsigned lineHeight,unsigned dot,unsigned connector,int step,double value,bool percent,bool finished=false) noexcept {
    const char* names[]={"Download","Extract","Library"};
    for(int i=0;i<3;++i){
        const unsigned dy=static_cast<unsigned>(box)+(lineHeight-dot)/2;
        if(i==step&&!finished){c.rounded(static_cast<unsigned>(x),dy,dot,dot,dot/2,accent);}
        else if(i<=step){c.rounded(static_cast<unsigned>(x),dy,dot,dot,dot/2,success);}
        else c.ring(static_cast<float>(x)+dot/2.f,static_cast<float>(dy)+dot/2.f,dot/2.f-1,2,faint);
        x+=static_cast<int>(dot)+(size>20?14:10);
        char label[48];if(i==step&&percent)std::snprintf(label,sizeof(label),"%s %.0f%%",names[i],value*100);else std::snprintf(label,sizeof(label),"%s",names[i]);
        const bool current=i==step&&!finished;
        text(c,x,box,label,size,lineHeight,current?ink:i<=step?muted:faint,current?bold:regular);
        x+=static_cast<int>(width(c,label,size,current?bold:regular));
        if(i<2){x+=size>20?14:10;c.rectangle(static_cast<unsigned>(x),static_cast<unsigned>(box)+lineHeight/2-1,connector,2,line);x+=static_cast<int>(connector)+(size>20?14:10);}
    }
}

const char* explain(Op op,const botty::Entry* e) noexcept {
    switch(op){
    case Op::pause:return "Pauses this download. Resume it at any time.";
    case Op::resume:return "Resumes this download where it stopped.";
    case Op::verify:return "rTorrent rechecks downloaded pieces. Extraction waits until verification finishes.";
    case Op::extract:return "Original archives are kept for seeding. Follow progress in Activity.";
    case Op::transfer:return "Source removal follows a successful copy; torrents stay paused. Follow progress in Activity.";
    case Op::move:return "Moves verified content. Existing files are not replaced. ShadowMount may need a scan.";
    case Op::compress:return "Creates a compressed copy and keeps the original. Close Botty+ when prompted. APR games need an existing index.";
    case Op::restoreOriginal:return "Restores the retained original as the playable game. The compressed image and archives are kept. Close Botty+ to finish.";
    case Op::removeOriginal:return "Deletes the retained original without verification. The compressed copy, saves and archives are kept.";
    case Op::cancelCompression:return "Requests cancellation and keeps the original game. Wait until the compression worker stops.";
    case Op::removeLibrary:return e&&e->compressed?"Deletes the compressed game and its retained original. Saves, torrents and archives are kept.":"Deletes installed game files. Saves, torrents and archives are kept. Close the game first.";
    case Op::removeTorrent:return "Permanently deletes this torrent and its downloaded files, including archives. Library games are kept.";
    case Op::cancel:return "Stops extraction at the next safe point. Partial files and original archives are kept.";
    case Op::dismiss:{const auto status=e?std::string_view(e->status.data()):std::string_view{};return status=="failed"||status=="cancelled"||status=="interrupted"?"Removes this row and deletes its partial files. Original downloads and archives are kept.":"Hides this row from Activity. Files are kept; ready and moved content stays in Library.";}
    case Op::remove:return "Deletes this extraction and any partial output. Original downloads and archives are kept.";
    default:return "";
    }
}
const char* buttonLabel(Op op) noexcept {
    switch(op){
    case Op::removeTorrent:case Op::removeLibrary:case Op::remove:return "Delete\xe2\x80\xa6";
    case Op::removeOriginal:return "Delete original\xe2\x80\xa6";
    case Op::restoreOriginal:return "Restore original\xe2\x80\xa6";
    case Op::dismiss:return "Remove\xe2\x80\xa6";
    case Op::cancel:return "Cancel extraction\xe2\x80\xa6";
    case Op::cancelCompression:return "Cancel compression\xe2\x80\xa6";
    case Op::compress:return "Compress";
    default:return botty::operationLabel(op);
    }
}

void notify(Toast::Kind kind,std::string_view title,std::string_view message) noexcept {
    toast=Toast{};toast.kind=kind;toast.visible=true;toast.until=botty::platform::now()+4000000;
    std::snprintf(toast.title.data(),toast.title.size(),"%.*s",static_cast<int>(title.size()),title.data());
    std::snprintf(toast.message.data(),toast.message.size(),"%.*s",static_cast<int>(message.size()),message.data());
    ++displayRevision;
}
void resultToast(const botty::ActionResult& result) noexcept {
    using Status=botty::ActionResult::Status;
    if(result.status!=Status::success){notify(Toast::Kind::error,result.status==Status::uncertain?"Check the current state":"Request failed",result.message.data());return;}
    char title[160];const char* name=sentName.data();
    switch(sentOperation){
    case Op::grab:case Op::exploreGrab:std::snprintf(title,sizeof(title),"%s added",name);break;
    case Op::add:std::snprintf(title,sizeof(title),"Magnet link added");break;
    case Op::pause:std::snprintf(title,sizeof(title),"%s paused",name);break;
    case Op::resume:std::snprintf(title,sizeof(title),"%s resumed",name);break;
    case Op::checkNativeUpdate:std::snprintf(title,sizeof(title),"Checking for updates");break;
    default:std::snprintf(title,sizeof(title),"%s",botty::operationLabel(sentOperation));break;
    }
    notify(Toast::Kind::success,title,result.message.data());toast.cover=sentCover;
}
bool send(const botty::Command& command,std::string_view name,const std::array<char,96>& cover) noexcept {
    if(!network.submit(command)){notify(Toast::Kind::error,"Request not sent","Network is busy or unavailable. Please try again.");return false;}
    sentOperation=command.operation;sentCover=cover;
    std::snprintf(sentName.data(),sentName.size(),"%.*s",static_cast<int>(name.size()),name.data());
    return true;
}
void rememberSearch(std::string_view query) noexcept {
    std::array<char,201> entry{};std::copy_n(query.begin(),std::min<std::size_t>(query.size(),200),entry.begin());
    unsigned keep=0;std::array<std::array<char,201>,3> next{};next[keep++]=entry;
    for(const auto& old:recentSearches)if(old[0]&&old!=entry&&keep<next.size())next[keep++]=old;
    recentSearches=next;
}

void header(Canvas& c,const char* state,Color stateColor) noexcept {
    c.rounded(96,42,44,44,12,accent);
    middle(c,static_cast<int>(96+(44-width(c,"B+",20,bold))/2),42,44,"B+",20,accentInk,bold);
    middle(c,154,47,34,"Botty+",28,ink,bold);
    unsigned x=154+width(c,"Botty+",28,bold)+64;
    x+=keycap(c,x,48,"L1")+40;
    const char* tabs[]={"Discover","Activity","Library","System"};
    for(unsigned i=0;i<Model::tabCount;++i){
        const bool active=model.tab==i;const unsigned w=width(c,tabs[i],28,active?bold:regular);
        text(c,static_cast<int>(x),47,tabs[i],28,34,active?ink:muted,active?bold:regular);
        if(active)c.rounded(x+(w-32)/2,89,32,4,2,accent);
        x+=w+40;
    }
    keycap(c,x,48,"R1");
    char free[96],size[48];free[0]=0;
    if(catalog.valid){botty::formatBytes(catalog.freeBytes,size,sizeof(size));std::snprintf(free,sizeof(free),"PS5 SSD \xc2\xb7 %s free",size);}
    unsigned right=1824;
    if(free[0]){const unsigned w=width(c,free,20);text(c,static_cast<int>(right-w),52,free,20,24,muted);right-=w+28;c.rectangle(right-1,52,1,24,line);right-=29;}
    const unsigned w=width(c,state,20);text(c,static_cast<int>(right-w),52,state,20,24,ink);
    c.rounded(right-w-20,59,10,10,5,stateColor);
}
void footerRight(Canvas& c,const char* position) noexcept {
    char label[48];std::snprintf(label,sizeof(label),"Botty+ %s",botty::nativeDisplayVersion);
    const unsigned version=width(c,label,20);
    text(c,static_cast<int>(1824-version),1006,label,20,24,faint);
    if(position&&position[0]){const unsigned w=width(c,position,20);text(c,static_cast<int>(1824-version-36-w),1006,position,20,24,muted);}
}

// --- Discover ---------------------------------------------------------------
bool exploreCurrent() noexcept {return std::string_view(catalog.exploreSort.data())==Model::exploreSorts[model.exploreSort];}
void drawDiscover(Canvas& c,bool requested,const char* detail) noexcept {
    const unsigned count=model.count;
    char label[160];
    if(count){
        const auto& e=catalog.exploreResults[model.selected];
        hero(c,e.id,model.selected,500);
        char sort[64];std::snprintf(sort,sizeof(sort),"%s \xc2\xb7 PS5",Model::exploreLabels[model.exploreSort]);
        eyebrow(c,96,196,sort,muted,true);
        unsigned size=76;if(width(c,e.name.data(),76,bold,-2)>1100)size=60;
        fit(c,96,240,e.name.data(),size,84,1100,ink,bold,-2);
        char total[48],seeds[32],grabs[32],meta[96];botty::formatBytes(e.total,total,sizeof(total));grouped(e.peers,seeds,sizeof(seeds));grouped(e.completedCount,grabs,sizeof(grabs));
        unsigned x=96;text(c,static_cast<int>(x),344,total,24,30,ink,bold);x+=width(c,total,24,bold)+18;
        const unsigned sources=e.sourceCount?e.sourceCount:1;
        const char* parts[]={seeds,grabs,nullptr};
        const char* suffixes[]={" seeders"," grabs"};
        for(unsigned i=0;i<3;++i){
            text(c,static_cast<int>(x),344,"\xc2\xb7",24,30,faint);x+=width(c,"\xc2\xb7",24)+18;
            if(i<2)std::snprintf(meta,sizeof(meta),"%s%s",parts[i],suffixes[i]);else std::snprintf(meta,sizeof(meta),"%u source%s",sources,sources==1?"":"s");
            text(c,static_cast<int>(x),344,meta,24,30,muted);x+=width(c,meta,24)+18;
        }
        // Hints, not focus targets: Cross gets the game and Options compares sources.
        const unsigned getWidth=28+26+16+width(c,"Get game",28,bold)+36;
        c.rounded(96,414,getWidth,72,14,accent);glyph(c,Glyph::cross,124,437,accentInk,26.f/24,3.5f);
        middle(c,166,414,72,"Get game",28,accentInk,bold);
        const unsigned compareX=96+getWidth+16,compareWidth=24+30+14+width(c,"Compare sources",28,bold)+32;
        c.rounded(compareX,414,compareWidth,72,14,ink,26);
        for(unsigned i=0;i<3;++i)c.stroke(static_cast<float>(compareX+27),static_cast<float>(444+i*6),static_cast<float>(compareX+51),static_cast<float>(444+i*6),3,ink);
        middle(c,static_cast<int>(compareX+68),414,72,"Compare sources",28,ink,bold);
        char disk[96];storageName(workflow.lastStorage.data(),disk,sizeof(disk));
        if(catalog.storageSupported)std::snprintf(label,sizeof(label),"Best source \xc2\xb7 %s \xc2\xb7 %s \xe2\x80\x94 change before downloading",workflow.lastAutomatic?"Full auto":"Download only",disk);
        else std::snprintf(label,sizeof(label),"Best source \xe2\x80\x94 change before downloading");
        text(c,96,508,label,20,24,muted);
    }else {
        eyebrow(c,96,196,"Discover \xc2\xb7 PS5",muted,true);
        text(c,96,240,catalog.valid?"Find your next game.":"Botty is not connected.",76,84,ink,bold,-2);
        wrap(c,96,344,catalog.valid?(*detail?detail:"Rankings from every enabled tracker appear here."):detail,24,34,1100,2,muted);
    }
    // Ranking chips: Triangle cycles them; Square searches every tracker.
    unsigned x=96;
    for(unsigned i=0;i<3;++i){
        const bool active=model.exploreSort==i;
        text(c,static_cast<int>(x),607,Model::exploreLabels[i],28,34,active?ink:faint,active?bold:regular);
        x+=width(c,Model::exploreLabels[i],28,active?bold:regular)+32;
    }
    glyph(c,Glyph::triangle,static_cast<float>(x),613,muted,22.f/24,2.5f);x+=30;
    text(c,static_cast<int>(x),612,"Sort",20,24,muted);x+=width(c,"Sort",20)+32;
    const bool saved=std::string_view(catalog.exploreNotice.data()).starts_with("Cached")||std::string_view(catalog.exploreNotice.data()).starts_with("Saved");
    const char* state=!catalog.exploreSupported?"":catalog.exploreAdding?"Adding the download\xe2\x80\xa6":catalog.exploreBusy||requested?(count?"Refreshing\xe2\x80\xa6":"Loading\xe2\x80\xa6"):catalog.exploreError[0]&&count?catalog.exploreError.data():"";
    if(*state)fit(c,static_cast<int>(x),612,state,20,24,1500-x,faint);
    else if(saved&&count){text(c,static_cast<int>(x),612,"Saved results",20,24,faint);x+=width(c,"Saved results",20)+14;x+=keycap(c,x,608,"R3")+10;text(c,static_cast<int>(x),612,"Refresh",20,24,faint);}
    const unsigned pill=18+20+12+width(c,"Search all trackers",20)+24;
    c.rounded(1824-pill,600,pill,48,24,raised);glyph(c,Glyph::square,static_cast<float>(1824-pill+18),614,ink,20.f/24,2.5f);
    middle(c,static_cast<int>(1824-pill+50),600,48,"Search all trackers",20,ink);
    // The focused game leads the shelf, like the PS5 home row.
    x=96;
    for(unsigned i=model.selected;i<count&&x<1824;++i){
        const auto& e=catalog.exploreResults[i];
        if(i==model.selected){focusRing(c,104,677,182,273,18,5,bg);art(c,104,677,182,273,12,e.id,e.name.data(),i,bg);x+=198+20;continue;}
        if(x+168>1824)break;
        art(c,x,688,168,252,12,e.id,e.name.data(),i,bg);x+=188;
    }
    unsigned hx=96;
    if(count)hx=hint(c,hx,Glyph::cross,"Get game",true);
    hx=hint(c,hx,Glyph::square,"Search",!count);hx=hint(c,hx,Glyph::triangle,"Sort");
    if(count)hint(c,hx,Glyph::menu,"Compare sources");
    char position[48];if(count)std::snprintf(position,sizeof(position),"%u of %u",model.selected+1,count);else position[0]=0;
    footerRight(c,position);
}
void searchField(Canvas& c,std::string_view value,bool focus,const char* right,bool password) noexcept {
    if(focus)focusRing(c,96,164,1728,104,24,4,bg);
    c.rounded(96,164,1728,104,18,raised);
    glyph(c,Glyph::square,128,201,focus?ink:muted,30.f/24,3);
    std::array<char,160> masked{};
    if(password){const unsigned n=std::min<std::size_t>(value.size(),24);for(unsigned i=0;i<n;++i)std::copy_n("\xe2\x80\xa2",3,masked.begin()+i*3);value=masked.data();}
    const unsigned rightWidth=right?width(c,right,20):0,limit=1792-rightWidth-48-182-27;
    unsigned w=0;
    if(value.size()>200||width(c,value,44,bold,-1)>limit){fitEnd(c,182,189,value,28,54,limit,ink,bold);w=std::min(width(c,value,28,bold),limit);}
    else {text(c,182,189,value,44,54,ink,bold,-1);w=width(c,value,44,bold,-1);}
    if(focus)c.rounded(182+w+(w?24:0),192,3,48,2,accent);
    if(right)text(c,static_cast<int>(1792-rightWidth),204,right,20,24,muted);
}
void drawSearch(Canvas& c) noexcept {
    // Square edits the query; its glyph sits inside the right-hand hint.
    c.rounded(96,164,1728,104,18,raised);
    glyph(c,Glyph::square,128,201,muted,30.f/24,3);
    const unsigned hintWidth=width(c,"Press",20)+8+18+8+width(c,"to edit with the PS5 keyboard",20);
    const unsigned limit=1792-hintWidth-48-182-27;
    fit(c,182,189,catalog.searchQuery.data(),44,54,limit,ink,bold,-1);
    unsigned hx=1792-hintWidth;text(c,static_cast<int>(hx),204,"Press",20,24,muted);hx+=width(c,"Press",20)+8;
    glyph(c,Glyph::square,static_cast<float>(hx),207,muted,18.f/24,2.2f);hx+=26;text(c,static_cast<int>(hx),204,"to edit with the PS5 keyboard",20,24,muted);
    char heading[64];std::snprintf(heading,sizeof(heading),"%u result%s",catalog.resultCount,catalog.resultCount==1?"":"s");
    if(catalog.resultCount){text(c,96,296,heading,28,34,ink,bold);text(c,static_cast<int>(96+width(c,heading,28,bold)+16),298,"most seeded first",24,30,muted);}
    const char* state=!catalog.searchSupported?"Update the Botty service to enable search.":catalog.searchBusy?"Searching every tracker\xe2\x80\xa6":catalog.searchAdding?"Adding the download\xe2\x80\xa6":catalog.searchError[0]?catalog.searchError.data():!catalog.resultCount?"No results. Try another name.":"";
    if(!catalog.resultCount){text(c,96,360,catalog.searchBusy?"Searching\xe2\x80\xa6":"Nothing found yet.",44,52,ink,bold,-1);wrap(c,96,424,state,24,34,1500,2,catalog.searchError[0]?warning:muted);}
    else if(*state)fit(c,static_cast<int>(96+width(c,heading,28,bold)+16+width(c,"most seeded first",24)+32),298,state,24,30,900,catalog.searchError[0]?warning:muted);
    const unsigned first=(model.selected/5)*5;
    for(unsigned i=first;i<catalog.resultCount&&i<first+5;++i){
        const auto& e=catalog.results[i];const unsigned y=359+(i-first)*122;const bool focus=i==model.selected;
        if(focus)focusRing(c,96,y,1728,100,20,4,bg);
        c.rounded(96,y,1728,100,14,focus?raised:surface);
        const auto id=artworkId(e,'s');
        if(coverFor(id))art(c,108,y+8,56,84,8,id,{},i,focus?raised:surface);else {c.rounded(108,y+8,56,84,8,line);glyph(c,Glyph::square,124,y+38,faint,1,2.2f);}
        fit(c,188,y+19,e.name.data(),28,34,916,e.complete?muted:ink,bold);
        text(c,188,y+57,e.complete?"Already added to Botty+":"Prowlarr",20,24,muted);
        char size[48],seeds[48],peers[48];botty::formatBytes(e.total,size,sizeof(size));
        if(e.peers<0)std::snprintf(seeds,sizeof(seeds),"Seeders unknown");else {char n[24];grouped(e.peers,n,sizeof(n));std::snprintf(seeds,sizeof(seeds),"%s seeder%s",n,e.peers==1?"":"s");}
        if(e.downloadingPeers<0)std::snprintf(peers,sizeof(peers),"\xe2\x80\x94");else {char n[24];grouped(e.downloadingPeers,n,sizeof(n));std::snprintf(peers,sizeof(peers),"%s peer%s",n,e.downloadingPeers==1?"":"s");}
        text(c,static_cast<int>(1278-width(c,size,24,bold)),y+35,size,24,30,ink,bold);
        text(c,static_cast<int>(1472-width(c,seeds,24,bold)),y+35,seeds,24,30,focus&&!e.complete?success:e.complete?muted:ink,bold);
        text(c,static_cast<int>(1626-width(c,peers,24)),y+35,peers,24,30,muted);
        if(e.complete)badge(c,1800-width(c,"ADDED",20,bold,2)-20,y+35,"ADDED",success,success,36);
        else if(focus){
            const unsigned w=16+18+10+width(c,"Get",22,bold)+20;
            c.rounded(1800-w,y+26,w,48,12,accent);glyph(c,Glyph::cross,static_cast<float>(1800-w+16),static_cast<float>(y+41),accentInk,.75f,3);
            middle(c,static_cast<int>(1800-w+44),static_cast<int>(y+26),48,"Get",22,accentInk,bold);
        }
    }
    unsigned hx2=96;hx2=hint(c,hx2,Glyph::cross,"Get",true);hx2=hint(c,hx2,Glyph::square,"Edit search");hint(c,hx2,Glyph::circle,"Back to Discover");
    char position[48];if(catalog.resultCount)std::snprintf(position,sizeof(position),"%u of %u",model.selected+1,catalog.resultCount);else position[0]=0;
    footerRight(c,position);
}

// --- Text entry ---------------------------------------------------------------
// While the PS5 keyboard is open, the console draws it over the lower screen.
void drawTyping(Canvas& c) noexcept {
    const auto op=workflow.command.operation;const bool password=op==Op::extract,search=op==Op::search;
    const auto value=std::string_view(workflow.command.text.data());
    char counter[64];std::snprintf(counter,sizeof(counter),"%zu / %u",value.size(),botty::Workflow::textLimit(op));
    searchField(c,value,true,counter,password);
    unsigned x=96;
    if(search&&recentSearches[0][0]){
        x=eyebrow(c,96,328,"Recent",muted)+24;
        for(const auto& recent:recentSearches){if(!recent[0])break;const unsigned w=44+width(c,recent.data(),20,bold);if(x+w>1200)break;chip(c,x,316,recent.data(),false);x+=w+12;}
    }
    const char* advice=search?"Confirm on the keyboard to search all trackers":password?"Confirm on the keyboard to return to Extract":"Confirm on the keyboard to continue";
    text(c,static_cast<int>(1824-width(c,advice,20)),328,advice,20,24,muted);
    if(workflow.notice[0])wrap(c,96,392,workflow.notice.data(),24,34,1728,2,warning);
    if(!nativeKeyboard.active()){
        unsigned hx=96;hx=hint(c,hx,Glyph::cross,"Open PS5 keyboard",true);hx=hint(c,hx,Glyph::square,"In-app keyboard");hint(c,hx,Glyph::circle,"Cancel");
    }
}
// In-app keyboard fallback, for links longer than the system dialog accepts.
void drawKeyboard(Canvas& c) noexcept {
    const auto op=workflow.command.operation;const bool password=op==Op::extract;
    const auto value=std::string_view(workflow.unicodeInput?workflow.codepoint.data():workflow.command.text.data());
    eyebrow(c,96,72,workflow.unicodeInput?"Unicode character":password?"Archive password":op==Op::search?"Search all trackers":"Add a magnet link",accent);
    const bool capacity=op==Op::add&&value.size()>=2048;
    text(c,96,104,workflow.unicodeInput?"Enter a code point":capacity?"Check the end of your link":password?"Enter the archive password":op==Op::search?"Type a game name":"Type your magnet link",44,52,ink,bold,-1);
    const char* subtitle=workflow.notice[0]?workflow.notice.data():workflow.unicodeInput?"Hexadecimal, from 20 to 10FFFF.":capacity?"It is longer than the PS5 keyboard allows (2,048), so finish it here.":password&&!value.size()?"No password? Select Continue.":"L1 / R1 switch between letters, capitals and symbols.";
    fit(c,96,164,subtitle,24,30,1728,workflow.notice[0]?warning:muted);
    c.rounded(96,276,1728,202,18,raised);
    std::array<char,400> shown{};std::string_view visible=value;
    if(password&&!workflow.passwordVisible&&!workflow.unicodeInput){const unsigned n=std::min<std::size_t>(value.size(),120);for(unsigned i=0;i<n;++i)std::copy_n("\xe2\x80\xa2",3,shown.begin()+i*3);visible=shown.data();}
    // Show the end: three wrapped rows that finish with the latest characters.
    std::size_t start=visible.size()>360?visible.size()-360:0;
    while(start<visible.size()&&(static_cast<unsigned char>(visible[start])&0xc0)==0x80)++start;
    const auto tail=botty::slice(visible,start);
    std::size_t rowStart=0;unsigned rows=0;std::array<std::size_t,64> breaks{};
    while(rowStart<tail.size()&&rows<breaks.size()){breaks[rows++]=rowStart;const auto n=fitting(c,botty::slice(tail,rowStart),24,regular,0,1664);rowStart+=n?n:codepointEnd(tail,rowStart)-rowStart;}
    const unsigned firstRow=rows>3?rows-3:0;
    for(unsigned r=firstRow;r<rows;++r){
        const auto end=r+1<rows?breaks[r+1]:tail.size();auto row=botty::slice(tail,breaks[r],end-breaks[r]);
        std::array<char,404> buffer{};
        if(r==firstRow&&(firstRow||start)){std::copy_n("\xe2\x80\xa6",3,buffer.begin());std::copy_n(row.begin(),std::min(row.size(),buffer.size()-4),buffer.begin()+3);row=buffer.data();}
        text(c,128,304+static_cast<int>(r-firstRow)*36,row,24,36,ink);
    }
    text(c,128,426,firstRow||start?"Showing the end":rows?"":"Empty",20,24,muted);
    char counter[64],used[24],limit[24];grouped(static_cast<long long>(value.size()),used,sizeof(used));grouped(workflow.unicodeInput?6:botty::Workflow::textLimit(op),limit,sizeof(limit));
    std::snprintf(counter,sizeof(counter),"%s / %s",used,limit);text(c,static_cast<int>(1792-width(c,counter,20,bold)),426,counter,20,24,ink,bold);
    const auto keys=botty::Workflow::keys(workflow.keyPage);
    for(unsigned i=0;i<40;++i){
        const unsigned x=96+(i%10)*174,y=520+(i/10)*80;const bool focus=workflow.selected==i;
        c.rounded(x,y,162,68,12,focus?ink:surface);
        const auto key=botty::slice(keys,i,1);middle(c,static_cast<int>(x+(162-width(c,key,28,bold))/2),static_cast<int>(y),68,key,28,focus?bg:ink,bold);
    }
    const char* controls[]={"Space","Backspace","Clear","ABC \xc2\xb7 #+=","Continue"};
    const unsigned widths[]={522,244,244,244,426};
    unsigned x=96;
    for(unsigned i=0;i<5;++i){
        const bool focus=workflow.selected>=40&&(workflow.selected-40)/2==i;
        c.rounded(x,848,widths[i],68,12,focus?ink:i==4?accent:surface);
        const char* label=i==4&&workflow.unicodeInput?"Insert":controls[i];
        middle(c,static_cast<int>(x+(widths[i]-width(c,label,24,bold))/2),848,68,label,24,focus?bg:i==4?accentInk:ink,bold);
        x+=widths[i]+12;
    }
    unsigned hx=96;hx=hint(c,hx,Glyph::cross,"Type",true);hx=hint(c,hx,Glyph::square,"Backspace");
    hx+=keycap(c,hx,1004,"L1 / R1",28)+10;text(c,static_cast<int>(hx),1006,"Letters \xc2\xb7 symbols",20,24,muted);hx+=width(c,"Letters \xc2\xb7 symbols",20)+36;
    if(password)hx=hint(c,hx,Glyph::triangle,workflow.passwordVisible?"Hide":"Show");
    hx=hint(c,hx,Glyph::menu,workflow.unicodeInput?"Letters":"Unicode");hint(c,hx,Glyph::circle,"Cancel");
}

// --- Activity -----------------------------------------------------------------
void drawEmpty(Canvas& c,unsigned y,const char* title,const char* body) noexcept {
    c.rounded(96,y,1728,240,22,surface);
    text(c,152,static_cast<int>(y+56),title,44,52,ink,bold,-1);wrap(c,152,static_cast<int>(y+124),body,24,34,1600,2,muted);
}
void drawActivity(Canvas& c,const char* detail) noexcept {
    text(c,96,148,"Activity",60,66,ink,bold,-2);
    unsigned running=0;double rate=0;
    for(unsigned i=0;;++i){const auto item=botty::activityAt(catalog,1,i);if(!item.entry)break;++running;if(!item.entry->items)rate+=item.entry->download;}
    char subtitle[160],speed[48];botty::formatBytes(rate,speed,sizeof(speed));
    std::snprintf(subtitle,sizeof(subtitle),"%u running \xc2\xb7 %s/s \xc2\xb7 keeps going when you close Botty+",running,speed);
    const bool live=catalog.valid||catalog.processing.count;
    text(c,96,224,!live?detail:catalog.stale?"Last update shown \xc2\xb7 reconnecting\xe2\x80\xa6":subtitle,24,30,catalog.stale||!live?warning:muted);
    // Free space on the PS5 SSD and the first connected external disk. Disk
    // totals are unknown, so no fill bars are drawn.
    if(catalog.valid){
        const botty::StorageDevice* external=nullptr;
        for(unsigned i=0;i<catalog.storageCount&&!external;++i)if(catalog.storage[i].available&&std::string_view(catalog.storage[i].id.data())!="internal")external=&catalog.storage[i];
        unsigned x=external?1176:1524;
        for(unsigned n=0;n<(external?2U:1U);++n){
            char name[96],free[64],size[48];
            if(!n){std::snprintf(name,sizeof(name),"PS5 SSD");botty::formatBytes(catalog.freeBytes,size,sizeof(size));}
            else {storageName(external->id.data(),name,sizeof(name));botty::formatBytes(external->freeBytes,size,sizeof(size));}
            std::snprintf(free,sizeof(free),"%s free",size);const unsigned freeWidth=width(c,free,20);
            fit(c,static_cast<int>(x),226,name,20,24,288-freeWidth,ink,bold);text(c,static_cast<int>(x+300-freeWidth),226,free,20,24,muted);
            x+=348;
        }
    }
    const char* names[]={"All","Running","Needs you","Done"};
    unsigned x=96;
    for(unsigned f=0;f<4;++f){char count[16];std::snprintf(count,sizeof(count),"%u",live?botty::activityCount(catalog,f):0);x+=chip(c,x,282,names[f],model.filter==f,count,f==2?warning:Color{})+12;}
    glyph(c,Glyph::arrows,static_cast<float>(x+12),294,faint);text(c,static_cast<int>(x+60),294,"Filter",20,24,faint);
    if(!live){drawEmpty(c,356,connection.status==botty::Probe::checking?"Syncing your downloads":"Your session is offline",detail);}
    else if(!model.count)drawEmpty(c,356,model.filter==2?"Nothing needs you.":model.filter==3?"Nothing finished yet.":"Nothing running.","Find a game in Discover, or add a magnet link with Square. Downloads and extractions appear here.");
    const unsigned first=(model.selected/5)*5;unsigned y=350;
    for(unsigned i=first;i<model.count&&i<first+5;++i){
        const auto item=botty::activityAt(catalog,model.filter,i);if(!item.entry)break;
        const auto& e=*item.entry;const bool focus=i==model.selected;const auto stage=stageOf(e,item.torrent);
        const auto state=botty::activityState(e,item.torrent);const auto id=artworkId(e,item.torrent?'t':'j');
        char line[512],percent[16];describe(e,item.torrent,line,sizeof(line));
        if(focus&&item.torrent&&state==botty::ActivityState::running){
            // The focused download shows its peers and disk; time left sits on the right.
            char done[48],total[48],rate[48],disk[96];botty::formatBytes(e.bytes,done,sizeof(done));botty::formatBytes(e.total,total,sizeof(total));botty::formatBytes(e.download,rate,sizeof(rate));storageName(e.storage.data(),disk,sizeof(disk));
            if(e.peers>=0)std::snprintf(line,sizeof(line),"%s of %s \xc2\xb7 %s/s \xc2\xb7 %d peers \xc2\xb7 %s",done,total,rate,e.peers,disk);
            else std::snprintf(line,sizeof(line),"%s of %s \xc2\xb7 %s/s \xc2\xb7 %s",done,total,rate,disk);
        }
        const bool measured=!(e.task&&e.total<=0)&&state!=botty::ActivityState::attention;
        if(state==botty::ActivityState::done)std::snprintf(percent,sizeof(percent),"100%%");else if(measured)std::snprintf(percent,sizeof(percent),"%.0f%%",e.progress*100);else std::snprintf(percent,sizeof(percent),"\xe2\x80\x94");
        const Color tone=state==botty::ActivityState::done?success:state==botty::ActivityState::attention?warning:state==botty::ActivityState::running?accent:muted;
        if(focus){
            focusRing(c,96,y+7,1728,180,20,4,bg);c.rounded(96,y+7,1728,180,14,raised);
            art(c,114,y+25,96,144,10,id,{},i,raised);
            const unsigned titleWidth=std::min(width(c,e.name.data(),32,bold),700U);
            fit(c,238,static_cast<int>(y+44),e.name.data(),32,40,700,ink,bold);
            if(stage.step>=0)pipeline(c,static_cast<int>(238+titleWidth+24),static_cast<int>(y+52),20,24,12,28,stage.step,0,false,state==botty::ActivityState::done);
            else {c.rounded(238+titleWidth+24,y+58,12,12,6,stage.color);text(c,static_cast<int>(238+titleWidth+46),static_cast<int>(y+52),stage.label,20,24,ink,bold);}
            text(c,static_cast<int>(1792-width(c,percent,44,bold,-1)),static_cast<int>(y+37),percent,44,54,state==botty::ActivityState::done?success:measured?ink:faint,bold,-1);
            progress(c,238,y+105,1554,8,state==botty::ActivityState::attention||state==botty::ActivityState::done?1:e.progress,tone);
            char left[48];timeLeft(e,left,sizeof(left));char eta[64];if(left[0]&&state==botty::ActivityState::running)std::snprintf(eta,sizeof(eta),"%s left",left);else eta[0]=0;
            const unsigned etaWidth=width(c,eta,24,bold);
            fit(c,238,static_cast<int>(y+127),line,24,30,1554-etaWidth-24,state==botty::ActivityState::attention?warning:muted);
            if(eta[0])text(c,static_cast<int>(1792-etaWidth),static_cast<int>(y+127),eta,24,30,ink,bold);
            y+=200;
        }else {
            c.rounded(96,y+7,1728,84,14,surface);
            art(c,114,y+13,48,72,6,id,{},i,surface);
            fit(c,186,static_cast<int>(y+19),e.name.data(),28,34,918,ink,bold);
            fit(c,186,static_cast<int>(y+55),line,20,24,918,state==botty::ActivityState::attention?warning:muted);
            c.rounded(1128,y+44,10,10,5,stage.color);text(c,1148,static_cast<int>(y+37),stage.label,20,24,ink,bold);
            progress(c,1352,y+46,320,6,state==botty::ActivityState::attention||state==botty::ActivityState::done?1:e.progress,tone);
            text(c,static_cast<int>(1792-width(c,percent,28,bold)),static_cast<int>(y+32),percent,28,34,state==botty::ActivityState::done?success:measured?ink:faint,bold);
            y+=104;
        }
    }
    if(catalog.truncated)text(c,960,1006,"Showing the first 256 downloads and jobs.",20,24,warning);
    unsigned hx=96;hx=hint(c,hx,Glyph::cross,"Details",true);hx=hint(c,hx,Glyph::menu,"Quick actions");hint(c,hx,Glyph::square,"Add magnet link");
    char position[48];if(model.count)std::snprintf(position,sizeof(position),"%u of %u",model.selected+1,model.count);else position[0]=0;
    footerRight(c,position);
}

// --- Details ------------------------------------------------------------------
void stat(Canvas& c,unsigned& x,const char* value,const char* label) noexcept {
    text(c,static_cast<int>(x),434,value,32,40,ink,bold);text(c,static_cast<int>(x),478,label,20,24,muted);
    x+=std::max(width(c,value,32,bold),width(c,label,20))+56;
}
void drawDetails(Canvas& c,const Focus& f) noexcept {
    const auto& e=*f.entry;const auto id=artworkId(e,artSource(f));const bool library=model.tab==Model::library;
    hero(c,id,model.selected,420);
    const auto state=botty::activityState(e,f.torrent);const auto stage=stageOf(e,f.torrent);const auto status=std::string_view(e.status.data());
    char location[96],kicker[192];storageName(e.storage.data(),location,sizeof(location));
    const char* phase=library?(status=="moved"?"In Library":status=="ready"?"Ready to move":status=="moving"?"Moving":"Needs you"):
        state==botty::ActivityState::attention?"Needs you":f.torrent?(e.complete?"Downloaded":status=="Paused"?"Paused":"Downloading"):e.task?stage.label:status=="extracting"?"Extracting":status=="ready"?"Extracted":status.data();
    std::snprintf(kicker,sizeof(kicker),"%s \xc2\xb7 %s",phase,location);
    eyebrow(c,96,184,kicker,muted,true,state==botty::ActivityState::attention?warning:library||state==botty::ActivityState::done?success:accent);
    unsigned size=76;if(width(c,e.name.data(),76,bold,-2)>1180)size=60;
    fit(c,96,224,e.name.data(),size,84,1180,ink,bold,-2);
    const bool measured=!(e.task&&e.total<=0);
    if(library){
        std::array<botty::LibraryCopy,2> copies{};const unsigned copyCount=botty::libraryCopies(e,copies);
        unsigned x=96;
        for(unsigned n=0;n<copyCount;++n){char where[96],label[160];storageName(copies[n].storage,where,sizeof(where));std::snprintf(label,sizeof(label),"%s \xc2\xb7 %s",copies[n].format,where);
            c.rounded(static_cast<unsigned>(x),345,14,14,7,n?muted:success);text(c,static_cast<int>(x+28),336,label,24,30,n?muted:ink,n?regular:bold);x+=28+width(c,label,24,n?regular:bold)+40;}
    }else if(stage.step>=0)pipeline(c,96,336,24,30,14,56,stage.step,e.progress,measured&&state==botty::ActivityState::running,state==botty::ActivityState::done);
    else {char label[64];if(measured)std::snprintf(label,sizeof(label),"%s %.0f%%",stage.label,e.progress*100);else std::snprintf(label,sizeof(label),"%s",stage.label);c.rounded(96,344,14,14,7,stage.color);text(c,124,336,label,24,30,ink,bold);}
    if(!library)progress(c,96,390,860,8,state==botty::ActivityState::done?1:e.progress,state==botty::ActivityState::attention?warning:state==botty::ActivityState::done?success:accent,ink,31);
    char a[64],b[64],d[64],r[64],t[96],left[48];unsigned x=96;
    botty::formatBytes(e.bytes,a,sizeof(a));botty::formatBytes(e.total,b,sizeof(b));botty::formatBytes(e.download,r,sizeof(r));timeLeft(e,left,sizeof(left));
    if(library){
        std::snprintf(d,sizeof(d),"%s",b);stat(c,x,d,"Size");
        stat(c,x,e.compressed?"Compressed":std::string_view(e.kind.data())=="folder"?"Folder":e.kind[0]?e.kind.data():"Unknown","Format");
        if(e.titleId[0])stat(c,x,e.titleId.data(),"Title ID");
        if(std::string_view(catalog.compressionJob.data())==e.id.data()&&catalog.compressionBusy&&catalog.compressionTotal>0){std::snprintf(d,sizeof(d),"%.0f%%",catalog.compressionBytes/catalog.compressionTotal*100);stat(c,x,d,"Compression");}
    }else if(f.torrent){
        std::snprintf(d,sizeof(d),"%s / %s",a,b);stat(c,x,d,"Downloaded");
        char up[48];botty::formatBytes(e.upload,up,sizeof(up));std::snprintf(d,sizeof(d),"%s/s",r);std::snprintf(t,sizeof(t),"Down \xc2\xb7 %s/s up",up);stat(c,x,d,t);
        if(e.peers<0){stat(c,x,"\xe2\x80\x94","Peers unavailable");}
        else {std::snprintf(d,sizeof(d),"%d",e.peers);if(e.downloadingPeers>=0)std::snprintf(t,sizeof(t),"Peers \xc2\xb7 %d sending",e.downloadingPeers);else std::snprintf(t,sizeof(t),"Peers");stat(c,x,d,t);}
        std::snprintf(t,sizeof(t),"Left \xc2\xb7 %s",location);stat(c,x,left[0]?left:e.complete?"Done":"\xe2\x80\x94",t);
    }else {
        if(e.items&&e.total>0)std::snprintf(d,sizeof(d),"%.0f / %.0f",e.bytes,e.total);else if(e.total>0)std::snprintf(d,sizeof(d),"%s / %s",a,b);else std::snprintf(d,sizeof(d),"\xe2\x80\x94");
        stat(c,x,d,e.task?(e.items?"Items":"Processed"):"Extracted");
        if(e.download>0){if(e.items)std::snprintf(d,sizeof(d),"%.1f/s",e.download);else std::snprintf(d,sizeof(d),"%s/s",r);stat(c,x,d,"Speed");}
        std::snprintf(d,sizeof(d),"%.0f s",e.elapsed);if(e.task)stat(c,x,d,"Elapsed");
        stat(c,x,left[0]?left:"\xe2\x80\x94","Left (current phase)");
    }
    // Buttons mirror Quick actions. Pause, Resume and Verify run immediately.
    detailActions.open(&e,workflowTab(f),catalog);
    unsigned bx=96,shown=0;
    for(unsigned i=0;i<detailActions.optionCount;++i){
        const auto op=detailActions.options[i];const char* label=buttonLabel(op);const bool risky=botty::Workflow::destructive(op);
        const bool pauseIcon=op==Op::pause;const unsigned w=56+width(c,label,24,bold)+(pauseIcon?32:0);
        if(bx+w>1824)break;
        const bool focus=model.detailButton==shown;const bool enabled=!*botty::unavailable(op,&e,catalog);
        if(focus){focusRing(c,bx,565,w,64,18,4,bg);c.rounded(bx,565,w,64,12,ink);}
        else if(risky)c.rounded(bx,565,w,64,12,danger,31);else c.rounded(bx,565,w,64,12,ink,26);
        unsigned tx=bx+28;
        if(pauseIcon){const Color icon=focus?bg:ink;c.rounded(tx+3,586,5,18,2,icon);c.rounded(tx+12,586,5,18,2,icon);tx+=32;}
        middle(c,static_cast<int>(tx),565,64,label,24,focus?bg:!enabled?faint:risky?danger:ink,bold);
        bx+=w+14;++shown;
    }
    model.buttonCount=shown;if(model.detailButton>=shown&&shown)model.detailButton=shown-1;
    const auto op=shown?detailActions.options[model.detailButton]:Op::none;const char* reason=op!=Op::none?botty::unavailable(op,&e,catalog):"";
    const char* note=detailNotice[0]?detailNotice.data():*reason?reason:op!=Op::none?explain(op,&e):e.task?"This task is followed here until it finishes.":"";
    wrap(c,96,656,note,20,24,1180,2,detailNotice[0]||*reason?warning:muted);
    // Files, destination and errors; up/down pages through them.
    std::array<std::string_view,40> rows{};unsigned rowCount=0;
    const auto addLines=[&](std::string_view value){while(!value.empty()&&rowCount<rows.size()){const auto n=value.find('\n');const auto row=n==std::string_view::npos?value:botty::slice(value,0,n);if(!row.empty())rows[rowCount++]=row;if(n==std::string_view::npos)break;value.remove_prefix(n+1);}};
    char title[64];
    if(e.error[0])addLines(e.error.data());
    if(f.torrent){std::snprintf(title,sizeof(title),"Files \xc2\xb7 %u",e.fileCount);addLines(e.files.data());}
    else {std::snprintf(title,sizeof(title),library?"Location":"Details");if(e.currentFile[0])addLines(e.currentFile.data());if(e.destination[0])addLines(e.destination.data());if(library&&catalog.library[0])addLines(catalog.library.data());if(e.phase[0]&&!e.task)addLines(e.phase.data());}
    if(rowCount){
        const unsigned pages=(rowCount+2)/3;if(model.detailPage>=pages){model.detailPage=pages-1;++displayRevision;}
        eyebrow(c,96,760,title,muted);
        if(pages>1){char more[48];std::snprintf(more,sizeof(more),"%u / %u \xc2\xb7 \xe2\x86\x93 to see more",model.detailPage+1,pages);text(c,static_cast<int>(956-width(c,more,20)),760,more,20,24,faint);}
        for(unsigned i=0;i<3&&model.detailPage*3+i<rowCount;++i){
            const unsigned y=796+i*52;c.rectangle(96,y,860,1,line);
            const auto row=rows[model.detailPage*3+i];const bool error=e.error[0]&&row.data()==e.error.data();
            fit(c,96,static_cast<int>(y+11),row,24,30,860,error?warning:i==2?muted:ink);
            if(i==2||model.detailPage*3+i+1==rowCount)c.rectangle(96,y+52,860,1,line);
        }
    }else model.detailPage=0;
    unsigned hx=96;hx=hint(c,hx,Glyph::cross,"Select",true);hx=hint(c,hx,Glyph::menu,"Quick actions");hint(c,hx,Glyph::circle,library?"Back to Library":"Back to Activity");
    footerRight(c,nullptr);
}

// --- Library ------------------------------------------------------------------
void drawLibrary(Canvas& c,const char* detail) noexcept {
    text(c,96,148,"Library",60,66,ink,bold,-2);
    double bytes=0;const unsigned all=catalog.valid?botty::libraryCount(catalog,0):0;
    for(unsigned i=0;i<all;++i)bytes+=botty::libraryAt(catalog,0,i)->total;
    char subtitle[160],size[48];botty::formatBytes(bytes,size,sizeof(size));
    std::snprintf(subtitle,sizeof(subtitle),"%u game%s \xc2\xb7 %s \xc2\xb7 play them from the PS5 Home screen",all,all==1?"":"s",size);
    text(c,96,224,catalog.valid?subtitle:detail,24,30,catalog.valid?muted:warning);
    const char* filters[]={"All","PS5 SSD","External","Compressed"};
    unsigned total=0;for(auto name:filters)total+=44+width(c,name,20,bold)+12;
    unsigned x=1824-total+12;
    for(unsigned f=0;f<4;++f)x+=chip(c,x,202,filters[f],model.filter==f)+12;
    if(!catalog.valid)drawEmpty(c,296,"Your session is offline",detail);
    else if(!model.count)drawEmpty(c,296,model.filter?"No games match this filter.":"Your next game belongs here.",model.filter?"Triangle changes the filter.":"Games you download and prepare appear here, ready to open from the PS5 Home screen.");
    const unsigned row=model.selected/Model::libraryColumns;
    for(unsigned r=0;r<2;++r)for(unsigned col=0;col<Model::libraryColumns;++col){
        const unsigned index=(row+r)*Model::libraryColumns+col;if(index>=model.count)break;
        const auto* e=botty::libraryAt(catalog,model.filter,index);if(!e)break;
        const unsigned cx=97+col*250,cy=296+r*464;const bool focus=index==model.selected;
        std::array<botty::LibraryCopy,2> copies{};const unsigned copyCount=botty::libraryCopies(*e,copies);
        char first[96],second[96],line[200],total[48];storageName(copies[0].storage,first,sizeof(first));botty::formatBytes(e->total,total,sizeof(total));
        bool offline=std::string_view(first).starts_with("Offline");
        if(copyCount==2){storageName(copies[1].storage,second,sizeof(second));offline=offline&&std::string_view(second).starts_with("Offline");
            if(std::string_view(first)==second)std::snprintf(line,sizeof(line),"%s \xc2\xb7 both formats",first);else std::snprintf(line,sizeof(line),"%s + %s",first,second);}
        else if(e->total>0)std::snprintf(line,sizeof(line),"%s \xc2\xb7 %s",first,total);
        else std::snprintf(line,sizeof(line),"%s",first);
        const auto status=std::string_view(e->status.data());
        if(std::string_view(catalog.compressionJob.data())==e->id.data()&&catalog.compressionBusy)std::snprintf(line,sizeof(line),"%s",catalog.compressionTotal>0?"Compressing\xe2\x80\xa6":"Compression pending");
        if(status=="moving")std::snprintf(line,sizeof(line),"Moving to Library");
        if(status=="ready")std::snprintf(line,sizeof(line),e->total>0?"Ready to move \xc2\xb7 %s":"Ready to move",total);
        if(offline)std::snprintf(line,sizeof(line),"Disk disconnected");
        if(focus)focusRing(c,cx,cy,216,324,18,5,bg);
        art(c,cx,cy,216,324,12,artworkId(*e,'j'),e->name.data(),index,bg,offline?102:255);
        if(offline)badge(c,cx+8,cy+8,"OFFLINE",warning,warning,41);
        else if(status=="move-error")badge(c,cx+8,cy+8,"MOVE FAILED",warning,warning,41);
        else if(status=="ready"||status=="moving")badge(c,cx+8,cy+8,status=="ready"?"READY":"MOVING",ink,bg,209);
        else if(copyCount==2)badge(c,cx+8,cy+8,"BOTH FORMATS",ink,bg,209);
        else if(e->compressed)badge(c,cx+8,cy+8,"COMPRESSED",ink,bg,209);
        fit(c,static_cast<int>(cx),static_cast<int>(cy+344),e->name.data(),24,30,216,offline?muted:ink,bold);
        fit(c,static_cast<int>(cx),static_cast<int>(cy+376),line,20,24,216,offline||status=="move-error"?warning:muted);
    }
    // The next row peeks out under the hint bar.
    c.fade(0,800,1920,140,bg,0,224,false);c.fade(0,940,1920,90,bg,224,255,false);c.rectangle(0,1030,1920,50,bg);
    unsigned hx=96;hx=hint(c,hx,Glyph::cross,"Details",true);hx=hint(c,hx,Glyph::menu,"Compress, move, delete\xe2\x80\xa6");hint(c,hx,Glyph::triangle,"Filter");
    char position[48];if(model.count)std::snprintf(position,sizeof(position),"%u of %u",model.selected+1,model.count);else position[0]=0;
    footerRight(c,position);
}

// --- System -------------------------------------------------------------------
void drawSystem(Canvas& c,bool online,bool managerOnline,bool updateStale,const char* detail) noexcept {
    text(c,96,148,"System",60,66,ink,bold,-2);
    text(c,96,224,"Remote access, updates and the Botty+ service",24,30,muted);
    c.rounded(96,300,1040,600,22,surface);
    eyebrow(c,152,356,"Open on your phone or computer",muted);
    const auto url=std::string_view(connection.url.data());
    const auto shown=url.starts_with("http://")?botty::slice(url,7):url;
    fit(c,152,400,online?(shown.empty()?"Address unavailable":shown):connection.status==botty::Probe::checking?"Connecting\xe2\x80\xa6":"Unavailable",60,72,640,ink,bold,-1);
    wrap(c,152,480,online?"Same Wi-Fi network \xc2\xb7 add a magnet or upload a .torrent from any browser":detail,24,34,600,2,online?muted:warning);
    const std::string_view password=connection.password.data();
    if(online&&password.size()>6)text(c,152,560,"Short password applies next session.",20,24,faint);
    (void)c.illustration(0,816,420,280,280);
    for(unsigned i=0;i<2;++i){
        const unsigned x=152+i*476;c.rounded(x,702,452,142,16,raised);
        eyebrow(c,static_cast<int>(x+32),730,i?"Password":"Username",muted);
        const std::string_view value=!online?std::string_view("\xe2\x80\x94"):i?password:std::string_view(connection.username.data());
        fit(c,static_cast<int>(x+32),762,value,value.size()>12?28:44,54,388,ink,bold,i?1:0);
    }
    const auto& update=catalog.nativeUpdate;
    const bool available=botty::nativeUpdateAvailable(update,updateStale),waiting=update.requested&&update.closeRequired&&std::string_view(update.status.data())!="blocked";
    c.rounded(1168,300,656,340,22,surface);
    const auto updateStatus=std::string_view(update.status.data());
    const bool upToDate=!updateStale&&update.supported&&!available&&(updateStatus=="current"||updateStatus=="complete");
    eyebrow(c,1208,340,available?"Update available":waiting?"Update queued":upToDate?"Up to date":"Updates",muted,true,available||waiting?accent:upToDate?success:faint);
    char current[32],next[32],title[96];publicVersion(botty::nativeVersion,current,sizeof(current));publicVersion(update.availableVersion.data(),next,sizeof(next));
    std::snprintf(title,sizeof(title),"Botty+ %s",available&&update.availableVersion[0]?next:botty::nativeDisplayVersion);
    text(c,1208,378,title,44,52,ink,bold,-1);
    char body[384];
    if(model.updateDialog)std::snprintf(body,sizeof(body),"App %s \xe2\x86\x92 %s \xc2\xb7 Manager %s \xe2\x86\x92 %s \xc2\xb7 Worker %s \xe2\x86\x92 %s \xc2\xb7 Engine %s \xe2\x86\x92 %s",current,next,update.installedServiceVersion.data(),update.availableServiceVersion.data(),update.installedWorkerVersion.data(),update.availableWorkerVersion.data(),update.installedEngineVersion.data(),update.availableEngineVersion.data());
    else if(available)std::snprintf(body,sizeof(body),"App %s \xe2\x86\x92 %s \xc2\xb7 Manager %s \xe2\x86\x92 %s. File jobs finish first; downloads pause only if rTorrent restarts.",current,next,update.installedServiceVersion.data(),update.availableServiceVersion.data());
    else std::snprintf(body,sizeof(body),"%s",!managerOnline?detail:update.message[0]?update.message.data():botty::nativeUpdateLabel(update,updateStale));
    wrap(c,1208,438,body,20,30,576,2,muted);
    if(model.updateDialog){
        // Installing closes Botty+, so Cancel keeps the focus until moved.
        for(unsigned i=0;i<2;++i){
            const unsigned x=1215+i*289,w=273;const bool focus=model.confirmUpdate==(i==1);
            if(focus)focusRing(c,x,533,w,68,18,4,surface);
            c.rounded(x,533,w,68,12,focus?(i?accent:ink):ink,focus?255:26);
            const char* label=i?"Install and close":"Cancel";
            middle(c,static_cast<int>(x+(w-width(c,label,24,bold))/2),533,68,label,24,focus?(i?accentInk:bg):ink,bold);
        }
    }else {
        const bool focus=model.selected==0;
        const char* label=std::string_view(update.status.data())=="blocked"?"Retry update check":waiting?"Close for update":available?"Update and close":"Check for updates";
        if(focus)focusRing(c,1215,533,562,68,18,4,surface);
        const bool primary=available||waiting;
        c.rounded(1215,533,562,68,12,primary?accent:ink,primary?255:26);
        const unsigned w=(primary?36:0)+width(c,label,28,bold),lx=1215+(562-w)/2;
        if(primary)glyph(c,Glyph::cross,static_cast<float>(lx),556,accentInk,22.f/24,3.5f);
        middle(c,static_cast<int>(lx+(primary?36:0)),533,68,label,28,primary?accentInk:ink,bold);
    }
    const char* rows[]={"Reconnect to the service","Close Botty+"};
    const char* stateText=online?"Connected":connection.status==botty::Probe::checking?"Connecting":managerOnline?"Reconnecting":"Offline";
    for(unsigned i=0;i<2;++i){
        const unsigned y=656+i*100;const bool focus=model.selected==i+1&&!model.updateDialog;
        if(focus)focusRing(c,1168,y,656,84,22,4,bg);
        c.rounded(1168,y,656,84,16,surface);
        middle(c,1200,static_cast<int>(y),84,rows[i],24,ink,bold);
        const char* right=i?"Downloads keep running":stateText;
        middle(c,static_cast<int>(1792-width(c,right,20)),static_cast<int>(y),84,right,20,i?muted:online?success:warning);
    }
    hint(c,96,Glyph::cross,model.updateDialog?"Confirm":"Select",true);
    const bool known=managerOnline&&!updateStale&&update.supported;
    char versions[384];std::snprintf(versions,sizeof(versions),"Manager %s \xc2\xb7 Worker %s \xc2\xb7 rTorrent %s",
        known&&update.installedServiceVersion[0]?update.installedServiceVersion.data():"unavailable",
        known&&update.installedWorkerVersion[0]?update.installedWorkerVersion.data():"unavailable",
        known&&update.installedEngineVersion[0]?update.installedEngineVersion.data():"unavailable");
    char label[48];std::snprintf(label,sizeof(label),"Botty+ %s",botty::nativeDisplayVersion);
    const unsigned version=width(c,label,20);
    text(c,static_cast<int>(1824-version),1006,label,20,24,faint);
    fit(c,static_cast<int>(1824-version-36-std::min(width(c,versions,20),1100U)),1006,versions,20,24,1100,faint);
}

// --- Sheets -------------------------------------------------------------------
void sheetFrame(Canvas& c) noexcept {c.rounded(0,0,1920,1080,0,bg,184);c.rectangle(1120,0,800,1080,surface);}
void sheetHeader(Canvas& c,const std::array<char,96>& cover,std::string_view name,const char* kicker,Color kickerColor,const char* line1,const char* line2,bool small) noexcept {
    const unsigned w=small?112:128,h=small?168:192,x=1192+w+32;
    if(cover[0]||!name.empty())art(c,1192,72,w,h,12,cover,{},0,surface);
    else {c.rounded(1192,72,w,h,12,raised);glyph(c,Glyph::menu,1192+w/2.f-18,72+h/2.f-18,muted,1.5f,3);}
    const unsigned block=24+10+50+(line1?10+(small?24:30):0)+(line2?34:0),top=72+h-(small?4:6)-block;
    eyebrow(c,static_cast<int>(x),static_cast<int>(top),kicker,kickerColor);
    fit(c,static_cast<int>(x),static_cast<int>(top+34),name,44,50,1848-x,ink,bold,-1);
    if(line1)fit(c,static_cast<int>(x),static_cast<int>(top+94),line1,small?20:24,small?24:30,1848-x,muted);
    if(line2)fit(c,static_cast<int>(x),static_cast<int>(top+128),line2,20,24,1848-x,muted);
}
// Left/right changes a focused row; from the button, up reaches the rows.
void sheetFooter(Canvas& c,const char* primary,Glyph change,const char* back) noexcept {
    unsigned x=1192;x=hint(c,x,Glyph::cross,primary,true,1008);
    if(change!=Glyph::none)x=hint(c,x,change,"Change",false,1008);
    hint(c,x,Glyph::circle,back,false,1008);
}
const char* ctaLabel(Op op) noexcept {
    switch(op){case Op::grab:case Op::exploreGrab:return "Get game";case Op::add:return "Start download";case Op::extract:return "Extract";case Op::transfer:return "Move";case Op::move:return "Move to Library";case Op::compress:return "Compress game";default:return "Continue";}
}
void drawSheet(Canvas& c) noexcept {
    sheetFrame(c);
    const auto op=workflow.command.operation;const auto* target=workflow.target(catalog);
    const bool download=op==Op::add||op==Op::grab||op==Op::exploreGrab;
    char line1[192]{},size[48];
    std::array<char,96> cover{};
    const char* kicker=op==Op::add?"Add a magnet link":op==Op::extract?"Extract":op==Op::transfer?"Move to another disk":op==Op::move?"Move to Library":op==Op::compress?"Compress game":"Get game";
    std::string_view name=workflow.targetName.data();
    if(op==Op::exploreGrab){
        if(model.selected<catalog.exploreCount&&!model.searchResults)cover=catalog.exploreResults[model.selected].id;
        const auto& source=workflow.sources[workflow.sourceIndex];botty::formatBytes(source.size,size,sizeof(size));
        std::snprintf(line1,sizeof(line1),"%s \xc2\xb7 PS5 \xc2\xb7 %u source%s",size,workflow.sourceCount,workflow.sourceCount==1?"":"s");
        if(!model.searchResults&&model.selected<catalog.exploreCount)name=catalog.exploreResults[model.selected].name.data();
    }else if(op==Op::grab){
        cover=artworkId(catalog.results[std::min(model.selected,99U)],'s');
        botty::formatBytes(workflow.sources[0].size,size,sizeof(size));std::snprintf(line1,sizeof(line1),"%s \xc2\xb7 search result",size);
    }else if(op==Op::add){name="Magnet link";std::snprintf(line1,sizeof(line1),"Added from a link you entered");}
    else if(target){
        cover=artworkId(*target,workflow.targetTab==0?'t':'j');botty::formatBytes(target->total,size,sizeof(size));
        if(op==Op::extract)std::snprintf(line1,sizeof(line1),"%s \xc2\xb7 %u file%s",size,target->fileCount,target->fileCount==1?"":"s");
        else {char where[96];storageName(target->storage.data(),where,sizeof(where));std::snprintf(line1,sizeof(line1),"%s \xc2\xb7 now on %s",size,where);}
    }
    sheetHeader(c,cover,name,kicker,accent,line1,nullptr,false);
    for(unsigned r=0;r<workflow.rowCount;++r){
        const unsigned y=320+r*130;const bool focus=workflow.focus==r;const auto row=workflow.rows[r];
        if(focus)focusRing(c,1192,y,656,120,22,5,surface);
        c.rounded(1192,y,656,120,14,raised);
        char label[64],value[512],sub[256];value[0]=sub[0]=0;unsigned index=0,count=0;bool showArrows=false;Color valueColor=ink;
        const char* tag=nullptr;Color tagColor=success;
        if(row==Row::source){
            std::snprintf(label,sizeof(label),"Source");const auto& s=workflow.sources[workflow.sourceIndex];
            std::snprintf(value,sizeof(value),"%s",s.tracker[0]?s.tracker.data():"Tracker");
            unsigned best=0;for(unsigned i=1;i<workflow.sourceCount;++i)if(workflow.sources[i].seeders>workflow.sources[best].seeders)best=i;
            if(workflow.sourceCount>1&&workflow.sourceIndex==best)tag="BEST";
            char seeds[24],bytes[48];grouped(s.seeders,seeds,sizeof(seeds));botty::formatBytes(s.size,bytes,sizeof(bytes));
            if(s.published[0])std::snprintf(sub,sizeof(sub),"%s seeders \xc2\xb7 %s \xc2\xb7 %.10s",seeds,bytes,s.published.data());else std::snprintf(sub,sizeof(sub),"%s seeders \xc2\xb7 %s",seeds,bytes);
            index=workflow.sourceIndex;count=workflow.sourceCount;showArrows=true;
        }else if(row==Row::magnet){
            std::snprintf(label,sizeof(label),"Magnet link");
            char used[24];grouped(static_cast<long long>(std::string_view(workflow.command.text.data()).size()),used,sizeof(used));
            std::snprintf(sub,sizeof(sub),"%s characters \xc2\xb7 select to edit",used);
        }else if(row==Row::storage){
            std::snprintf(label,sizeof(label),"%s",download?"Install on":op==Op::extract?"Extract to":op==Op::transfer?"Move to":op==Op::move?"Library disk":"Compressed copy on");
            const auto id=std::string_view(workflow.command.storage.data());
            for(unsigned i=0;i<catalog.storageCount;++i){const auto& d=catalog.storage[i];
                if(!d.available||(op==Op::transfer&&target&&d.id==target->storage))continue;
                if(id==d.id.data()){index=count;char free[48];botty::formatBytes(d.freeBytes,free,sizeof(free));
                    storageName(d.id.data(),value,sizeof(value));
                    const double needed=download?workflow.sources[workflow.sourceIndex].size:0;
                    if(download&&needed>0&&d.freeBytes>needed){char after[48];botty::formatBytes(d.freeBytes-needed,after,sizeof(after));std::snprintf(sub,sizeof(sub),"%s free \xc2\xb7 %s left after",free,after);}
                    else if(download&&needed>0)std::snprintf(sub,sizeof(sub),"%s free \xc2\xb7 not enough space",free);
                    else if(op==Op::extract&&target&&d.id==target->storage)std::snprintf(sub,sizeof(sub),"Same disk as the download \xc2\xb7 %s free",free);
                    else std::snprintf(sub,sizeof(sub),"%s free",free);}
                ++count;
            }
            if(!value[0]){std::snprintf(value,sizeof(value),"No disk available");std::snprintf(sub,sizeof(sub),"Connect a disk to continue");valueColor=warning;}
            showArrows=count>0;
        }else if(row==Row::mode){
            std::snprintf(label,sizeof(label),"After download");
            std::snprintf(value,sizeof(value),"%s",workflow.command.automatic?"Extract and add to Library":"Keep the download only");
            std::snprintf(sub,sizeof(sub),"%s",workflow.command.automatic?"Archives are kept for seeding":"Extract it later from Activity");
            index=workflow.command.automatic?0:1;count=2;showArrows=true;
        }else if(row==Row::archive){
            std::snprintf(label,sizeof(label),"Archive");
            if(target&&workflow.archiveIndex<target->archiveCount){
                const auto path=std::string_view(catalog.archives[target->archiveStart+workflow.archiveIndex].data());
                const auto slash=path.rfind('/');std::snprintf(value,sizeof(value),"%.*s",static_cast<int>(slash==std::string_view::npos?path.size():path.size()-slash-1),slash==std::string_view::npos?path.data():path.data()+slash+1);
                if(target->archiveCount==1){tag="AUTO";tagColor=muted;std::snprintf(sub,sizeof(sub),"Found automatically%s",target->archivesOmitted?" \xc2\xb7 some names exceed the list limit":"");}
                else std::snprintf(sub,sizeof(sub),"%u archive sets found%s",target->archiveCount,target->archivesOmitted?" \xc2\xb7 some names exceed the list limit":"");
                index=workflow.archiveIndex;count=target->archiveCount;showArrows=count>1;
            }else {std::snprintf(value,sizeof(value),"No archive available");valueColor=warning;}
        }else {
            std::snprintf(label,sizeof(label),"Password");const auto password=std::string_view(workflow.command.text.data());
            if(password.empty()){std::snprintf(value,sizeof(value),"None");valueColor=muted;std::snprintf(sub,sizeof(sub),"Only needed for locked archives");}
            else {
                if(workflow.passwordVisible)std::snprintf(value,sizeof(value),"%.*s",static_cast<int>(std::min<std::size_t>(password.size(),200)),password.data());
                else {const unsigned n=static_cast<unsigned>(std::min<std::size_t>(password.size(),20));for(unsigned i=0;i<n;++i)std::copy_n("\xe2\x80\xa2",3,value+i*3);value[n*3]=0;}
                std::snprintf(sub,sizeof(sub),"Entered with the PS5 keyboard");
            }
        }
        eyebrow(c,1220,static_cast<int>(y+13),label,muted);
        const unsigned rightWidth=row==Row::password?200:showArrows?132:0,limit=600-rightWidth-(tag?width(c,tag,20,bold,2)+32:0);
        if(row==Row::magnet)fitEnd(c,1220,static_cast<int>(y+43),workflow.command.text.data(),28,34,600,ink,bold);
        else {const unsigned w=fit(c,1220,static_cast<int>(y+43),value,28,34,limit,valueColor,bold,row==Row::password&&!workflow.passwordVisible?2:0);if(tag)badge(c,1220+w+12,y+45,tag,tagColor,tagColor,tagColor==success?36:20);}
        fit(c,1220,static_cast<int>(y+83),sub,20,24,600-rightWidth,muted);
        if(showArrows){
            char position[24];std::snprintf(position,sizeof(position),"%u / %u",count?index+1:0,count);
            const unsigned pw=width(c,position,20,bold);
            chevron(c,1806,y+49,true,index+1<count?ink:faint);
            text(c,static_cast<int>(1794-pw),static_cast<int>(y+48),position,20,24,muted,bold);
            chevron(c,static_cast<float>(1794-pw-26),y+49,false,index>0?ink:faint);
        }
        if(row==Row::password){
            // Square edits the password; Triangle reveals it.
            const bool empty=!workflow.command.text[0];unsigned hx=1820;
            if(!empty){const char* show=workflow.passwordVisible?"Hide":"Show";hx-=width(c,show,20);text(c,static_cast<int>(hx),static_cast<int>(y+48),show,20,24,muted);hx-=28;glyph(c,Glyph::triangle,static_cast<float>(hx),static_cast<float>(y+49),muted,20.f/24,2.4f);hx-=24;}
            const char* edit=empty?"Enter":"Edit";hx-=width(c,edit,20);text(c,static_cast<int>(hx),static_cast<int>(y+48),edit,20,24,muted);hx-=28;glyph(c,Glyph::square,static_cast<float>(hx),static_cast<float>(y+49),muted,20.f/24,2.4f);
        }
    }
    const unsigned cta=workflow.rowCount?320+workflow.rowCount*130+30:360;
    const bool ctaFocus=workflow.focus>=workflow.rowCount;
    if(ctaFocus)focusRing(c,1200,cta+8,640,84,20,5,surface);
    c.rounded(1200,cta+8,640,84,14,accent);
    const char* label=ctaLabel(op);const unsigned lw=26+16+width(c,label,32,bold),lx=1200+(640-lw)/2;
    glyph(c,Glyph::cross,static_cast<float>(lx),static_cast<float>(cta+37),accentInk,26.f/24,3.5f);
    middle(c,static_cast<int>(lx+42),static_cast<int>(cta+8),84,label,32,accentInk,bold);
    const char* note=workflow.notice[0]?workflow.notice.data():download?"Starts right away. Pause or remove it anytime in Activity.":explain(op,target);
    wrap(c,1192,static_cast<int>(cta+120),note,20,30,656,3,workflow.notice[0]?warning:muted);
    const bool row=!ctaFocus&&workflow.focus<workflow.rowCount;
    const auto focusedRow=row?workflow.rows[workflow.focus]:Row::source;
    const Glyph change=!row?(workflow.rowCount?Glyph::updown:Glyph::none):focusedRow==Row::password||focusedRow==Row::magnet?Glyph::none:Glyph::arrows;
    sheetFooter(c,!row?label:focusedRow==Row::password?"Enter password":focusedRow==Row::magnet?"Edit link":"Done",change,"Cancel");
}
// Confirmation fill: the sheet surface under a 8% danger tint.
constexpr Color confirmFill=rgb(37,24,31);
void drawMenu(Canvas& c) noexcept {
    sheetFrame(c);
    const auto* target=workflow.target(catalog);
    std::array<char,96> cover{};char line1[192]{},line2[192]{};
    if(target){
        cover=artworkId(*target,workflow.targetTab==0?'t':'j');
        char size[48];botty::formatBytes(target->total,size,sizeof(size));
        if(workflow.targetTab==2){
            std::array<botty::LibraryCopy,2> copies{};const unsigned n=botty::libraryCopies(*target,copies);
            for(unsigned i=0;i<n;++i){char where[96];storageName(copies[i].storage,where,sizeof(where));const bool sized=!i&&target->total>0;std::snprintf(i?line2:line1,sizeof(line1),"%s \xc2\xb7 %s%s%s",copies[i].format,where,sized?" \xc2\xb7 ":"",sized?size:"");}
        }else {char where[96];storageName(target->storage.data(),where,sizeof(where));describe(*target,workflow.targetTab==0,line1,sizeof(line1));if(target->total>0)std::snprintf(line2,sizeof(line2),"%s \xc2\xb7 %s",size,where);else std::snprintf(line2,sizeof(line2),"%s",where);}
    }
    sheetHeader(c,cover,workflow.targetName.data(),"Quick actions",muted,line1[0]?line1:nullptr,line2[0]?line2:nullptr,true);
    unsigned y=288;
    for(unsigned i=0;i<workflow.optionCount;++i){
        const auto op=workflow.options[i];const bool focus=workflow.selected==i;
        const char* reason=botty::unavailable(op,target,catalog);const bool enabled=!*reason;
        if(workflow.confirming&&focus){
            // Destructive choices confirm inline, with Cancel focused first.
            char explanation[400],estimate[160];
            if(op==Op::removeLibrary||op==Op::removeTorrent){botty::formatDeletionEstimate(target?(op==Op::removeLibrary?target->total:target->bytes):0,estimate,sizeof(estimate));std::snprintf(explanation,sizeof(explanation),"%s %s",explain(op,target),estimate);}
            else std::snprintf(explanation,sizeof(explanation),"%s",explain(op,target));
            const unsigned lines=wrap(c,1220,0,explanation,20,30,600,4,ink,regular,false);
            const unsigned h=28+34+10+lines*30+22+74+28;
            c.rounded(1192,y,656,h,16,danger,115);c.rounded(1194,y+2,652,h-4,14,confirmFill);
            char title[96];std::snprintf(title,sizeof(title),"%s?",botty::operationLabel(op));
            fit(c,1220,static_cast<int>(y+28),title,28,34,600,danger,bold);
            wrap(c,1220,static_cast<int>(y+72),explanation,20,30,600,4,ink);
            const unsigned row=y+72+lines*30+22;
            for(unsigned b=0;b<2;++b){
                const unsigned bx=1227+b*307;const bool chosen=workflow.confirm==(b==1);
                if(chosen)focusRing(c,bx,row+7,279,60,16,4,confirmFill);
                if(b)c.rounded(bx,row+7,279,60,10,danger);else c.rounded(bx,row+7,279,60,10,ink,chosen?255:26);
                const char* label=b?botty::operationLabel(op):"Cancel";
                fit(c,static_cast<int>(bx+(279-std::min(width(c,label,24,bold),255U))/2),static_cast<int>(row+7+15),label,24,30,255,b?dangerInk:chosen?bg:ink,bold);
            }
            y+=h+8;continue;
        }
        if(focus)focusRing(c,1192,y,656,76,20,4,surface);
        c.rounded(1192,y,656,76,14,raised);
        char right[96];right[0]=0;
        if(!enabled)std::snprintf(right,sizeof(right),"Unavailable");
        else if(op==Op::restoreOriginal)std::snprintf(right,sizeof(right),"Close Botty+ to finish");
        else if(op==Op::transfer){unsigned disks=0;for(unsigned d=0;d<catalog.storageCount;++d)if(catalog.storage[d].available&&!(target&&catalog.storage[d].id==target->storage))++disks;std::snprintf(right,sizeof(right),"%u other disk%s",disks,disks==1?"":"s");}
        else if(op==Op::extract&&target)std::snprintf(right,sizeof(right),"%u archive%s",target->archiveCount,target->archiveCount==1?"":"s");
        else if(botty::Workflow::immediate(op))std::snprintf(right,sizeof(right),"Runs right away");
        const unsigned rw=width(c,right,20);
        fit(c,1220,static_cast<int>(y+23),botty::operationLabel(op),24,30,600-rw-16,enabled?botty::Workflow::destructive(op)?danger:ink:faint,bold);
        if(right[0])text(c,static_cast<int>(1820-rw),static_cast<int>(y+26),right,20,24,enabled?muted:faint);
        y+=84;
    }
    const auto selectedOp=workflow.selected<workflow.optionCount?workflow.options[workflow.selected]:Op::none;
    const char* reason=selectedOp!=Op::none?botty::unavailable(selectedOp,target,catalog):"";
    const char* note=workflow.notice[0]?workflow.notice.data():*reason?reason:workflow.confirming?"":explain(selectedOp,target);
    wrap(c,1192,static_cast<int>(y+16),note,20,30,656,3,workflow.notice[0]||*reason?warning:muted);
    sheetFooter(c,workflow.confirming?"Confirm choice":"Select",workflow.confirming?Glyph::arrows:Glyph::none,workflow.confirming?"Back":"Close");
}
void drawToast(Canvas& c,bool sheet) noexcept {
    if(!toast.visible)return;
    const bool error=toast.kind==Toast::Kind::error;const unsigned x=sheet?96:1184,w=640;
    const bool cover=!error&&coverFor(toast.cover);
    const unsigned textX=x+24+(cover?58:0),limit=x+w-24-(error?0:30)-textX;
    const unsigned lines=wrap(c,static_cast<int>(textX),0,toast.message.data(),20,24,limit,error?4:2,ink,regular,false);
    const unsigned h=20+30+2+lines*24+(error?34:0)+20,y=996-24-h;
    if(error){c.rounded(x,y,w,h,16,danger,115);c.rounded(x+2,y+2,w-4,h-4,14,bg);c.rounded(x+2,y+2,w-4,h-4,14,danger,26);}
    else c.rounded(x,y,w,h,16,raised);
    if(cover)art(c,x+24,y+(h-60)/2,40,60,6,toast.cover,{},0,raised);
    fit(c,static_cast<int>(textX),static_cast<int>(y+20),toast.title.data(),24,30,limit,error?danger:ink,bold);
    wrap(c,static_cast<int>(textX),static_cast<int>(y+52),toast.message.data(),20,24,limit,error?4:2,error?ink:muted);
    if(error)hint(c,textX,Glyph::circle,"Dismiss",false,y+52+lines*24+8);
    else c.rounded(x+w-34,y+h/2-5,10,10,5,toast.kind==Toast::Kind::success?success:warning);
}

bool nativeInputPending() noexcept {return textEntryState==TextEntryState::accepted||textEntryState==TextEntryState::capacity;}
bool finishNativeInput(unsigned edge,bool busy,bool snapshotKnown=true) noexcept {
    // Circle backs out like the keyboard panel does: a password edit returns to Extract.
    if(edge&botty::Buttons::circle)(void)workflow.press(botty::Buttons::circle,catalog,busy);
    else if(textEntryState==TextEntryState::capacity){
        const auto text=nativeKeyboard.text();workflow.command.text.fill(0);
        std::copy(text.begin(),text.end(),workflow.command.text.begin());workflow.selected=0;
        std::snprintf(workflow.notice.data(),workflow.notice.size(),"PS5 keyboard limit reached. Check the full magnet link in the in-app keyboard before continuing.");
        nativeKeyboard.clearText();textEntryState=TextEntryState::fallback;++displayRevision;return false;
    }
    else if(!snapshotKnown)return false;
    else if(busy){workflow.finishInput(catalog,true);return false;}
    else {
        const bool emit=workflow.acceptText(nativeKeyboard.text(),catalog,false);
        nativeKeyboard.clearText();++displayRevision;textEntryState=workflow.panel==Panel::keyboard?TextEntryState::ready:TextEntryState::idle;return emit;
    }
    nativeKeyboard.clearText();++displayRevision;textEntryState=TextEntryState::idle;return false;
}
void submitWorkflow(bool& quiet) noexcept {
    const auto op=workflow.command.operation;
    std::array<char,96> cover{};std::string_view name=workflow.targetName.data();
    if(op==Op::exploreGrab&&model.selected<catalog.exploreCount)cover=catalog.exploreResults[model.selected].id;
    else if(op==Op::grab&&model.selected<catalog.resultCount)cover=artworkId(catalog.results[model.selected],'s');
    else if(const auto* target=workflow.target(catalog))cover=artworkId(*target,workflow.targetTab==0?'t':'j');
    if(op==Op::add)name="Magnet link";
    if(op==Op::search)quiet=true;
    if(send(workflow.command,name,cover)){
        if(op==Op::search){rememberSearch(workflow.command.text.data());model.tab=Model::discover;model.searchResults=true;model.selected=0;}
        // File operations continue in Activity, where their progress is shown.
        if(op==Op::transfer||op==Op::move||op==Op::extract||op==Op::compress||op==Op::remove||op==Op::removeLibrary||op==Op::removeTorrent||op==Op::removeOriginal||op==Op::dismiss){model.tab=Model::activity;model.selected=model.filter=0;model.details=false;}
    }else quiet=false;
    workflow.command.text.fill(0);
}

bool draw(Canvas& c) noexcept {
    const auto now=botty::platform::now();
    if(c.take_resumed()) {
        input.reset();discardPadBatch=true;network.retry();
        botty::platform::log("VideoOut resumed - refreshing local service");
    }
    static bool exploreRequested=false,quietRequest=false,exploreRefresh=false,exploreVisited=false;
    static bool updateSubmitting=false;
    // The worker publishes result and busy under one gate. Consume that snapshot
    // before dispatching deferred input, so a toast reflects the finished request.
    const auto resultRevision=actionResult.revision;
    botty::ActionResult receivedResult=actionResult;
    bool inputNetworkBusy=false;
    const bool inputNetworkKnown=network.read(connection,nullptr,&receivedResult,&inputNetworkBusy);
    if(receivedResult.revision!=resultRevision){
        actionResult=receivedResult;
        if(updateSubmitting){updateSubmitting=false;if(receivedResult.status==botty::ActionResult::Status::success)return false;}
        if(!quietRequest||receivedResult.status!=botty::ActionResult::Status::success)resultToast(receivedResult);
        quietRequest=false;++displayRevision;
    }
    if(toast.visible&&toast.kind!=Toast::Kind::error&&now>=toast.until){toast.visible=false;++displayRevision;}
    const unsigned oldTab=model.tab;
    const bool keyboardWasActive=nativeKeyboard.active();
    const auto keyboardResult=nativeKeyboard.poll();
    unsigned edge=pollPad(now);
    if(keyboardWasActive){
        edge=0;input.reset();discardPadBatch=true;
        using Result=botty::NativeKeyboard::Result;
        if(keyboardResult==Result::accepted){
            textEntryState=TextEntryState::accepted;
        }else if(keyboardResult==Result::atCapacity){
            textEntryState=TextEntryState::capacity;
        }else if(keyboardResult==Result::cancelled){
            textEntryState=TextEntryState::ready;
            std::snprintf(workflow.notice.data(),workflow.notice.size(),"PS5 keyboard closed. Cross: Reopen or Square: Use in-app keyboard.");
        }
        else if(keyboardResult==Result::tooLong){
            textEntryState=TextEntryState::editing;
            std::snprintf(workflow.notice.data(),workflow.notice.size(),"Input exceeds the byte limit. Shorten it in the PS5 keyboard.");
        }
        else if(keyboardResult==Result::failed){
            textEntryState=TextEntryState::fallback;
            std::snprintf(workflow.notice.data(),workflow.notice.size(),"PS5 keyboard could not accept this input. Use the in-app keyboard.");
        }
        if(keyboardResult!=Result::pending)++displayRevision;
    }
    if(workflow.panel!=Panel::keyboard)textEntryState=TextEntryState::idle;
    if(edge)++displayRevision;
    // A failure stays visible until Circle, and that press only dismisses it.
    if(toast.visible&&toast.kind==Toast::Kind::error&&(edge&botty::Buttons::circle)){toast.visible=false;edge&=~botty::Buttons::circle;}
    if(workflow.panel!=Panel::closed){
        unsigned workflowEdge=edge;
        if(workflow.panel==Panel::keyboard&&textEntryState!=TextEntryState::fallback){
            workflowEdge=edge&botty::Buttons::circle;
            if(!nativeKeyboard.active()&&!nativeInputPending()&&(edge&botty::Buttons::square)){textEntryState=TextEntryState::fallback;workflow.notice.fill(0);}
            if(!nativeKeyboard.active()&&!nativeInputPending()&&(edge&(botty::Buttons::cross|botty::Buttons::options)))textEntryState=TextEntryState::idle;
        }
        const bool emit=nativeInputPending()?finishNativeInput(edge,inputNetworkBusy,inputNetworkKnown):workflow.press(workflowEdge,catalog,network.busy());
        if(emit)submitWorkflow(quietRequest);
    }else {
        using Action=Model::Action;
        if(edge)detailNotice.fill(0);
        const auto action=model.press(edge);
        if(action==Action::retry)network.retry();
        if(action==Action::update){
            if(catalog.nativeUpdate.requested&&catalog.nativeUpdate.closeRequired&&std::string_view(catalog.nativeUpdate.status.data())!="blocked"){
                if(!network.busy())return false;
                notify(Toast::Kind::error,"Close Botty+ later","Wait for the pending request before closing.");
            }
            else if(botty::nativeUpdateAvailable(catalog.nativeUpdate,catalog.stale)){model.updateDialog=true;model.confirmUpdate=false;}
            else {
                botty::Command command;command.operation=Op::checkNativeUpdate;
                const auto reason=botty::unavailable(command.operation,nullptr,catalog);
                if(*reason)notify(Toast::Kind::error,"Request not sent",reason);else (void)send(command,"Botty+",{});
            }
        }
        if(action==Action::installUpdate){
            botty::Command command;command.operation=Op::nativeUpdate;
            command.serviceVersion=catalog.nativeUpdate.installedServiceVersion;
            const auto reason=botty::unavailable(command.operation,nullptr,catalog);
            if(*reason)notify(Toast::Kind::error,"Request not sent",reason);
            else if(send(command,"Botty+",{}))updateSubmitting=true;
        }
        if(action==Action::quit){if(!network.busy())return false;notify(Toast::Kind::error,"Close Botty+ later","Wait for the pending request before closing.");}
        if(action==Action::search){
            if(!catalog.valid||!catalog.searchSupported)notify(Toast::Kind::notice,"Search unavailable",botty::unavailable(Op::search,nullptr,catalog));
            else workflow.search(model.searchResults?std::string_view(catalog.searchQuery.data()):std::string_view{});
        }
        if(action==Action::get||action==Action::compare){
            if(model.searchResults){
                if(model.selected<catalog.resultCount){
                    const auto& result=catalog.results[model.selected];
                    if(result.complete)notify(Toast::Kind::notice,"Already added","This result is already in Botty+. Follow it in Activity.");
                    else workflow.grab(result,catalog);
                }
            }else if(exploreCurrent()&&model.selected<catalog.exploreCount)workflow.chooseSources(catalog.exploreResults[model.selected],catalog,action==Action::compare);
        }
        if(action==Action::explore){exploreRequested=true;exploreRefresh=false;}
        if(model.tab==Model::discover&&!model.searchResults&&(edge&r3Button)){exploreRequested=true;exploreRefresh=true;}
        if(action==Action::menu){
            const auto f=focused();
            if(f.entry){workflow.open(f.entry,workflowTab(f),catalog);if(workflow.panel==Panel::closed)notify(Toast::Kind::notice,"No quick actions",f.entry->task?"This task is followed in Activity until it finishes.":"Nothing can be changed here right now.");}
        }
        if(action==Action::add){
            const char* reason=botty::unavailable(Op::add,nullptr,catalog);
            if(*reason)notify(Toast::Kind::notice,"Magnet links unavailable",reason);else workflow.add();
        }
        if(action==Action::run){
            const auto f=focused();
            // Resolve the button from the current entry, not from the last painted frame.
            if(f.entry)detailActions.open(f.entry,workflowTab(f),catalog);
            if(f.entry&&model.detailButton<detailActions.optionCount&&model.detailButton<model.buttonCount){
                const auto op=detailActions.options[model.detailButton];
                workflow.open(f.entry,workflowTab(f),catalog);
                if(workflow.choose(op,catalog,network.busy()))submitWorkflow(quietRequest);
                else if(workflow.panel==Panel::menu&&!workflow.confirming){detailNotice.fill(0);std::copy_n(workflow.notice.begin(),detailNotice.size()-1,detailNotice.begin());workflow.close();}
            }
        }
    }
    if(workflow.panel==Panel::keyboard&&textEntryState==TextEntryState::idle&&!nativeKeyboard.active()){
        textEntryState=TextEntryState::editing;
        const auto op=workflow.command.operation;
        const bool password=op==Op::extract,url=op==Op::add;
        const unsigned limit=botty::Workflow::textLimit(op);
        if(!nativeKeyboard.open(workflow.command.text.data(),password?"Archive password":url?"Add a magnet link":"Search all trackers",limit,password,url)){
            textEntryState=TextEntryState::fallback;
            std::snprintf(workflow.notice.data(),workflow.notice.size(),"PS5 keyboard unavailable. Use the in-app keyboard.");
        }else workflow.notice.fill(0);
        input.reset();discardPadBatch=true;++displayRevision;
    }
    // Keep the focused row selected when a refresh reorders the lists.
    std::array<char,96> focusedId{};bool focusedTorrent=false;
    {const auto f=focused();if(f.entry){focusedId=f.entry->id;focusedTorrent=f.torrent;}}
    if(model.tab==Model::discover){
        if(model.searchResults){if(model.selected<catalog.resultCount)focusedId=catalog.results[model.selected].id;}
        else if(exploreCurrent()&&model.selected<catalog.exploreCount)focusedId=catalog.exploreResults[model.selected].id;
    }
    const auto previousRevision=catalog.revision;
    (void)network.read(connection,&catalog);
    static botty::Processing processing;
    const bool processingChanged=network.readProcessing(processing);
    if(processingChanged)++displayRevision;
    catalog.processing=processing;
    static bool wasBusy=false;if(wasBusy!=network.busy()){wasBusy=network.busy();++displayRevision;}
    const auto nextDeletion=network.deletion();
    if(deletion!=nextDeletion){deletion=nextDeletion;++displayRevision;}
    // Processing refreshes reorder Activity independently of the catalog.
    const bool changed=previousRevision!=catalog.revision||processingChanged;
    if(previousRevision!=catalog.revision)++displayRevision;
    if(model.tab==Model::activity){
        model.count=(catalog.valid||catalog.processing.count)?botty::activityCount(catalog,model.filter):0;
        if(changed&&focusedId[0]){
            bool found=false;
            for(unsigned i=0;i<model.count;++i){const auto item=botty::activityAt(catalog,model.filter,i);if(item.entry&&item.entry->id==focusedId&&item.torrent==focusedTorrent){model.selected=i;found=true;break;}}
            if(!found)model.details=false;
        }
    }else if(model.tab==Model::library){
        model.count=catalog.valid?botty::libraryCount(catalog,model.filter):0;
        if(changed&&focusedId[0]){
            bool found=false;
            for(unsigned i=0;i<model.count;++i)if(botty::libraryAt(catalog,model.filter,i)->id==focusedId){model.selected=i;found=true;break;}
            if(!found)model.details=false;
        }
    }else if(model.tab==Model::discover){
        if((!exploreVisited||oldTab!=Model::discover)&&!exploreCurrent()){exploreRequested=true;exploreRefresh=false;}
        exploreVisited=true;
        if(exploreRequested&&catalog.valid&&catalog.exploreSupported&&!catalog.exploreBusy&&!catalog.exploreAdding&&!network.busy()&&workflow.panel==Panel::closed){
            botty::Command command;command.operation=Op::explore;command.refresh=exploreRefresh;std::snprintf(command.text.data(),command.text.size(),"%s",Model::exploreSorts[model.exploreSort]);
            if(network.submit(command)){exploreRequested=false;quietRequest=true;sentOperation=Op::explore;}
        }
        if(model.searchResults){
            model.count=catalog.resultCount;
            if(changed&&focusedId[0])for(unsigned i=0;i<model.count;++i)if(catalog.results[i].id==focusedId){model.selected=i;break;}
        }else {
            model.count=exploreCurrent()?catalog.exploreCount:0;
            if(changed&&focusedId[0])for(unsigned i=0;i<model.count;++i)if(catalog.exploreResults[i].id==focusedId){model.selected=i;break;}
        }
    }else model.count=Model::systemItems;
    if(model.selected>=model.count)model.selected=model.count?model.count-1:0;
    if(model.details&&!focused().entry)model.details=false;
    {
        botty::CoverIds ids{};std::array<std::string_view,botty::coverSlots> names{};
        if(model.tab==Model::discover&&!model.searchResults){for(unsigned i=0;i<9&&model.selected+i<model.count;++i){ids[i]=catalog.exploreResults[model.selected+i].id;names[i]=catalog.exploreResults[model.selected+i].name.data();}}
        else if(catalog.catalogArtworkSupported&&model.tab==Model::discover){const unsigned first=(model.selected/5)*5;for(unsigned i=0;i<5&&first+i<model.count;++i){ids[i]=artworkId(catalog.results[first+i],'s');names[i]=catalog.results[first+i].name.data();}}
        else if(catalog.catalogArtworkSupported&&model.tab==Model::activity){
            const unsigned first=(model.selected/5)*5;
            for(unsigned i=0;i<5&&first+i<model.count;++i){const auto item=botty::activityAt(catalog,model.filter,first+i);if(item.entry){ids[i]=artworkId(*item.entry,item.torrent?'t':'j');names[i]=item.entry->name.data();}}
        }else if(catalog.catalogArtworkSupported&&model.tab==Model::library){
            const unsigned first=(model.selected/Model::libraryColumns)*Model::libraryColumns;
            for(unsigned i=0;i<botty::coverSlots&&first+i<model.count;++i){const auto* entry=botty::libraryAt(catalog,model.filter,first+i);if(entry){ids[i]=artworkId(*entry,'j');names[i]=entry->name.data();}}
        }
        artwork.request(ids);if(artwork.read(covers))++displayRevision;
#ifdef BOTTY_HOST_PREVIEW
        // Cover fixtures are matched by title, so a cover never lands on another game:
        // "Astro Bot" loads <BOTTY_PREVIEW_COVERS>/astro-bot.rgb when it exists.
        const char* directory=std::getenv("BOTTY_PREVIEW_COVERS");
        for(unsigned i=0;directory&&i<ids.size();++i){
            if(!ids[i][0]||coverFor(ids[i]))continue;
            char slug[160]{};unsigned length=0;bool dash=false;
            for(char ch:names[i]){
                const char lower=ch>='A'&&ch<='Z'?static_cast<char>(ch-'A'+'a'):ch;
                const bool keep=(lower>='a'&&lower<='z')||(lower>='0'&&lower<='9');
                if(keep){if(dash&&length&&length+1<sizeof(slug))slug[length++]='-';if(length+1<sizeof(slug))slug[length++]=lower;dash=false;}else dash=true;
            }
            char path[256];std::snprintf(path,sizeof(path),"%s/%s.rgb",directory,slug);
            FILE* file=std::fopen(path,"rb");if(!file)continue;
            covers.ready[i]=std::fread(covers.pixels[i].data(),1,covers.pixels[i].size(),file)==covers.pixels[i].size();std::fclose(file);covers.ids[i]=ids[i];
        }
#endif
    }
    static auto previousPanel=Panel::closed;
    if(previousPanel!=workflow.panel){previousPanel=workflow.panel;
        const char* panels[]={"Workflow: closed","Workflow: quick actions","Workflow: sheet","Workflow: keyboard"};
        const auto index=static_cast<unsigned>(previousPanel);
        botty::platform::log(index<sizeof(panels)/sizeof(panels[0])?panels[index]:"Workflow: unknown panel");
    }
    const auto status=connection.status;
    if(!c.needs_update(displayRevision))return true;
    const bool online=status==botty::Probe::ready;
    const bool managerOnline=online||status==botty::Probe::transmissionUnavailable;
    const bool updateStale=catalog.stale||!catalog.valid||!managerOnline;
    const char* state=deletion!=botty::Network::Deletion::idle?"Deleting files":network.busy()?"Sending request":catalog.stale?"Reconnecting":online?"Connected":status==botty::Probe::checking?"Connecting":
        status==botty::Probe::legacy?"Update required":status==botty::Probe::transmissionUnavailable?"Reconnecting":"Offline";
    const Color stateColor=deletion!=botty::Network::Deletion::idle||network.busy()?accent:catalog.stale?warning:online?success:status==botty::Probe::checking||status==botty::Probe::transmissionUnavailable?warning:danger;
    const char* detail=status==botty::Probe::legacy?"Update Botty from the portal to show your login details.":
        status==botty::Probe::unavailable?"Open the Botty portal and select Start session.":
        status==botty::Probe::transmissionUnavailable?"rTorrent is not responding. Retrying automatically.":
        status==botty::Probe::rejected?"Access rejected. Reconnect from System.":
        status==botty::Probe::incompatible?"Update Botty from the portal.":
        status==botty::Probe::workerError?"Close and reopen Botty+.":
        status==botty::Probe::malformed?"Invalid response from Botty. Reconnect from System.":
        status==botty::Probe::checking?"Connecting to Botty on your PS5\xe2\x80\xa6":"";
    c.clear(bg);
    if(workflow.panel==Panel::keyboard&&textEntryState==TextEntryState::fallback)drawKeyboard(c);
    else {
        if(workflow.panel==Panel::keyboard)drawTyping(c);
        else if(model.tab==Model::discover){
            if(model.searchResults)drawSearch(c);
            else {
                const char* exploreState=!catalog.exploreSupported?"Update the Botty service to enable Discover.":catalog.exploreBusy||exploreRequested?"Loading this ranking\xe2\x80\xa6":catalog.exploreError[0]?catalog.exploreError.data():"No new PS5 games found for this ranking.";
                drawDiscover(c,exploreRequested||quietRequest,catalog.valid?exploreState:detail);
            }
        }else if(model.tab==Model::system)drawSystem(c,online,managerOnline,updateStale,detail);
        else if(model.details)drawDetails(c,focused());
        else if(model.tab==Model::activity)drawActivity(c,detail);
        else drawLibrary(c,detail);
        header(c,state,stateColor);
        if(catalog.transferring&&workflow.panel==Panel::closed&&!model.details&&model.tab!=Model::system)
            fit(c,760,1006,catalog.transferError[0]?catalog.transferError.data():catalog.transferPhase.data(),20,24,560,catalog.transferError[0]?warning:muted);
        if(workflow.panel==Panel::menu)drawMenu(c);
        else if(workflow.panel==Panel::sheet)drawSheet(c);
    }
    drawToast(c,workflow.panel==Panel::menu||workflow.panel==Panel::sheet);
    return true;
}
}
int main() {
#ifdef BOTTY_HOST_PREVIEW
    const char* previewMode=std::getenv("BOTTY_PREVIEW_MODE");
    const std::string_view mode=previewMode?previewMode:"";
    if(mode=="discover"||mode=="explore"||mode=="sources"||mode=="storage"||mode=="download-mode"||mode=="get-game"||mode=="compare")model.tab=Model::discover;
    else if(mode=="search"||mode=="search-typing"){model.tab=Model::discover;model.searchResults=true;}
    else if(mode=="library"||mode=="delete-game"||mode=="checking-game-deletion"||mode=="quick-actions"||mode=="library-details")model.tab=Model::library;
    else if(mode=="connections"||mode=="system"||mode=="quit"||mode=="exit"||mode.starts_with("update-"))model.tab=Model::system;
    else model.tab=Model::activity;
#endif
    // A fresh per-launch log stays bounded; no access to /data or credentials.
    const int fd=sceKernelOpen("/download0/botty-native-network.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0)(void)sceKernelClose(fd);
    char startup[96];std::snprintf(startup,sizeof(startup),"Botty+ %s (PS5 %s) - main entered",botty::nativeDisplayVersion,botty::nativeVersion);
    botty::platform::log(startup);
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
