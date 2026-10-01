// Read headers only: RAR_SKIP never decompresses or writes archive members.
#define _UNIX
#include "dll.hpp"
#include "unicode.hpp"
#include "json.hpp"
#include <cstdio>
#include <fstream>
#include <cstdint>
using nlohmann::json;
extern "C" void Ps5UnrarProgress(int) {}
static int CALLBACK callback(UINT message, LPARAM, LPARAM, LPARAM) {
  if(message==UCM_NEEDPASSWORD || message==UCM_NEEDPASSWORDW)return -1;
  return 1;
}
int main(){
 FILE*out=fopen("/data/botty/manager/archive-inventory.jsonl","w");if(!out)return 1;
 HANDLE handle=nullptr;
 auto emit=[&](const json& j){fprintf(out,"%s\n",j.dump().c_str());fflush(out);};
 try {
  std::ifstream request("/data/botty/manager/archive-inspect-request.json");json config;request>>config;
  std::string archive=config.at("archive");
  if(archive.rfind("/data/botty/downloads/complete/",0)!=0 || archive.find("/../")!=std::string::npos)throw std::runtime_error("Invalid inspection path");
  RAROpenArchiveDataEx open{};open.ArcName=archive.data();open.OpenMode=RAR_OM_LIST;open.Callback=callback;
  handle=RAROpenArchiveEx(&open);if(!handle||open.OpenResult)throw std::runtime_error("Open failed: "+std::to_string(open.OpenResult));
  emit({{"type","archive"},{"path",archive},{"flags",open.Flags}});
  uint64_t total=0;size_t files=0,directories=0;
  for(size_t n=0;n<200000;++n){
   RARHeaderDataEx h{};int code=RARReadHeaderEx(handle,&h);
   if(code==ERAR_END_ARCHIVE){emit({{"type","summary"},{"complete",true},{"files",files},{"directories",directories},{"unpackedBytes",total},{"dataCRCVerified",false}});RARCloseArchive(handle);fclose(out);return 0;}
   if(code)throw std::runtime_error("Header failed: "+std::to_string(code));
   const auto name=botty::wideToUtf8(h.FileNameW,1024);
   const uint64_t size=(uint64_t(h.UnpSizeHigh)<<32)|h.UnpSize;
   const bool directory=(h.Flags&RHDF_DIRECTORY)!=0;
   emit({{"type","entry"},{"name",name},{"bytes",size},{"directory",directory},{"flags",h.Flags},{"method",h.Method},{"unpackVersion",h.UnpVer},{"redirectType",h.RedirType},{"crc32",h.FileCRC}});
   if(directory)++directories;else{++files;total+=size;}
   code=RARProcessFile(handle,RAR_SKIP,nullptr,nullptr);if(code)throw std::runtime_error("Skip failed: "+std::to_string(code));
  }
  throw std::runtime_error("Entry limit exceeded");
 }catch(const std::exception&e){emit({{"type","error"},{"error",e.what()}});if(handle)RARCloseArchive(handle);fclose(out);return 1;}
}
