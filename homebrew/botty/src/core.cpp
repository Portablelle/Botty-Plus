#include "core.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__PS5__) || defined(__APPLE__)
#include <sys/mount.h>
#endif
namespace botty {
std::string readText(const fs::path& path, size_t limit) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read " + path.string());
  std::string data; char block[4096];
  while (input.read(block, sizeof block) || input.gcount()) {
    data.append(block, size_t(input.gcount()));
    if (data.size() > limit) throw std::runtime_error("File exceeds size limit");
  }
  if (input.bad()) throw std::runtime_error("File read failed");
  return data;
}
void writeJson(const fs::path& path, const json& value) {
  const auto tmp = path.string() + ".tmp";
  int fd = open(tmp.c_str(), O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("Cannot save job state");
  try {
    const std::string data = value.dump(2) + "\n"; size_t offset = 0;
    while (offset < data.size()) {
      auto n = write(fd, data.data()+offset, data.size()-offset);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("State write failed");
      offset += size_t(n);
    }
    if (fsync(fd)) throw std::runtime_error("State flush failed");
    close(fd); fd = -1;
    fs::rename(tmp, path);
  } catch (...) { if (fd >= 0) close(fd); throw; }
}
fs::path safeRelative(const std::string& name) {
  if (name.empty() || name.find('\0') != std::string::npos || name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
    throw std::runtime_error("Unsafe archive path");
  fs::path path(name);
  if (path.is_absolute() || path.has_root_name()) throw std::runtime_error("Absolute archive path rejected");
  for (const auto& part : path) if (part == ".." || part == ".") throw std::runtime_error("Archive path traversal rejected");
  return path;
}
fs::path containedExisting(const fs::path& root, const fs::path& path) {
  const auto base = fs::canonical(root); const auto full = fs::canonical(path);
  auto b = base.begin(), f = full.begin();
  for (; b != base.end(); ++b, ++f) if (f == full.end() || *b != *f) throw std::runtime_error("Path leaves the allowed directory");
  if (f == full.end()) throw std::runtime_error("Expected an item inside the directory");
  auto lexicalBase = fs::absolute(root).lexically_normal();
  auto current = lexicalBase;
  auto relative = full.lexically_relative(base);
  // Also reject any symlink in the original lexical path, even if it resolves within root.
  auto original = fs::absolute(path).lexically_normal().lexically_relative(lexicalBase);
  safeRelative(original.string());
  for (const auto& part : original) { current /= part; if (fs::is_symlink(fs::symlink_status(current))) throw std::runtime_error("Symbolic links are not supported"); }
  return full;
}
uint64_t freeBytes(const fs::path& path) {
  struct statvfs data{};
  if (statvfs(path.c_str(), &data)) throw std::runtime_error("Cannot read free disk space");
  return uint64_t(data.f_bavail) * uint64_t(data.f_frsize);
}
namespace {
#ifdef __PS5__
// The payload SDK's *at stubs return a positive kernel errno and leave libc
// errno unchanged. For openat that value even looks like a valid descriptor.
// Read the syscall carry flag and normalize to the POSIX contract we use below.
long relativeFileCall(long number,int fd,const char* path,long argument,long flags=0) {
  register long fourth asm("r10")=flags;
  long result=number;bool failed;
  asm volatile("syscall" : "+a"(result), "=@ccc"(failed)
      : "D"(fd), "S"(path), "d"(argument), "r"(fourth) : "rcx","r11","memory");
  if(failed){errno=int(result);return -1;}return result;
}
int openDirectoryAt(int fd,const char* name){return int(relativeFileCall(499,fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW));}
int statFileAt(int fd,const char* name,struct stat* info){return int(relativeFileCall(493,fd,name,reinterpret_cast<long>(info),AT_SYMLINK_NOFOLLOW));}
int unlinkFileAt(int fd,const char* name){return int(relativeFileCall(503,fd,name,0));}
#else
int openDirectoryAt(int fd,const char* name){return openat(fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);}
int statFileAt(int fd,const char* name,struct stat* info){return fstatat(fd,name,info,AT_SYMLINK_NOFOLLOW);}
int unlinkFileAt(int fd,const char* name){return unlinkat(fd,name,0);}
#endif
}
void downloadedFiles(const fs::path& root, const std::vector<fs::path>& files, bool remove) {
  for(const auto& file:files) {
    const auto relative=safeRelative(file.string());
    int fd=open(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0)throw std::runtime_error("Cannot open download directory");
    try {
      bool missing=false;
      for(const auto& part:relative.parent_path()) {
        const int next=openDirectoryAt(fd,part.c_str());
        if(next<0) {
          if(errno==ENOENT){missing=true;break;}
          throw std::runtime_error("Cannot access download directory safely");
        }
        close(fd);fd=next;
      }
      if(!missing) {
        const auto name=relative.filename();struct stat info{};
        if(statFileAt(fd,name.c_str(),&info)) {
          if(errno!=ENOENT)throw std::runtime_error("Cannot inspect downloaded file");
        } else {
          if(!S_ISREG(info.st_mode))throw std::runtime_error("Unexpected download entry; deletion refused");
          if(remove) {
            if(unlinkFileAt(fd,name.c_str())&&errno!=ENOENT)throw std::runtime_error("Cannot delete downloaded file; torrent kept paused for retry");
            if(statFileAt(fd,name.c_str(),&info)==0||errno!=ENOENT)
              throw std::runtime_error("Downloaded file still exists; torrent kept paused for retry");
          }
        }
      }
      close(fd);
    } catch(...) {close(fd);throw;}
  }
}
std::string randomId() {
  unsigned char bytes[16]; arc4random_buf(bytes, sizeof bytes);
  const char* hex = "0123456789abcdef"; std::string out;
  for (auto b : bytes) { out += hex[b >> 4]; out += hex[b & 15]; }
  return out;
}
json classify(const fs::path& root) {
  std::vector<json> candidates;
  size_t count = 0;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (++count > 200000) throw std::runtime_error("Too many extracted entries");
    if (entry.is_symlink()) throw std::runtime_error("Extracted links are not allowed");
    if (!entry.is_regular_file()) continue;
    const auto path = entry.path();
    if (path.filename() == "param.json" && path.parent_path().filename() == "sce_sys") {
      auto param = json::parse(readText(path));
      std::string title;
      for (const char* key : {"titleId", "title_id", "TITLE_ID"}) if (param.contains(key) && param[key].is_string()) title = param[key].get<std::string>();
      if (!std::regex_match(title, std::regex("PPSA[0-9]{5}"))) continue;
      auto app = path.parent_path().parent_path();
      candidates.push_back({{"kind","folder"},{"source",app.lexically_relative(root).string()},{"destination",title+"-app"},{"titleId",title}});
    } else if (path.extension() == ".exfat") {
      std::ifstream file(path, std::ios::binary); char header[11]{}; file.read(header, sizeof header);
      if (file.gcount() == sizeof header && std::string(header+3,8) == "EXFAT   ")
        candidates.push_back({{"kind","exfat"},{"source",path.lexically_relative(root).string()},{"destination",path.filename().string()}});
    }
  }
  if (candidates.size() != 1) return {{"kind","unsupported"},{"reason",candidates.empty()?"No supported app folder or exFAT image found. PKG installation is not included.":"Multiple app/image candidates found. Manual selection is required."}};
  return candidates.front();
}
// Extraction stays private; only verified content being published becomes readable
// by the game sandbox. Refuse links and special files, and never follow a leaf link.
static void prepareLibraryPermissions(const fs::path& source) {
  const auto prepare=[](const fs::path& path) {
    int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)throw std::runtime_error("Cannot open library content for permission preparation");
    struct stat info{};
    if(fstat(fd,&info)||(!S_ISDIR(info.st_mode)&&!S_ISREG(info.st_mode))){close(fd);throw std::runtime_error("Unsupported library entry");}
    const auto ext=path.extension().string();
    const bool executable=path.filename()=="eboot.bin"||ext==".elf"||ext==".self"||ext==".prx"||ext==".sprx";
    const mode_t mode=S_ISDIR(info.st_mode)||executable?0755:0644;
    const int result=fchmod(fd,mode);close(fd);
    if(result)throw std::runtime_error("Cannot prepare library permissions; content was not moved");
  };
  if(fs::is_symlink(fs::symlink_status(source)))throw std::runtime_error("Extracted links are not allowed");
  prepare(source);
  if(fs::is_directory(source))for(const auto& entry:fs::recursive_directory_iterator(source)){
    if(entry.is_symlink())throw std::runtime_error("Extracted links are not allowed");
    prepare(entry.path());
  }
}
json movePrepared(const Paths& paths, json job) {
  if (job.value("status", "") != "ready") throw std::runtime_error("Extraction is not ready to move");
  const std::string id = job.at("id");
  if (!std::regex_match(id,std::regex("[a-f0-9]{32}"))) throw std::runtime_error("Invalid job ID");
  const auto root = containedExisting(paths.extracted, paths.extracted/id);
  const json content = classify(root); // Never trust a stale client-provided source/destination.
  if (content.value("kind", "") == "unsupported") throw std::runtime_error(content.at("reason"));
  const std::string relative = content.at("source");
  const auto source = relative.empty() || relative == "." ? root : containedExisting(root, root/safeRelative(relative));
  const auto name = safeRelative(content.at("destination").get<std::string>());
  if (name.has_parent_path()) throw std::runtime_error("Invalid library filename");
  fs::create_directories(paths.library);
  if (fs::is_symlink(fs::symlink_status(paths.library))) throw std::runtime_error("Library directory is a symbolic link");
  const auto target = paths.library/name;
  if (fs::exists(fs::symlink_status(target))) throw std::runtime_error("Destination already exists; nothing was replaced");
  prepareLibraryPermissions(source);
  // Journal the move before it starts, so a crash is reported as uncertain on restart.
  job["status"]="moving"; job["destination"]=target.string(); writeJson(paths.jobs/(id+".json"),job);
  try {
    if (fs::is_regular_file(source)) {
      // Atomic no-overwrite publication on the same filesystem.
      if (link(source.c_str(), target.c_str())) throw std::runtime_error("Cannot publish image without overwriting: "+std::string(strerror(errno)));
      if (unlink(source.c_str())) throw std::runtime_error("Image published, but source cleanup failed; inspect both paths");
    } else fs::rename(source,target); // Existing nonempty directories cannot be replaced.
  } catch (const std::exception& error) {
    job["status"]="move-error"; job["error"]=error.what(); writeJson(paths.jobs/(id+".json"),job); throw;
  }
  job["status"]="moved"; job["content"]=content; writeJson(paths.jobs/(id+".json"),job);
  return job;
}
void deleteLibraryGame(const Paths& paths, const json& job) {
  const std::string id=job.at("id"),status=job.value("status","");
  if(!std::regex_match(id,std::regex("[a-f0-9]{32}")) || (status!="moved"&&status!="library-delete-error"))
    throw std::runtime_error("Only a game moved to Library can be deleted");
  const auto content=job.at("content");
  const std::string title=content.value("titleId","");
  if(content.value("kind","")!="folder" || !std::regex_match(title,std::regex("PPSA[0-9]{5}")) || title=="PPSA99071")
    throw std::runtime_error("Only tracked game folders can be deleted; mounted images require manual removal");
  const std::string name=title+"-app";
  if(content.value("destination","")!=name || job.value("destination","")!=(paths.library/name).string())
    throw std::runtime_error("Library destination does not match the recorded game");
  if(fs::is_symlink(fs::symlink_status(paths.library)))throw std::runtime_error("Library links cannot be deleted");
  const auto target=paths.library/name;
  if(!fs::exists(fs::symlink_status(target)))return; // Retry after external removal or interrupted cleanup.
  const auto full=containedExisting(paths.library,target);
  const auto overlaps=[&](const fs::path& path){
    const auto value=path.lexically_normal().string(),base=full.string();
    return value==base || value.rfind(base+"/",0)==0;
  };
#if defined(__PS5__) || defined(__APPLE__)
  struct statfs* mounts=nullptr;const int count=getmntinfo(&mounts,MNT_NOWAIT);
  if(count<=0)throw std::runtime_error("Cannot verify mounted games; deletion refused");
  for(int i=0;i<count;++i)if(overlaps(mounts[i].f_mntonname)||overlaps(mounts[i].f_mntfromname))
    throw std::runtime_error("Game is mounted. Close and unmount it before deleting its files");
#else
  std::ifstream mounts("/proc/mounts");if(!mounts)throw std::runtime_error("Cannot verify mounted games");
  std::string source,mount,rest;while(mounts>>source>>mount){std::getline(mounts,rest);if(overlaps(source)||overlaps(mount))throw std::runtime_error("Game is mounted; unmount it before deletion");}
#endif
  struct stat base{};if(lstat(paths.library.c_str(),&base))throw std::runtime_error("Cannot inspect library");
  const auto check=[&](const fs::path& path){struct stat st{};
    if(lstat(path.c_str(),&st)||st.st_dev!=base.st_dev||(!S_ISDIR(st.st_mode)&&!S_ISREG(st.st_mode)))
      throw std::runtime_error("Library game contains a mount, link or unsupported file; deletion refused");
  };
  check(full);if(!fs::is_directory(full))throw std::runtime_error("Recorded game folder is not a directory");
  // Validate the complete tree before deleting anything. remove_all never follows links.
  for(const auto& entry:fs::recursive_directory_iterator(full))check(entry.path());
  fs::remove_all(full);
  if(fs::exists(fs::symlink_status(target)))throw std::runtime_error("Game files remain; retry deletion");
}

}
