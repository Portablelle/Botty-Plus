// SPDX-License-Identifier: GPL-3.0-or-later
#include "actions.hpp"
#include <cstdio>
#include <cmath>
#include <algorithm>
namespace botty {
void formatDeletionEstimate(double bytes,char* out,unsigned size) noexcept {
 // Coarse extrapolation from the ~250 GB Ace Combat deletion on the test PS5.
 // This is a planning range, not measured live progress or a completion deadline.
 if(!std::isfinite(bytes)||bytes<=0){std::snprintf(out,size,"Large games can take several minutes.");return;}
 const double factor=bytes/250122412221.0;
 const double low=std::fmax(1,std::ceil(factor*5)),high=std::fmax(1,std::ceil(factor*10));
 if(low==high)std::snprintf(out,size,"Estimated total: about %.0f min. Actual time varies.",low);
 else std::snprintf(out,size,"Estimated total: about %.0f-%.0f min. Actual time varies.",low,high);
}
const char* operationLabel(Operation op) noexcept {
 if(op==Operation::nativeUpdate)return "Update Botty+";
 if(op==Operation::checkNativeUpdate)return "Check for updates";
 switch(op){case Operation::transfer:return "Move to another disk";case Operation::restoreOriginal:return "Restore uncompressed game";case Operation::removeOriginal:return "Delete uncompressed copy";case Operation::compress:return "Compress game";case Operation::cancelCompression:return "Cancel compression";case Operation::removeLibrary:return "Delete game";case Operation::removeTorrent:return "Delete torrent & files";case Operation::explore:return "Explore games";case Operation::search:return "Search games";case Operation::exploreGrab:case Operation::grab:return "Add game";case Operation::pause:return "Pause";case Operation::resume:return "Resume";case Operation::verify:return "Verify files";case Operation::add:return "Add magnet";case Operation::extract:return "Extract";case Operation::move:return "Move to Library";case Operation::remove:return "Delete extraction";case Operation::cancel:return "Cancel extraction";case Operation::dismiss:return "Remove from Activity";default:return "Actions";}
}
const char* actionPath(Operation op) noexcept {
 if(op==Operation::nativeUpdate)return "/api/native-update";
 if(op==Operation::checkNativeUpdate)return "/api/native-update/check";
 switch(op){case Operation::transfer:return "/api/transfer";case Operation::restoreOriginal:return "/api/restore-uncompressed";case Operation::removeOriginal:return "/api/delete-uncompressed";case Operation::compress:return "/api/compress-game";case Operation::cancelCompression:return "/api/cancel-compression";case Operation::removeLibrary:return "/api/delete-library-game";case Operation::explore:return "/api/explore";case Operation::exploreGrab:return "/api/explore/add";case Operation::search:return "/api/search";case Operation::grab:return "/api/search/add";case Operation::extract:return "/api/extract";case Operation::move:return "/api/move";case Operation::remove:return "/api/delete-extraction";case Operation::cancel:return "/api/cancel-extraction";case Operation::dismiss:return "/api/dismiss-extraction";default:return "/api/torrent";}
}
bool encodeCommand(const Command& cmd,char* out,std::size_t capacity,std::size_t& length) noexcept {
 length=0;bool ok=true;
 const auto append=[&](std::string_view s){for(char c:s){if(length+1>=capacity){ok=false;return;}out[length++]=c;}};
 const auto quote=[&](std::string_view s){append("\"");for(unsigned char c:s){if(c=='"')append("\\\"");else if(c=='\\')append("\\\\");else if(c<32){char b[7];std::snprintf(b,sizeof(b),"\\u%04x",c);append(b);}else {char b=static_cast<char>(c);append({&b,1});}}append("\"");};
 if(cmd.operation==Operation::none)return false;
 append("{");
 if(cmd.operation==Operation::nativeUpdate||cmd.operation==Operation::checkNativeUpdate){}
 else if(cmd.operation==Operation::explore){const auto sort=std::string_view(cmd.text.data());if(sort!="seeders"&&sort!="completed"&&sort!="newest")return false;append("\"sort\":");quote(sort);if(cmd.refresh)append(",\"refresh\":true");}
 else if(cmd.operation==Operation::search){if(!cmd.text[0]||std::string_view(cmd.text.data()).size()>200)return false;append("\"query\":");quote(cmd.text.data());}
 else if(cmd.operation==Operation::grab||cmd.operation==Operation::exploreGrab){if(std::string_view(cmd.id.data()).size()!=32)return false;append("\"id\":");quote(cmd.id.data());}
 else if(cmd.operation==Operation::add){if(!std::string_view(cmd.text.data()).starts_with("magnet:?"))return false;append("\"action\":\"add\",\"magnet\":");quote(cmd.text.data());}
 else {
  append("\"id\":");
  if((cmd.operation==Operation::transfer&&!cmd.torrent)||cmd.operation==Operation::restoreOriginal||cmd.operation==Operation::removeOriginal||cmd.operation==Operation::compress||cmd.operation==Operation::cancelCompression||cmd.operation==Operation::removeLibrary||cmd.operation==Operation::move||cmd.operation==Operation::remove||cmd.operation==Operation::cancel||cmd.operation==Operation::dismiss){if(!cmd.id[0])return false;quote(cmd.id.data());}
  else {std::string_view id=cmd.id.data();if(id.empty()||id.size()>10||(id.size()>1&&id.front()=='0'))return false;unsigned long long value=0;for(char c:id){if(c<'0'||c>'9')return false;value=value*10+c-'0';}if(value>2147483647)return false;append(id);}
  if(cmd.operation==Operation::restoreOriginal||cmd.operation==Operation::removeOriginal||cmd.operation==Operation::compress||cmd.operation==Operation::removeLibrary)append(",\"confirmed\":true");
  else if(cmd.operation==Operation::extract){if(!cmd.archive[0]||std::string_view(cmd.text.data()).size()>1024)return false;append(",\"archive\":");quote(cmd.archive.data());append(",\"password\":");quote(cmd.text.data());}
  else if(cmd.operation!=Operation::transfer&&cmd.operation!=Operation::cancelCompression&&cmd.operation!=Operation::move&&cmd.operation!=Operation::remove&&cmd.operation!=Operation::cancel&&cmd.operation!=Operation::dismiss){append(",\"action\":");if(cmd.operation==Operation::removeTorrent){quote("remove-data");append(",\"confirmed\":true");}else quote(cmd.operation==Operation::pause?"pause":cmd.operation==Operation::resume?"resume":"verify");}
 }
 if(cmd.storage[0]&&cmd.operation!=Operation::nativeUpdate&&cmd.operation!=Operation::checkNativeUpdate){append(",\"storage\":");quote(cmd.storage.data());}
 if(cmd.operation==Operation::transfer){append(",\"kind\":");quote(cmd.torrent?"torrent":"job");}
 if(cmd.storage[0]&&(cmd.operation==Operation::add||cmd.operation==Operation::grab||cmd.operation==Operation::exploreGrab))append(cmd.automatic?",\"automatic\":true":",\"automatic\":false");
 append("}");if(length<capacity)out[length]=0;return ok;
}
const char* unavailable(Operation op,const Entry* e,const Catalog& c) noexcept {
 if(c.stale)return "Reconnecting to Botty. Wait for an updated status before making changes.";
 if(op==Operation::checkNativeUpdate)return c.nativeUpdate.supported&&std::string_view(c.nativeUpdate.scope.data())=="installation"?"":"Update the Botty service from Portal+ to enable installation updates.";
 if(op==Operation::nativeUpdate)return nativeUpdateAvailable(c.nativeUpdate,c.stale)?"":"Check for an available update before installing.";
 if(!c.valid)return "Reconnect to Botty before performing an action.";
 if(op==Operation::explore||op==Operation::exploreGrab){if(!c.exploreSupported)return "Update the Botty service to enable Explore.";if(c.exploreBusy)return "Explore is refreshing. Browse sources now; download when it finishes.";if(c.exploreAdding)return "Wait for the current download request.";return op==Operation::explore||c.transmissionReady?"":"Wait for rTorrent to reconnect.";}
 if(op==Operation::search||op==Operation::grab){if(!c.searchSupported)return "Update the Botty service to enable search.";if(c.searchBusy||c.searchAdding)return "Wait for the current search or download request.";if(op==Operation::search)return "";return c.transmissionReady?"":"Wait for rTorrent to reconnect.";}
 if(op==Operation::add)return c.transmissionReady?"":"Wait for rTorrent to reconnect.";
 if(e&&e->task)return "This task is followed in Activity. Use Library for game actions.";
 if(!e)return "This item is no longer available. Close this menu and refresh.";
 if(op==Operation::transfer){if(!c.storageSupported)return "Update Botty to enable external storage.";if(c.extracting||c.compressionBusy)return "Wait for the current file operation.";if(!e)return "Item unavailable.";return e->status[0]&&std::string_view(e->status.data())!="extracting"?"":"Wait for extraction to finish.";}
 if(op==Operation::compress){
  if(!c.compressionSupported)return "Update Botty to enable Library compression.";
  if(c.compressionBusy||c.extracting)return "Wait for the current file operation to finish.";
  if(std::string_view(e->status.data())!="moved"||!std::string_view(e->titleId.data()).starts_with("PPSA")||std::string_view(e->titleId.data())=="PPSA99071"||std::string_view(e->kind.data())!="folder")return "Only PS5 game folders in Library can be compressed.";
  if(e->compressed)return "This game already has a compressed copy.";
  return "";
 }
 if(op==Operation::restoreOriginal)return e->originalKept&&(std::string_view(e->compressionState.data())=="ready"||std::string_view(e->compressionState.data())=="uncertain")?"":"No original backup available for recovery.";
 if(op==Operation::removeOriginal){const auto state=std::string_view(e->compressionState.data());const bool recovery=state=="uncertain"&&std::string_view(c.compressionStatus.data())=="uncertain"&&std::string_view(c.compressionJob.data())==e->id.data();return (c.compressionBusy&&!recovery)||c.extracting?"Wait for the current file operation.":e->originalKept&&(state=="ready"||state=="uncertain")?"":"No original backup available for deletion.";}
 if(op==Operation::cancelCompression)return (std::string_view(c.compressionStatus.data())=="running"||std::string_view(c.compressionStatus.data())=="starting")&&c.compressionBusy&&std::string_view(c.compressionJob.data())==e->id.data()?"":"No active compression for this game.";
 if(c.compressionBusy&&(op==Operation::extract||op==Operation::move||op==Operation::remove||op==Operation::removeLibrary||op==Operation::removeTorrent))return "Wait for compression to finish.";
 if(op==Operation::removeTorrent){if(!c.torrentRemovalSupported)return "Update Botty to enable torrent deletion.";if(!c.transmissionReady)return "Wait for rTorrent to reconnect.";return c.extracting?"Wait for extraction to finish before deleting archives.":"";}
 if(op==Operation::pause||op==Operation::resume||op==Operation::verify||op==Operation::extract){
  if(!c.transmissionReady)return "Wait for rTorrent to reconnect.";
  if(op==Operation::extract){if(c.extracting)return "Wait for the active extraction to finish.";if(!e->extractable)return "Wait until this torrent is complete, verified and error-free.";if(!e->archiveCount)return e->archivesOmitted?"Archive names exceed the display limit.":"No first RAR volume found in this torrent.";}
  return "";
 }
 if((op==Operation::cancel||op==Operation::dismiss)&&!c.extractionControls)return "Start the updated Botty service next session to use this action.";
 const auto status=std::string_view(e->status.data());
 if(op==Operation::cancel)return status=="extracting"?"":"This extraction is no longer running.";
 if(op==Operation::removeLibrary){if(e->compressed){if(c.extracting)return "Wait for extraction to finish.";if(!c.compressedDeletionSupported)return "Update Botty to enable compressed game deletion.";return std::string_view(e->compressionState.data())=="ready"?"":"Wait for compression or recovery to finish.";}if(!c.libraryDeletionSupported)return "Update Botty to enable game deletion.";if(c.extracting)return "Wait for extraction to finish.";if(status!="moved")return "Only games moved to Library can be deleted.";return std::string_view(e->kind.data())=="folder"?"":"Image files require unmounting and manual removal.";}
 if(op==Operation::dismiss)return status=="ready"||status=="moved"||status=="failed"||status=="cancelled"||status=="interrupted"?"":"Only finished extractions can be removed from the list.";
 if(c.extracting)return "Wait for the active extraction to finish.";
 if(op==Operation::move){if(status!="ready")return "Only ready extractions can be moved.";if(!e->kind[0]||std::string_view(e->kind.data())=="unsupported")return "This extraction does not contain a supported game format.";}
 if(op==Operation::remove&&status!="ready"&&status!="failed"&&status!="interrupted"&&status!="cancelled")return "Moved or active extractions cannot be deleted here.";
 return "";
}
namespace {
int storageIndex(const Catalog& c,std::string_view id) noexcept {for(unsigned i=0;i<c.storageCount;++i)if(id==c.storage[i].id.data())return static_cast<int>(i);return -1;}
bool storageReady(const Catalog& c,std::string_view id) noexcept {const int i=storageIndex(c,id);return !id.empty()&&i>=0&&c.storage[static_cast<unsigned>(i)].available;}
// Keep the preferred disk when connected, else the first connected disk other than avoid.
// Position of an exact archive path in an entry's current list, or -1.
int archivePosition(const Catalog& c,const Entry& e,const std::array<char,4096>& path) noexcept {
 for(unsigned i=0;i<e.archiveCount&&e.archiveStart+i<c.archiveCount;++i)if(c.archives[e.archiveStart+i]==path)return static_cast<int>(i);
 return -1;
}
void pickStorage(const Catalog& c,std::array<char,64>& out,std::string_view preferred,std::string_view avoid) noexcept {
 out.fill(0);
 if(preferred!=avoid&&storageReady(c,preferred)){const int i=storageIndex(c,preferred);out=c.storage[static_cast<unsigned>(i)].id;return;}
 for(unsigned i=0;i<c.storageCount;++i)if(c.storage[i].available&&avoid!=c.storage[i].id.data()){out=c.storage[i].id;return;}
}
}
bool Workflow::immediate(Operation op) noexcept {return op==Operation::pause||op==Operation::resume||op==Operation::verify;}
bool Workflow::destructive(Operation op) noexcept {
 return op==Operation::removeTorrent||op==Operation::removeLibrary||op==Operation::removeOriginal||op==Operation::restoreOriginal||
  op==Operation::remove||op==Operation::dismiss||op==Operation::cancel||op==Operation::cancelCompression;
}
void Workflow::close() noexcept {panel=Panel::closed;rowCount=focus=0;confirming=confirm=false;unicodeInput=false;codepoint.fill(0);command.text.fill(0);savedPassword.fill(0);notice.fill(0);++revision;}
const Entry* Workflow::target(const Catalog& c) const noexcept {const auto& list=targetTab==0?c.torrents:c.jobs;const auto count=targetTab==0?c.torrentCount:c.jobCount;for(unsigned i=0;i<count;++i)if(std::string_view(list[i].id.data())==targetId.data())return &list[i];return nullptr;}
bool Workflow::hasRow(Row row) const noexcept {for(unsigned i=0;i<rowCount;++i)if(rows[i]==row)return true;return false;}
unsigned quickActions(const Entry* e,unsigned tab,const Catalog& c,std::array<Operation,8>& options) noexcept {
 unsigned optionCount=0;if(!e||e->task||tab>2)return 0;
 if(tab==0){options[optionCount++]=e->active?Operation::pause:Operation::resume;options[optionCount++]=Operation::verify;options[optionCount++]=Operation::extract;}
 else if(tab==2){
  if(std::string_view(e->status.data())=="ready")options[optionCount++]=Operation::move;
  else if(c.compressionSupported){
   const bool current=c.compressionBusy&&std::string_view(c.compressionJob.data())==e->id.data();
   if(current&&(std::string_view(c.compressionStatus.data())=="running"||std::string_view(c.compressionStatus.data())=="starting"))options[optionCount++]=Operation::cancelCompression;
   else if(e->compressed&&e->originalKept&&(std::string_view(e->compressionState.data())=="ready"||std::string_view(e->compressionState.data())=="uncertain"))options[optionCount++]=Operation::removeOriginal;
   else if(!e->compressed&&std::string_view(e->kind.data())=="folder"&&std::string_view(e->titleId.data()).starts_with("PPSA")&&std::string_view(e->titleId.data())!="PPSA99071")options[optionCount++]=Operation::compress;
  }
  if(std::string_view(e->status.data())!="ready"&&e->originalKept&&(std::string_view(e->compressionState.data())=="ready"||std::string_view(e->compressionState.data())=="uncertain"))options[optionCount++]=Operation::restoreOriginal;
 }else {if(e->active)options[optionCount++]=Operation::cancel;options[optionCount++]=Operation::move;}
 if(c.storageSupported&&(tab==0||std::string_view(e->status.data())=="ready"||std::string_view(e->status.data())=="moved"))options[optionCount++]=Operation::transfer;
 // Destructive choices stay last, each behind an inline confirmation.
 if(tab==0)options[optionCount++]=Operation::removeTorrent;
 else if(tab==2){if(std::string_view(e->status.data())=="ready")options[optionCount++]=Operation::remove;else options[optionCount++]=Operation::removeLibrary;}
 else {options[optionCount++]=Operation::remove;options[optionCount++]=Operation::dismiss;}
 return optionCount;
}
void Workflow::open(const Entry* e,unsigned tab,const Catalog& c) noexcept {
 close();optionCount=quickActions(e,tab,c,options);if(!optionCount)return;
 panel=Panel::menu;selected=0;targetTab=tab;targetId=e->id;targetName=e->name;
}
bool Workflow::choose(Operation op,const Catalog& c,bool busy) noexcept {
 const Entry* e=target(c);++revision;
 unsigned index=0;while(index<optionCount&&options[index]!=op)++index;
 if(index==optionCount)return false;
 selected=index;confirming=confirm=false;
 const char* reason=unavailable(op,e,c);if(*reason){std::snprintf(notice.data(),notice.size(),"%s",reason);return false;}
 command=Command{};command.operation=op;command.id=targetId;command.torrent=targetTab==0;notice.fill(0);
 if(immediate(op)){
  if(busy){std::snprintf(notice.data(),notice.size(),"Wait for the current request to finish.");return false;}
  panel=Panel::closed;return true;
 }
 if(destructive(op)){panel=Panel::menu;confirming=true;confirm=false;return false;}
 openSheet(c);return false;
}
void Workflow::openSheet(const Catalog& c) noexcept {
 const auto op=command.operation;const Entry* e=target(c);rowCount=0;passwordVisible=false;
 // The archive is kept by path: refreshes may reorder the list while a password is typed.
 if(op==Operation::extract){archiveIndex=0;command.archive.fill(0);if(e&&e->archiveCount)command.archive=c.archives[e->archiveStart];}
 if(op==Operation::exploreGrab||op==Operation::grab)rows[rowCount++]=Row::source;
 if(op==Operation::add)rows[rowCount++]=Row::magnet;
 if(op==Operation::extract){rows[rowCount++]=Row::archive;rows[rowCount++]=Row::password;}
 if(c.storageSupported||op==Operation::transfer)rows[rowCount++]=Row::storage;
 const bool download=op==Operation::add||op==Operation::grab||op==Operation::exploreGrab;
 if(download&&c.storageSupported){rows[rowCount++]=Row::mode;command.automatic=lastAutomatic;}
 if(hasRow(Row::storage)){
  if(download)pickStorage(c,command.storage,lastStorage.data(),{});
  else if(op==Operation::transfer)pickStorage(c,command.storage,{},e?std::string_view(e->storage.data()):std::string_view{});
  else pickStorage(c,command.storage,e?std::string_view(e->storage.data()):std::string_view{},{});
 }
 panel=Panel::sheet;focus=rowCount;confirming=confirm=false;notice.fill(0);
}
void Workflow::change(Row row,bool forward,const Catalog& c) noexcept {
 const Entry* e=target(c);
 if(row==Row::source&&sourceCount){
  if(forward&&sourceIndex+1<sourceCount)++sourceIndex;else if(!forward&&sourceIndex)--sourceIndex;
  command.id=sources[sourceIndex].id;targetName=sources[sourceIndex].name;
 }else if(row==Row::mode)command.automatic=!forward;
 else if(row==Row::archive&&e&&e->archiveCount){
  const int at=archivePosition(c,*e,command.archive);if(at>=0)archiveIndex=static_cast<unsigned>(at);else archiveIndex=0;
  if(at>=0&&forward&&archiveIndex+1<e->archiveCount)++archiveIndex;else if(at>=0&&!forward&&archiveIndex)--archiveIndex;
  command.archive=c.archives[e->archiveStart+archiveIndex];
 }
 else if(row==Row::storage){
  const std::string_view avoid=command.operation==Operation::transfer&&e?std::string_view(e->storage.data()):std::string_view{};
  int current=storageIndex(c,command.storage.data()),next=-1;
  for(int i=forward?current+1:current-1;i>=0&&i<static_cast<int>(c.storageCount);i+=forward?1:-1)
   if(c.storage[static_cast<unsigned>(i)].available&&avoid!=c.storage[static_cast<unsigned>(i)].id.data()){next=i;break;}
  if(next>=0)command.storage=c.storage[static_cast<unsigned>(next)].id;
 }
 notice.fill(0);
}
void Workflow::editPassword() noexcept {
 std::copy_n(command.text.begin(),savedPassword.size()-1,savedPassword.begin());savedPassword.back()=0;
 panel=Panel::keyboard;selected=48;keyPage=0;passwordVisible=false;unicodeInput=false;codepoint.fill(0);notice.fill(0);
}
bool Workflow::submit(const Catalog& c,bool busy) noexcept {
 if(busy){std::snprintf(notice.data(),notice.size(),"Wait for the current request to finish.");return false;}
 const auto op=command.operation;const Entry* e=target(c);
 const bool download=op==Operation::add||op==Operation::grab||op==Operation::exploreGrab;
 const char* reason=unavailable(op,download?nullptr:e,c);
 if(*reason){std::snprintf(notice.data(),notice.size(),"%s",reason);return false;}
 if(op==Operation::exploreGrab||op==Operation::grab){if(sourceIndex>=sourceCount)return false;command.id=sources[sourceIndex].id;}
 if(op==Operation::extract){
  const int at=e&&command.archive[0]?archivePosition(c,*e,command.archive):-1;
  if(at<0){
   std::snprintf(notice.data(),notice.size(),"The archive list changed. Check the archive, then extract again.");
   archiveIndex=0;command.archive.fill(0);if(e&&e->archiveCount)command.archive=c.archives[e->archiveStart];
   return false;
  }
  archiveIndex=static_cast<unsigned>(at);
 }
 if(hasRow(Row::storage)){
  if(!storageReady(c,command.storage.data())){std::snprintf(notice.data(),notice.size(),"Connect the selected disk, or choose another one.");return false;}
  if(op==Operation::transfer&&e&&command.storage==e->storage){std::snprintf(notice.data(),notice.size(),"Choose a different disk.");return false;}
 }
 if(download){lastStorage=command.storage;lastAutomatic=command.automatic;}
 savedPassword.fill(0);panel=Panel::closed;return true;
}
void Workflow::search(std::string_view previous) noexcept {
 close();command=Command{};command.operation=Operation::search;
 for(unsigned i=0;i<previous.size()&&i<textLimit(Operation::search);++i)command.text[i]=previous[i];
 panel=Panel::keyboard;selected=0;keyPage=0;
}
void Workflow::grab(const Entry& e,const Catalog& c) noexcept {
 close();command=Command{};command.operation=Operation::grab;command.id=e.id;targetName=e.name;targetId.fill(0);gameName=e.name;gameId=e.id;
 sourceCount=1;sourceIndex=0;auto& source=sources[0];source=DownloadSource{};source.id=e.id;source.name=e.name;
 std::snprintf(source.tracker.data(),source.tracker.size(),"Prowlarr");source.size=e.total;source.seeders=e.peers;source.leechers=e.downloadingPeers;
 openSheet(c);
}
void Workflow::chooseSources(const Entry& e,const Catalog& c,bool compare) noexcept {
 close();command=Command{};command.operation=Operation::exploreGrab;targetName=e.name;targetId.fill(0);sourceCount=0;gameName=e.name;gameId=e.id;
 for(unsigned i=0;i<e.sourceCount&&i<sources.size()&&e.sourceStart+i<c.sourceCount;++i)sources[sourceCount++]=c.sources[e.sourceStart+i];
 // Older services provide a single source.
 if(!sourceCount){auto& source=sources[sourceCount++];source=DownloadSource{};source.id=e.id;source.name=e.name;std::snprintf(source.tracker.data(),source.tracker.size(),"Prowlarr");source.size=e.total;source.seeders=e.peers;source.leechers=e.downloadingPeers;source.grabs=e.completedCount;source.published=e.published;}
 // The best-seeded source is preselected; every other source stays one press away.
 sourceIndex=0;for(unsigned i=1;i<sourceCount;++i)if(sources[i].seeders>sources[sourceIndex].seeders)sourceIndex=i;
 command.id=sources[sourceIndex].id;
 openSheet(c);if(compare)focus=0;
}
void Workflow::add() noexcept {close();command=Command{};command.operation=Operation::add;sourceCount=sourceIndex=0;targetId.fill(0);targetName.fill(0);gameName.fill(0);gameId.fill(0);std::snprintf(command.text.data(),command.text.size(),"magnet:?xt=urn:btih:");panel=Panel::keyboard;selected=0;keyPage=0;passwordVisible=false;}
std::string_view Workflow::keys(unsigned page) noexcept {
 switch(page%3){case 0:return "1234567890qwertyuiopasdfghjkl;zxcvbnm,./";case 1:return "1234567890QWERTYUIOPASDFGHJKL:ZXCVBNM<>?";default:return "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~01234567";}
}
unsigned Workflow::textLimit(Operation op) noexcept {return op==Operation::search?200:op==Operation::extract?1024:16384;}
void Workflow::append(char c) noexcept {
 if(unicodeInput){unsigned length=static_cast<unsigned>(std::string_view(codepoint.data()).size());if(length<6&&((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))){codepoint[length]=c;notice.fill(0);}else std::snprintf(notice.data(),notice.size(),"Enter up to six hexadecimal digits (0-9, A-F).");++revision;return;}
 auto length=std::string_view(command.text.data()).size();const unsigned limit=textLimit(command.operation);if(length<limit){command.text[length]=c;command.text[length+1]=0;notice.fill(0);}else std::snprintf(notice.data(),notice.size(),"Maximum length reached.");++revision;}
void Workflow::erase() noexcept {if(unicodeInput){auto n=std::string_view(codepoint.data()).size();if(n)codepoint[n-1]=0;++revision;return;}auto length=std::string_view(command.text.data()).size();if(length){unsigned start=static_cast<unsigned>(length-1);while(start&&(static_cast<unsigned char>(command.text[start])&0xc0)==0x80)--start;for(unsigned i=start;i<length;++i)command.text[i]=0;}notice.fill(0);++revision;}
bool Workflow::finishUnicode() noexcept {
 unsigned cp=0;for(char c:std::string_view(codepoint.data()))cp=cp*16+(c<='9'?c-'0':(c|32)-'a'+10);
 if(!codepoint[0]||cp<32||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)){std::snprintf(notice.data(),notice.size(),"Enter a Unicode code point from U+0020 to U+10FFFF (excluding surrogates).");return false;}
 unsigned bytes=cp<128?1:cp<2048?2:cp<65536?3:4;
 if(std::string_view(command.text.data()).size()+bytes>(textLimit(command.operation))){std::snprintf(notice.data(),notice.size(),"Maximum input length reached.");return false;}
 unicodeInput=false;
 if(bytes==1)append(static_cast<char>(cp));else {
  append(static_cast<char>(bytes==2?0xc0|(cp>>6):bytes==3?0xe0|(cp>>12):0xf0|(cp>>18)));
  if(bytes==4)append(static_cast<char>(0x80|((cp>>12)&63)));
  if(bytes>=3)append(static_cast<char>(0x80|((cp>>6)&63)));
  append(static_cast<char>(0x80|(cp&63)));
 }
 codepoint.fill(0);notice.fill(0);return true;
}
bool Workflow::acceptText(std::string_view text,const Catalog& c,bool busy) noexcept {
 if(panel!=Panel::keyboard)return false;
 if(busy){std::snprintf(notice.data(),notice.size(),"Wait for the current request to finish.");++revision;return false;}
 const unsigned limit=textLimit(command.operation);
 if(text.size()>limit||text.find('\0')!=std::string_view::npos){std::snprintf(notice.data(),notice.size(),"Input exceeds the allowed length or contains a null character.");++revision;return false;}
 command.text.fill(0);for(unsigned i=0;i<text.size();++i)command.text[i]=text[i];
 unicodeInput=false;codepoint.fill(0);++revision;return finishInput(c,busy);
}
bool Workflow::finishInput(const Catalog& c,bool busy) noexcept {
 if(busy){std::snprintf(notice.data(),notice.size(),"Wait for the current request to finish.");return false;}
 if(command.operation==Operation::add&&(std::string_view(command.text.data())=="magnet:?xt=urn:btih:"||std::string_view(command.text.data()).size()<=8||!std::string_view(command.text.data()).starts_with("magnet:?"))){std::snprintf(notice.data(),notice.size(),"Enter a complete magnet link.");return false;}
 if(command.operation==Operation::search){if(!command.text[0]){std::snprintf(notice.data(),notice.size(),"Enter a game name.");return false;}const auto reason=unavailable(Operation::search,nullptr,c);if(*reason){std::snprintf(notice.data(),notice.size(),"%s",reason);return false;}panel=Panel::closed;return true;}
 // Extraction returns to its sheet; a magnet opens the Get game sheet.
 if(command.operation==Operation::extract){savedPassword.fill(0);panel=Panel::sheet;focus=rowCount;notice.fill(0);return false;}
 if(hasRow(Row::magnet)&&panel==Panel::keyboard){panel=Panel::sheet;focus=rowCount;notice.fill(0);return false;}
 openSheet(c);return false;
}
bool Workflow::press(unsigned edge,const Catalog& c,bool busy) noexcept {
 if(panel==Panel::closed||!edge)return false;++revision;
 if(edge&Buttons::circle){
  // Circle backs out one level: a confirmation, then a password edit, then the panel.
  if(panel==Panel::menu&&confirming){confirming=confirm=false;notice.fill(0);return false;}
  if(panel==Panel::keyboard&&command.operation==Operation::extract&&rowCount){command.text.fill(0);std::copy(savedPassword.begin(),savedPassword.end(),command.text.begin());savedPassword.fill(0);unicodeInput=false;codepoint.fill(0);panel=Panel::sheet;focus=rowCount;notice.fill(0);return false;}
  close();return false;
 }
 if(panel==Panel::menu){
  const Entry* e=target(c);
  if(confirming){
   if(edge&(Buttons::left|Buttons::right))confirm=!confirm;
   if(edge&Buttons::cross){
    if(!confirm){confirming=false;notice.fill(0);return false;}
    if(busy){std::snprintf(notice.data(),notice.size(),"Wait for the current request to finish.");return false;}
    const char* reason=unavailable(command.operation,e,c);if(*reason){std::snprintf(notice.data(),notice.size(),"%s",reason);return false;}
    panel=Panel::closed;confirming=false;return true;
   }
   return false;
  }
  if((edge&Buttons::up)&&selected){--selected;notice.fill(0);}
  if((edge&Buttons::down)&&selected+1<optionCount){++selected;notice.fill(0);}
  if((edge&Buttons::cross)&&selected<optionCount)return choose(options[selected],c,busy);
 }else if(panel==Panel::sheet){
  if((edge&Buttons::up)&&focus)--focus;
  if((edge&Buttons::down)&&focus<rowCount)++focus;
  if((edge&(Buttons::left|Buttons::right))&&focus<rowCount)change(rows[focus],edge&Buttons::right,c);
  if(hasRow(Row::password)){
   if(edge&Buttons::triangle)passwordVisible=!passwordVisible;
   if(edge&Buttons::square){editPassword();return false;}
  }
  if(edge&Buttons::cross){
   if(focus>=rowCount)return submit(c,busy);
   if(rows[focus]==Row::password){editPassword();return false;}
   if(rows[focus]==Row::magnet){panel=Panel::keyboard;selected=0;keyPage=0;notice.fill(0);return false;}
   focus=rowCount; // Cross on a value row returns to the primary button.
  }
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
    return finishInput(c,busy);
   }}
 }
 return false;
}
}
