#include "core.hpp"
#define _UNIX
#include "dll.hpp"
#include <algorithm>
#include "unicode.hpp"
#include <fcntl.h>
#include <limits>
#include <set>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <unistd.h>
namespace botty {
namespace {
struct Context {
  fs::path parent; std::string password, error; int fd=-1;
  std::function<bool()> cancelled;
  Progress progress; Reporter report; uint64_t written=0, expected=0;
};
std::string nameOf(const RARHeaderDataEx& header) {
  std::string name = header.FileNameW[0] ? wideToUtf8(header.FileNameW, sizeof(header.FileNameW)/sizeof(header.FileNameW[0])) : std::string(header.FileName);
  std::replace(name.begin(),name.end(),'\\','/');
  return name;
}
int CALLBACK callback(UINT message, LPARAM user, LPARAM first, LPARAM second) {
  auto& context=*reinterpret_cast<Context*>(user);
  try {
    if(context.cancelled && context.cancelled())throw std::runtime_error("Extraction cancelled");
    if (message == UCM_CHANGEVOLUME || message == UCM_CHANGEVOLUMEW) {
      const std::string name = message == UCM_CHANGEVOLUMEW ? wideToUtf8(reinterpret_cast<wchar_t*>(first), 1024) : std::string(reinterpret_cast<char*>(first));
      const auto path=containedExisting(context.parent, fs::path(name).is_absolute()?fs::path(name):context.parent/name);
      if (!fs::is_regular_file(path)) throw std::runtime_error("Missing archive volume");
      return 1;
    }
    if (message == UCM_NEEDPASSWORD || message == UCM_NEEDPASSWORDW) {
      if (context.password.empty()) throw std::runtime_error("This archive requires a password");
      if (message == UCM_NEEDPASSWORD) {
        if (context.password.size()+1>size_t(second)) return -1;
        std::copy(context.password.begin(),context.password.end(),reinterpret_cast<char*>(first));
        reinterpret_cast<char*>(first)[context.password.size()]=0;
      } else {
        auto wide=utf8ToWide<wchar_t>(context.password);
        if (wide.size()+1>size_t(second)) return -1;
        std::copy(wide.begin(),wide.end(),reinterpret_cast<wchar_t*>(first)); reinterpret_cast<wchar_t*>(first)[wide.size()]=0;
      }
      return 1;
    }
    if (message == UCM_PROCESSDATA && context.fd>=0) {
      if (second<0 || uint64_t(second)>context.expected-context.written) throw std::runtime_error("Unexpected decompressed size");
      auto data=reinterpret_cast<const char*>(first); size_t offset=0;
      while (offset<size_t(second)) {
        auto n=write(context.fd,data+offset,size_t(second)-offset);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) throw std::runtime_error("Extraction write failed; check disk space");
        offset+=size_t(n);
      }
      context.written+=uint64_t(second); context.progress.bytes+=uint64_t(second);
      context.report(context.progress);
    }
    return 1;
  } catch(const std::exception& error) { context.error=error.what(); return -1; }
}
struct Archive {
  HANDLE handle=nullptr; unsigned flags=0;
  Archive(const fs::path& path, unsigned mode, Context& context) {
    RAROpenArchiveDataEx open{}; std::string filename=fs::canonical(path).string();
    open.ArcName=filename.data(); open.OpenMode=mode; open.Callback=callback; open.UserData=reinterpret_cast<LPARAM>(&context);
    handle=RAROpenArchiveEx(&open);flags=open.Flags;
    if (!handle || open.OpenResult) { if(handle)RARCloseArchive(handle);handle=nullptr; throw std::runtime_error("Cannot open RAR archive (code "+std::to_string(open.OpenResult)+")"); }
    if (!context.password.empty()) RARSetPassword(handle,context.password.data());
  }
  ~Archive(){if(handle) RARCloseArchive(handle);}
};
void checkHeader(const RARHeaderDataEx& header) {
  safeRelative(nameOf(header));
  if (header.RedirType || (header.HostOS==3 && (header.FileAttr&0170000)==0120000)) throw std::runtime_error("Archive links are rejected");
}
void checkCode(int code, const Context& context) {
  if(context.cancelled && context.cancelled())throw std::runtime_error("Extraction cancelled");
  if(code!=ERAR_SUCCESS) throw std::runtime_error(context.error.empty()?"RAR extraction failed (code "+std::to_string(code)+"). Check all volumes, CRC and password.":context.error);
}
}
void extractRar(const fs::path& archive, const fs::path& destination, Reporter report, const std::string& password, std::function<bool()> cancelled, unsigned workers) {
  Context context;context.parent=fs::canonical(archive.parent_path()); context.password=password; context.report=report; context.cancelled=std::move(cancelled);
  context.progress.phase="Checking archive headers";context.report(context.progress);
  std::set<std::string> names; size_t entries=0;
  struct Member {std::string name;uint64_t size;bool directory;unsigned flags,version,method;};
  std::vector<Member> members;bool independent=password.empty();
  {
    Archive source(archive,RAR_OM_LIST,context);
    independent=independent && !(source.flags&(ROADF_SOLID|ROADF_ENCHEADERS));
    while(true) {
      checkCode(ERAR_SUCCESS,context);
      RARHeaderDataEx header{}; int code=RARReadHeaderEx(source.handle,&header);
      if(code==ERAR_END_ARCHIVE)break;
      checkCode(code,context); checkHeader(header);
      if(++entries>200000)throw std::runtime_error("Archive has too many entries");
      if(!names.insert(nameOf(header)).second)throw std::runtime_error("Duplicate archive paths are rejected");
      const uint64_t memberSize=(uint64_t(header.UnpSizeHigh)<<32)|header.UnpSize;
      members.push_back({nameOf(header),memberSize,bool(header.Flags&RHDF_DIRECTORY),header.Flags,header.UnpVer,header.Method});
      if(header.Flags&(RHDF_SOLID|RHDF_ENCRYPTED))independent=false;
      if(!(header.Flags&RHDF_DIRECTORY) && header.UnpVer>29)independent=false;
      if(!(header.Flags&RHDF_DIRECTORY)) {
        const uint64_t size=(uint64_t(header.UnpSizeHigh)<<32)|header.UnpSize;
        if(size>std::numeric_limits<uint64_t>::max()-context.progress.total)throw std::runtime_error("Invalid archive size");
        context.progress.total+=size;
      }
      checkCode(RARProcessFile(source.handle,RAR_SKIP,nullptr,nullptr),context);
    }
  }
  const uint64_t margin=512ULL*1024*1024;
  const auto available=freeBytes(destination.parent_path());
  if(available<margin || context.progress.total>available-margin)throw std::runtime_error("Not enough free space for extraction plus a 512 MiB reserve");
  if(!fs::create_directory(destination))throw std::runtime_error("Extraction destination already exists");
  auto sorted=members;std::stable_sort(sorted.begin(),sorted.end(),[](const Member&a,const Member&b){return a.size>b.size;});
  // User preference: three independent members at a time whenever safe.
  // A one/two-worker override is retained for controlled benchmarks/tests.
  const unsigned workerCount=independent?(workers==1?1:workers==2?2:3):1;
  std::map<std::string,unsigned> assignment;uint64_t loads[3]={0,0,0};
  for(const auto& member:sorted){unsigned owner=static_cast<unsigned>(std::min_element(loads,loads+workerCount)-loads);assignment[member.name]=owner;if(!member.directory)loads[owner]+=member.size;}
  context.progress.phase=workerCount>1?"Extracting and checking CRC ("+std::to_string(workerCount)+" workers)":"Extracting and checking CRC";context.report(context.progress);
  std::atomic<bool> stop{false};std::mutex progressLock;std::exception_ptr failure;
  const auto externalCancel=context.cancelled;
  auto run=[&](unsigned worker){
    Context local;local.parent=context.parent;local.password=password;local.progress.total=context.progress.total;local.progress.phase=context.progress.phase;
    local.cancelled=[&]{return stop.load()||(externalCancel&&externalCancel());};
    uint64_t previous=0;
    local.report=[&](const Progress& p){std::lock_guard<std::mutex> guard(progressLock);context.progress.bytes+=p.bytes-previous;previous=p.bytes;context.progress.file=p.file;context.report(context.progress);};
    try {
      auto& context=local;
  Archive source(archive,RAR_OM_EXTRACT,context);
  size_t memberIndex=0;
  while(true) {
    checkCode(ERAR_SUCCESS,context);
    RARHeaderDataEx header{};int code=RARReadHeaderEx(source.handle,&header);
    if(code==ERAR_END_ARCHIVE)break;
    checkCode(code,context);checkHeader(header);
    const auto name=nameOf(header);
    if(header.Flags&RHDF_SPLITBEFORE){
      // LIST hides continuation headers; EXTRACT exposes them when skipping a
      // member assigned to the other worker. Follow its remaining volumes.
      if(workerCount<2||!memberIndex||members[memberIndex-1].name!=name||assignment.at(name)==worker)
        throw std::runtime_error("Unexpected archive continuation");
      checkCode(RARProcessFile(source.handle,RAR_SKIP,nullptr,nullptr),context);continue;
    }
    if(memberIndex>=members.size())throw std::runtime_error("Archive changed after inspection");
    const auto& expected=members[memberIndex++];
    const uint64_t size=(uint64_t(header.UnpSizeHigh)<<32)|header.UnpSize;
    if(name!=expected.name||size!=expected.size||header.Flags!=expected.flags||header.UnpVer!=expected.version||header.Method!=expected.method)throw std::runtime_error("Archive changed after inspection: member "+std::to_string(memberIndex)+" flags "+std::to_string(header.Flags)+" expected "+std::to_string(expected.flags)+" size "+std::to_string(size)+" expected "+std::to_string(expected.size));
    if(workerCount>1 && assignment.at(name)!=worker){checkCode(RARProcessFile(source.handle,RAR_SKIP,nullptr,nullptr),context);continue;}
    auto relative=safeRelative(name);const auto target=destination/relative;
    context.progress.file=relative.string(); context.written=0;
    context.expected=(uint64_t(header.UnpSizeHigh)<<32)|header.UnpSize;
    if(header.Flags&RHDF_DIRECTORY) {
      fs::create_directories(target);checkCode(RARProcessFile(source.handle,RAR_SKIP,nullptr,nullptr),context);continue;
    }
    fs::create_directories(target.parent_path());
    context.fd=open(target.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(context.fd<0)throw std::runtime_error("Cannot create extracted file");
    try {
      // TEST streams validated output through our callback. UnRAR never creates paths,
      // symlinks or special files itself; all filesystem writes remain in this stage.
      code=RARProcessFile(source.handle,RAR_TEST,nullptr,nullptr);
      checkCode(code,context);
      if(context.written!=context.expected)throw std::runtime_error("Extracted size mismatch");
      if(fsync(context.fd))throw std::runtime_error("Could not flush extracted file");
      close(context.fd);context.fd=-1;
    } catch(...) {close(context.fd);context.fd=-1;throw;}
  }
  if(memberIndex!=members.size())throw std::runtime_error("Archive changed after inspection");

    }catch(...){std::lock_guard<std::mutex> guard(progressLock);if(!failure)failure=std::current_exception();stop=true;}
  };
  std::vector<std::thread> pool;
  try{for(unsigned i=1;i<workerCount;++i)pool.emplace_back([&,i]{run(i);});}
  catch(...){stop=true;for(auto& thread:pool)thread.join();throw;}
  run(0);for(auto& thread:pool)thread.join();
  if(failure)std::rethrow_exception(failure);
  if(context.progress.bytes!=context.progress.total)throw std::runtime_error("Total extracted size mismatch");
  checkCode(ERAR_SUCCESS,context);
  context.progress.phase="Extraction verified";context.report(context.progress);
}
}
extern "C" void Ps5UnrarProgress(int) {}
