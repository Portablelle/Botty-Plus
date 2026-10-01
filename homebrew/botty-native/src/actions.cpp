// SPDX-License-Identifier: GPL-3.0-or-later
#include "actions.hpp"
#include <cstdio>
namespace botty {
const char* operationLabel(Operation op) noexcept {
 switch(op){case Operation::removeTorrent:return "Delete torrent & files";case Operation::explore:return "Explore games";case Operation::search:return "Search games";case Operation::exploreGrab:case Operation::grab:return "Download and prepare";case Operation::pause:return "Pause";case Operation::resume:return "Resume";case Operation::verify:return "Verify files";case Operation::add:return "Add magnet";case Operation::extract:return "Extract";case Operation::move:return "Move to library";case Operation::remove:return "Delete extraction";case Operation::cancel:return "Cancel extraction";case Operation::dismiss:return "Remove from Extracted";default:return "Actions";}
}
const char* actionPath(Operation op) noexcept {
 switch(op){case Operation::explore:return "/api/explore";case Operation::exploreGrab:return "/api/explore/add";case Operation::search:return "/api/search";case Operation::grab:return "/api/search/add";case Operation::extract:return "/api/extract";case Operation::move:return "/api/move";case Operation::remove:return "/api/delete-extraction";case Operation::cancel:return "/api/cancel-extraction";case Operation::dismiss:return "/api/dismiss-extraction";default:return "/api/torrent";}
}
bool encodeCommand(const Command& cmd,char* out,std::size_t capacity,std::size_t& length) noexcept {
 length=0;bool ok=true;
 const auto append=[&](std::string_view s){for(char c:s){if(length+1>=capacity){ok=false;return;}out[length++]=c;}};
 const auto quote=[&](std::string_view s){append("\"");for(unsigned char c:s){if(c=='"')append("\\\"");else if(c=='\\')append("\\\\");else if(c<32){char b[7];std::snprintf(b,sizeof(b),"\\u%04x",c);append(b);}else {char b=static_cast<char>(c);append({&b,1});}}append("\"");};
 if(cmd.operation==Operation::none)return false;
 append("{");
 if(cmd.operation==Operation::explore){const auto sort=std::string_view(cmd.text.data());if(sort!="seeders"&&sort!="completed"&&sort!="newest")return false;append("\"sort\":");quote(sort);if(cmd.refresh)append(",\"refresh\":true");}
 else if(cmd.operation==Operation::search){if(!cmd.text[0]||std::string_view(cmd.text.data()).size()>200)return false;append("\"query\":");quote(cmd.text.data());}
 else if(cmd.operation==Operation::grab||cmd.operation==Operation::exploreGrab){if(std::string_view(cmd.id.data()).size()!=32)return false;append("\"id\":");quote(cmd.id.data());}
 else if(cmd.operation==Operation::add){if(!std::string_view(cmd.text.data()).starts_with("magnet:?"))return false;append("\"action\":\"add\",\"magnet\":");quote(cmd.text.data());}
 else {
  append("\"id\":");
  if(cmd.operation==Operation::move||cmd.operation==Operation::remove||cmd.operation==Operation::cancel||cmd.operation==Operation::dismiss){if(!cmd.id[0])return false;quote(cmd.id.data());}
  else {std::string_view id=cmd.id.data();if(id.empty()||id.size()>10||(id.size()>1&&id.front()=='0'))return false;unsigned long long value=0;for(char c:id){if(c<'0'||c>'9')return false;value=value*10+c-'0';}if(value>2147483647)return false;append(id);}
  if(cmd.operation==Operation::extract){if(!cmd.archive[0]||std::string_view(cmd.text.data()).size()>1024)return false;append(",\"archive\":");quote(cmd.archive.data());append(",\"password\":");quote(cmd.text.data());}
  else if(cmd.operation!=Operation::move&&cmd.operation!=Operation::remove&&cmd.operation!=Operation::cancel&&cmd.operation!=Operation::dismiss){append(",\"action\":");if(cmd.operation==Operation::removeTorrent){quote("remove-data");append(",\"confirmed\":true");}else quote(cmd.operation==Operation::pause?"pause":cmd.operation==Operation::resume?"resume":"verify");}
 }
 append("}");if(length<capacity)out[length]=0;return ok;
}
const char* unavailable(Operation op,const Entry* e,const Catalog& c) noexcept {
 if(!c.valid)return "Reconnect to Botty before performing an action.";
 if(op==Operation::explore||op==Operation::exploreGrab){if(!c.exploreSupported)return "Update the Botty service to enable Explore.";if(c.exploreBusy||c.exploreAdding)return "Wait for the current Explore request.";return op==Operation::explore||c.transmissionReady?"":"Start Transmission from the portal first.";}
 if(op==Operation::search||op==Operation::grab){if(!c.searchSupported)return "Update the Botty service to enable search.";if(c.searchBusy||c.searchAdding)return "Wait for the current search or download request.";if(op==Operation::search)return "";return c.transmissionReady?"":"Start Transmission from the portal first.";}
 if(op==Operation::add)return c.transmissionReady?"":"Start Transmission from the portal first.";
 if(!e)return "This item is no longer available. Close this menu and refresh.";
 if(op==Operation::removeTorrent){if(!c.torrentRemovalSupported)return "Update Botty to enable torrent deletion.";if(!c.transmissionReady)return "Start Transmission from the portal first.";return c.extracting?"Wait for extraction to finish before deleting archives.":"";}
 if(op==Operation::pause||op==Operation::resume||op==Operation::verify||op==Operation::extract){
  if(!c.transmissionReady)return "Start Transmission from the portal first.";
  if(op==Operation::extract){if(c.extracting)return "Wait for the active extraction to finish.";if(!e->extractable)return "Wait until this torrent is complete, verified and error-free.";if(!e->archiveCount)return e->archivesOmitted?"Archive names exceed the display limit.":"No first RAR volume found in this torrent.";}
  return "";
 }
 if((op==Operation::cancel||op==Operation::dismiss)&&!c.extractionControls)return "Start the updated Botty service next session to use this action.";
 const auto status=std::string_view(e->status.data());
 if(op==Operation::cancel)return status=="extracting"?"":"This extraction is no longer running.";
 if(op==Operation::dismiss)return status=="ready"||status=="moved"||status=="failed"||status=="cancelled"||status=="interrupted"?"":"Only finished extractions can be removed from the list.";
 if(c.extracting)return "Wait for the active extraction to finish.";
 if(op==Operation::move){if(status!="ready")return "Only ready extractions can be moved.";if(!e->kind[0]||std::string_view(e->kind.data())=="unsupported")return "This extraction does not contain a supported game format.";}
 if(op==Operation::remove&&status!="ready"&&status!="failed"&&status!="interrupted"&&status!="cancelled")return "Moved or active extractions cannot be deleted here.";
 return "";
}
void Workflow::close() noexcept {panel=Panel::closed;unicodeInput=false;codepoint.fill(0);command.text.fill(0);notice.fill(0);++revision;}
const Entry* Workflow::target(const Catalog& c) const noexcept {const auto& list=targetTab==0?c.torrents:c.jobs;const auto count=targetTab==0?c.torrentCount:c.jobCount;for(unsigned i=0;i<count;++i)if(std::string_view(list[i].id.data())==targetId.data())return &list[i];return nullptr;}
void Workflow::open(const Entry* e,unsigned tab,const Catalog&) noexcept {
 close();panel=Panel::menu;selected=0;targetTab=tab;targetId.fill(0);targetName.fill(0);optionCount=0;
 if(e&&tab<3){targetId=e->id;targetName=e->name;if(tab==0){options[optionCount++]=e->active?Operation::pause:Operation::resume;options[optionCount++]=Operation::verify;options[optionCount++]=Operation::extract;options[optionCount++]=Operation::removeTorrent;}else{if(e->active)options[optionCount++]=Operation::cancel;options[optionCount++]=Operation::move;options[optionCount++]=Operation::remove;options[optionCount++]=Operation::dismiss;}}
 options[optionCount++]=Operation::add;
}
void Workflow::search() noexcept {close();command=Command{};command.operation=Operation::search;panel=Panel::keyboard;selected=0;keyPage=0;}
void Workflow::grab(const Entry& e,bool exploration) noexcept {close();command=Command{};command.operation=exploration?Operation::exploreGrab:Operation::grab;command.id=e.id;targetName=e.name;panel=Panel::confirm;confirm=false;}
void Workflow::add() noexcept {close();command=Command{};command.operation=Operation::add;std::snprintf(command.text.data(),command.text.size(),"magnet:?xt=urn:btih:");panel=Panel::keyboard;selected=0;keyPage=0;passwordVisible=false;}
std::string_view Workflow::keys(unsigned page) noexcept {
 switch(page%3){case 0:return "abcdefghijklmnopqrstuvwxyz0123456789-_.:";case 1:return "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:";default:return "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~01234567";}
}
void Workflow::append(char c) noexcept {
 if(unicodeInput){unsigned length=static_cast<unsigned>(std::string_view(codepoint.data()).size());if(length<6&&((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))){codepoint[length]=c;notice.fill(0);}else std::snprintf(notice.data(),notice.size(),"Enter up to six hexadecimal digits (0-9, A-F).");++revision;return;}
 auto length=std::string_view(command.text.data()).size();const unsigned limit=command.operation==Operation::search?200:command.operation==Operation::extract?1024:16384;if(length<limit){command.text[length]=c;command.text[length+1]=0;notice.fill(0);}else std::snprintf(notice.data(),notice.size(),"Maximum length reached.");++revision;}
void Workflow::erase() noexcept {if(unicodeInput){auto n=std::string_view(codepoint.data()).size();if(n)codepoint[n-1]=0;++revision;return;}auto length=std::string_view(command.text.data()).size();if(length){unsigned start=static_cast<unsigned>(length-1);while(start&&(static_cast<unsigned char>(command.text[start])&0xc0)==0x80)--start;for(unsigned i=start;i<length;++i)command.text[i]=0;}notice.fill(0);++revision;}
bool Workflow::finishUnicode() noexcept {
 unsigned cp=0;for(char c:std::string_view(codepoint.data()))cp=cp*16+(c<='9'?c-'0':(c|32)-'a'+10);
 if(!codepoint[0]||cp<32||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)){std::snprintf(notice.data(),notice.size(),"Enter a Unicode code point from U+0020 to U+10FFFF (excluding surrogates).");return false;}
 unsigned bytes=cp<128?1:cp<2048?2:cp<65536?3:4;
 if(std::string_view(command.text.data()).size()+bytes>(command.operation==Operation::search?200:command.operation==Operation::extract?1024:16384)){std::snprintf(notice.data(),notice.size(),"Maximum input length reached.");return false;}
 unicodeInput=false;
 if(bytes==1)append(static_cast<char>(cp));else {
  append(static_cast<char>(bytes==2?0xc0|(cp>>6):bytes==3?0xe0|(cp>>12):0xf0|(cp>>18)));
  if(bytes==4)append(static_cast<char>(0x80|((cp>>12)&63)));
  if(bytes>=3)append(static_cast<char>(0x80|((cp>>6)&63)));
  append(static_cast<char>(0x80|(cp&63)));
 }
 codepoint.fill(0);notice.fill(0);return true;
}
bool Workflow::press(unsigned edge,const Catalog& c,bool busy) noexcept {
 if(panel==Panel::closed||!edge)return false;++revision;
 if(edge&Buttons::circle){close();return false;}
 if(busy){std::snprintf(notice.data(),notice.size(),"Wait for the current request to finish.");return false;}
 const Entry* e=target(c);
 if(panel==Panel::menu){
  if((edge&Buttons::up)&&selected)--selected;if((edge&Buttons::down)&&selected+1<optionCount)++selected;
  if(edge&Buttons::cross){auto op=options[selected];const char* reason=unavailable(op,e,c);if(*reason){std::snprintf(notice.data(),notice.size(),"%s",reason);return false;}
   if(op==Operation::add){add();return false;}command=Command{};command.operation=op;command.id=targetId;confirm=false;notice.fill(0);
   if(op==Operation::extract){panel=Panel::archives;archiveIndex=selected=0;}else panel=Panel::confirm;
  }
 }else if(panel==Panel::archives){
  const char* reason=unavailable(Operation::extract,e,c);if(*reason){std::snprintf(notice.data(),notice.size(),"%s",reason);return false;}
  if((edge&Buttons::up)&&archiveIndex)--archiveIndex;if((edge&Buttons::down)&&archiveIndex+1<e->archiveCount)++archiveIndex;
  if(edge&Buttons::cross){command.archive=c.archives[e->archiveStart+archiveIndex];command.text.fill(0);panel=Panel::keyboard;selected=48;keyPage=0;passwordVisible=false;}
 }else if(panel==Panel::keyboard){
  if(edge&Buttons::options){unicodeInput=!unicodeInput;codepoint.fill(0);notice.fill(0);return false;}
  if(edge&Buttons::l1)keyPage=(keyPage+2)%3;if(edge&Buttons::r1)keyPage=(keyPage+1)%3;
  if(edge&Buttons::square)erase();if(edge&Buttons::triangle)passwordVisible=!passwordVisible;
  if(edge&Buttons::up)selected=selected>=40?selected-9:selected>=10?selected-10:selected;
  if(edge&Buttons::down)selected=selected<30?selected+10:selected<40?40+2*((selected%10)/2):selected;
  if(edge&Buttons::left)selected=selected>=40?(selected>40?selected-2:selected):selected%10?selected-1:selected;
  if(edge&Buttons::right)selected=selected>=40?(selected<48?selected+2:selected):selected%10<9?selected+1:selected;
  if(edge&Buttons::cross){if(selected<40)append(keys(keyPage)[selected]);else if(selected<42)append(' ');else if(selected<44)erase();else if(selected<46){if(unicodeInput)codepoint.fill(0);else command.text.fill(0);}else if(selected<48){keyPage=(keyPage+1)%3;}else {
    if(unicodeInput){finishUnicode();return false;}
    if(command.operation==Operation::add&&(std::string_view(command.text.data())=="magnet:?xt=urn:btih:"||std::string_view(command.text.data()).size()<=8||!std::string_view(command.text.data()).starts_with("magnet:?"))){std::snprintf(notice.data(),notice.size(),"Enter a complete magnet link.");return false;}
    if(command.operation==Operation::search){if(!command.text[0]){std::snprintf(notice.data(),notice.size(),"Enter a game name.");return false;}const auto reason=unavailable(Operation::search,nullptr,c);if(*reason){std::snprintf(notice.data(),notice.size(),"%s",reason);return false;}panel=Panel::closed;return true;}
    panel=Panel::confirm;confirm=false;notice.fill(0);
   }}
 }else if(panel==Panel::confirm){
  if(edge&(Buttons::left|Buttons::right))confirm=!confirm;
  if(edge&Buttons::cross){if(!confirm){close();return false;}const char* reason=unavailable(command.operation,e,c);if(*reason){std::snprintf(notice.data(),notice.size(),"%s",reason);return false;}panel=Panel::closed;return true;}
 }
 return false;
}
}
