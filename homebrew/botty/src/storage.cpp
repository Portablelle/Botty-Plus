#include "storage.hpp"
#include <fstream>
#include <cstring>
namespace botty {
namespace {
using Inventory=std::map<fs::path,uint64_t>;
Inventory inventory(const fs::path& root) {
  Inventory result;
  struct stat base{};if(lstat(root.c_str(),&base))throw std::runtime_error("Transfer source is unavailable");
  auto add=[&](const fs::path& p){struct stat s{};if(lstat(p.c_str(),&s)||s.st_dev!=base.st_dev||(!S_ISREG(s.st_mode)&&!S_ISDIR(s.st_mode)))throw std::runtime_error("Transfer contains a mount, link or special file");result[p.lexically_relative(root)]=S_ISDIR(s.st_mode)?UINT64_MAX:uint64_t(s.st_size);};
  add(root);if(fs::is_directory(root))for(const auto& e:fs::recursive_directory_iterator(root)){if(result.size()>200000)throw std::runtime_error("Too many transfer files");add(e.path());}return result;
}
void copyFile(const fs::path& a,const fs::path& b,const std::function<void()>& check,const DeleteProgress& progress) {
  int in=open(a.c_str(),O_RDONLY|O_NOFOLLOW),out=-1;
  if(in<0)throw std::runtime_error("Cannot open transfer source");
  try {
    out=open(b.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(out<0)throw std::runtime_error("Transfer destination exists or is unavailable");
    uint64_t done=0;std::vector<char> buf(1024*1024);
    for(;;){if(check)check();auto n=read(in,buf.data(),buf.size());if(n<0&&errno==EINTR)continue;if(n<0)throw std::runtime_error("Transfer read failed");if(!n)break;ssize_t at=0;while(at<n){auto w=write(out,buf.data()+at,size_t(n-at));if(w<0&&errno==EINTR)continue;if(w<=0)throw std::runtime_error("Transfer write failed; source kept");at+=w;}done+=uint64_t(n);if(progress)progress(done,0,a.filename().string());}
    if(fsync(out))throw std::runtime_error("Transfer flush failed; source kept");close(out);out=-1;close(in);in=-1;
    // Successful writes, fsync and the enclosing inventory/size checks are kept.
    // No second full read of the source and destination after copying.
  }catch(...){if(in>=0)close(in);if(out>=0)close(out);throw;}
}
}
void copyChecked(const fs::path& source,const fs::path& destination,const std::function<void()>& check,const DeleteProgress& progress) {
  if(check)check();
  const auto files=inventory(source);uint64_t total=0;
  for(const auto& e:files)if(e.second!=UINT64_MAX){if(e.second>UINT64_MAX-total)throw std::runtime_error("Transfer is too large");total+=e.second;}
  if(total>UINT64_MAX-536870912ULL||freeBytes(destination.parent_path())<total+536870912ULL)throw std::runtime_error("Not enough destination space (512 MiB reserve required)");
  if(fs::exists(fs::symlink_status(destination)))throw std::runtime_error("Destination exists; nothing was overwritten");
  uint64_t done=0;const auto report=[&](uint64_t bytes,uint64_t,const std::string& file){if(progress)progress(done+bytes,total,file);};
  if(progress)progress(0,total,"");
  auto stage=destination.parent_path()/(".botty-transfer-"+randomId());
  if(files.at(".")==UINT64_MAX){fs::create_directory(stage);for(const auto& e:files){if(e.first==".")continue;if(check)check();if(e.second==UINT64_MAX)fs::create_directory(stage/e.first);else {copyFile(source/e.first,stage/e.first,check,report);done+=e.second;}}}
  else copyFile(source,stage,check,report);
  if(inventory(source)!=files||inventory(stage)!=files)throw std::runtime_error("Transfer source changed; both copies kept for inspection");
  if(check)check();if(fs::exists(fs::symlink_status(destination)))throw std::runtime_error("Transfer destination appeared; source kept");
  publishExclusive(stage,destination);
}
void publishExclusive(const fs::path& source,const fs::path& destination) {
  // Reserve the name atomically, including on exFAT (which has no hard links).
  // Rename replaces only our empty reservation, never a preexisting item.
  struct stat reserved{};
  if(fs::is_directory(source)){
    if(mkdir(destination.c_str(),0700))throw std::runtime_error("Destination exists or cannot be reserved");
    if(lstat(destination.c_str(),&reserved))throw std::runtime_error("Cannot inspect destination reservation");
  }else{
    const int fd=open(destination.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0)throw std::runtime_error("Destination exists or cannot be reserved");
    const int rc=fstat(fd,&reserved);close(fd);if(rc)throw std::runtime_error("Cannot inspect destination reservation");
  }
  struct stat current{};
  if(lstat(destination.c_str(),&current)||current.st_dev!=reserved.st_dev||current.st_ino!=reserved.st_ino)throw std::runtime_error("Destination changed; source kept");
  fs::rename(source,destination);
}
void removeTransferred(const fs::path& source) {
  if(fs::is_directory(source))deleteGameDirectory(source.parent_path(),source.filename());
  else downloadedFiles(source.parent_path(),{source.filename()},true);
}
}
