#pragma once
#include "core.hpp"
#include "../vendor/unrar/rartypes.hpp"
#include "../vendor/unrar/sha256.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <regex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace botty {
inline std::string nativeSha256(const std::string& bytes) {
  byte digest[SHA256_DIGEST_SIZE];
  sha256_get(bytes.data(),bytes.size(),digest);
  std::string out;
  for (auto value:digest) {
    out += "0123456789abcdef"[value>>4];
    out += "0123456789abcdef"[value&15];
  }
  return out;
}

class NativeTransaction {
public:
  friend std::string nativeRead(const fs::path&, size_t);
  friend void nativeWrite(const fs::path&, const std::string&, bool);
  friend void validateNativeManifest(const json&);
  friend std::string nativeTransactionId();
  struct Config {
    fs::path nativeRoot = "/data/homebrew/PPSA99071";
    fs::path stateRoot = "/data/botty/native";
    std::array<fs::path,3> metadataRoots = {{"/user/app/PPSA99071/sce_sys", "/user/appmeta/PPSA99071", "/system_data/priv/appmeta/PPSA99071"}};
    std::function<void(const std::string&)> checkpoint;
  };
  using Fetch = std::function<std::string(const std::string&, size_t)>;
  using Gate = std::function<void()>;
  static constexpr size_t maxFileBytes = 32*1024*1024;
  static const std::array<std::string,13>& files() {
    static const std::array<std::string,13> value = {{"assets/Manrope-OFL.txt","assets/build.txt","assets/nebula.rgb","assets/courier.rgba","assets/extractor.rgba","assets/vault.rgba","assets/ui-font.bin","eboot.bin","sce_module/libc.prx","sce_sys/icon0.png","sce_sys/pic0.dds","sce_sys/param.json","sce_sys/snd0.at9"}};
    return value;
  }
  NativeTransaction() : NativeTransaction(Config{}) {}
  explicit NativeTransaction(Config config) : cfg(std::move(config)) {
    for (const auto& p : {cfg.nativeRoot,cfg.stateRoot}) validPath(p);
    for (const auto& p : cfg.metadataRoots) validPath(p);
  }
  std::string installedVersion() const {
    auto data=read(cfg.nativeRoot/"sce_sys/param.json",16384);
    if (!data) fail("Installed title has no identity");
    return identity(*data).at("contentVersion").get<std::string>();
  }
  std::string installedFingerprint() const {
    installedVersion();
    inspectTree(cfg.nativeRoot);
    json digests=json::array();
    for (const auto& file:files()) {
      auto data=read(cfg.nativeRoot/file,maxFileBytes);
      if (!data || data->empty()) fail("Installed native tree is incomplete");
      digests.push_back({{"path",file},{"size",data->size()},{"sha256",nativeSha256(*data)}});
    }
    return nativeSha256(digests.dump());
  }
  bool pendingJournal() const {
    auto record=journal();
    return record && record->at("status")=="pending";
  }
  bool targetComplete(const json& manifest,const std::string& hash) const {
    validate(manifest);checkHash(hash);auto record=journal();
    return record&&record->at("status")=="complete"&&record->at("target")==hash&&matches(cfg.nativeRoot,manifest);
  }
  bool stagedComplete(const json& manifest,const std::string& hash) const {
    validate(manifest);checkHash(hash);const auto root=stageRoot(hash);
    if(!exists(root))return false;
    inspectStage(root);
    const auto param=read(root/"sce_sys/param.json",16384);
    return matches(root,manifest)&&param&&identity(*param).at("contentVersion")==manifest.at("version");
  }
  bool canReleaseRequest(const std::string& fingerprint) const {
    checkHash(fingerprint);
    if (pendingJournal()) return false;
    if (installedFingerprint()!=fingerprint) fail("Installed native tree changed; request admission remains blocked");
    return true;
  }
  void preflight(const json& manifest) const {
    validate(manifest);
    capability(cfg.stateRoot); capability(cfg.nativeRoot.parent_path());
    if (exists(cfg.stateRoot)) inspectTree(cfg.stateRoot);
    if (exists(cfg.nativeRoot)) {
      capability(cfg.nativeRoot);
      auto version=installedVersion();
      if (version>manifest.at("version").get<std::string>()) fail("Native downgrade refused");
      inspectTree(cfg.nativeRoot);
    }
    for (const auto& root:cfg.metadataRoots) {
      auto param=read(root/"param.json",16384);
      if (param) {
        identity(*param);
        if (identity(*param).at("contentVersion").get<std::string>()>manifest.at("version").get<std::string>()) fail("Metadata downgrade refused");
        capability(root);
        if (root==cfg.metadataRoots[0]) capability(root.parent_path());
        for (const auto& name:{"param.json","icon0.png","pic0.dds","snd0.at9"}) read(root/name,maxFileBytes);
        if (root==cfg.metadataRoots[0]) read(root.parent_path()/"icon0.png",maxFileBytes);
      }
    }
    journal();
  }
  void stage(const json& manifest,const std::string& hash,const Fetch& fetch) {
    checkHash(hash); preflight(manifest);
    installedVersion();
    auto record=journal();
    if (record && record->at("status")=="pending") fail("Recover pending native update before staging");
    auto root=stageRoot(hash);
    if(exists(root)) inspectStage(root);
    for (const auto& f:manifest.at("files")) {
      auto path=root/f.at("path").get<std::string>();
      if (matchesFile(path,f)) {
        auto data=read(path,maxFileBytes); write(path,*data,mode(f.at("path").get<std::string>())); continue;
      }
      auto data=fetch(f.at("path").get<std::string>(),f.at("size").get<size_t>());
      verify(data,f); mkdirs(path.parent_path()); write(path,data,mode(f.at("path").get<std::string>()));
      if (!matchesFile(path,f)) fail("Staged native file verification failed");
    }
    identity(*read(root/"sce_sys/param.json",16384));
    if (identity(*read(root/"sce_sys/param.json",16384)).at("contentVersion")!=manifest.at("version")) fail("Package identity version mismatch");
    inspectStage(root);
    if (!matches(root,manifest)) fail("Native staging verification failed");
    for (const auto& dir:{root/"assets",root/"sce_module",root/"sce_sys",root}) {
      ancestors(dir);
      if (::chmod(dir.c_str(),0755)) fail("Cannot set native directory permissions");
      sync(dir);
    }
  }
  void publish(const json& manifest,const std::string& hash,const Gate& gate) {
    checkHash(hash); preflight(manifest);
    recover(manifest,hash,gate);
    if(exists(stageRoot(hash))) inspectStage(stageRoot(hash));
    if (!matches(stageRoot(hash),manifest)) {
      if (matches(cfg.nativeRoot,manifest)) return;
      fail("Native staging verification failed");
    }
    auto param=read(stageRoot(hash)/"sce_sys/param.json",16384);
    if (!param || identity(*param).at("contentVersion")!=manifest.at("version")) fail("Package identity version mismatch");
    auto old=read(cfg.nativeRoot/"sce_sys/param.json",16384);
    if (!old) fail("Native updater requires an installed title");
    requireGate(gate); preflight(manifest);
    auto current=read(cfg.nativeRoot/"sce_sys/param.json",16384);
    if (!current || nativeSha256(*current)!=nativeSha256(*old)) fail("Installed native identity changed during preparation");
    mkdirs(cfg.stateRoot/"backups");
    std::string id=randomHex();
    auto backup=cfg.stateRoot/"backups"/id/"PPSA99071";
    mkdirs(backup.parent_path());
    json rec={{"schema",1},{"status","pending"},{"target",hash},{"previous",old ? nativeSha256(*old) : nativeSha256("")},{"backup",backup.string()}};
    saveJournal(rec); point("pending");
    requireGate(gate);
    if (old) { move(cfg.nativeRoot,backup); point("backup"); }
    move(stageRoot(hash),cfg.nativeRoot); point("published");
    if (!matches(cfg.nativeRoot,manifest)) fail("Published native verification failed");
    metadata(manifest,backup);
    rec["status"]="complete"; saveJournal(rec); point("complete");
  }
  void recover(const json& manifest,const std::string& hash,const Gate& gate) {
    checkHash(hash); validate(manifest);
    auto rec=journal();
    if (!rec || rec->at("status")!="pending") return;
    requireGate(gate); capability(cfg.stateRoot); capability(cfg.nativeRoot.parent_path());
    auto backup=fs::path(rec->at("backup").get<std::string>());
    if (rec->at("target")==hash && matches(cfg.nativeRoot,manifest)) {
      metadata(manifest,backup); (*rec)["status"]="complete"; saveJournal(*rec); return;
    }
    auto current=read(cfg.nativeRoot/"sce_sys/param.json",16384);
    if (current && nativeSha256(*current)==rec->at("previous")) {
      (*rec)["status"]="rolled-back"; saveJournal(*rec); return;
    }
    auto saved=read(backup/"sce_sys/param.json",16384);
    if (!saved || nativeSha256(*saved)!=rec->at("previous")) fail("Native recovery backup unavailable; files retained");
    identity(*saved); inspectTree(backup);
    if (exists(cfg.nativeRoot)) {
      if (::rmdir(cfg.nativeRoot.c_str())) fail("Unexpected native recovery destination; files retained");
      sync(cfg.nativeRoot.parent_path());
    }
    auto restore=backup.parent_path()/"restore";
    if (exists(restore)) fail("Recovery staging already exists; files retained");
    copyTree(backup,restore); move(restore,cfg.nativeRoot);
    (*rec)["status"]="rolled-back"; saveJournal(*rec);
  }
private:
  Config cfg;
  [[noreturn]] static void fail(const std::string& what) { throw std::runtime_error(what); }
  static void validPath(const fs::path& p) {
    if (!p.is_absolute() || p.lexically_normal()!=p) fail("Invalid native root path");
    for (const auto& part:p) if (part==".." || part==".") fail("Invalid native path");
  }
  static void checkHash(const std::string& s) { if (!std::regex_match(s,std::regex("[a-f0-9]{64}"))) fail("Invalid native hash"); }
  static void ancestors(const fs::path& p) {
    validPath(p); fs::path current;
    for (const auto& part:p) {
      current/=part; struct stat st{};
      if (::lstat(current.c_str(),&st)) { if (errno==ENOENT) continue; fail("Cannot inspect native path"); }
      if (S_ISLNK(st.st_mode)) fail("Native symlink refused");
      if (current!=p && !S_ISDIR(st.st_mode)) fail("Native ancestor is not a directory");
    }
  }
  static bool exists(const fs::path& p) {
    ancestors(p); struct stat st{};
    if (!::lstat(p.c_str(),&st)) return true;
    if (errno!=ENOENT) fail("Cannot inspect native path");
    return false;
  }
  static void capability(fs::path p) {
    ancestors(p);
    while (!exists(p)) p=p.parent_path();
    struct stat st{};
    if (::lstat(p.c_str(),&st) || !S_ISDIR(st.st_mode) || st.st_uid!=::geteuid() || (st.st_mode&0022) || (st.st_mode&0700)!=0700 || ::access(p.c_str(),R_OK|W_OK|X_OK)) fail("Manager cannot safely write native or registered metadata directories. Use the Portal installer to restore required ownership and permissions.");
    sync(p);
  }
  static std::optional<std::string> read(const fs::path& p,size_t limit) {
    if (limit>maxFileBytes) fail("Native read exceeds fixed bound");
    ancestors(p); int fd=::open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) { if (errno==ENOENT) return {}; fail("Cannot read native file"); }
    struct stat st{};
    if (::fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<0 || uint64_t(st.st_size)>limit) { ::close(fd); fail("Invalid or oversized native file"); }
    std::string data(size_t(st.st_size),'\0'); size_t done=0;
    while (done<data.size()) { auto n=::read(fd,&data[done],data.size()-done); if(n<0 && errno==EINTR) continue; if(n<=0) {::close(fd);fail("Short native read");} done+=size_t(n); }
    char extra; auto n=::read(fd,&extra,1); ::close(fd); if(n!=0) fail("Native file changed during read"); return data;
  }
  static void sync(const fs::path& p) {
    ancestors(p); int fd=::open(p.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if (fd<0) fail("Cannot open native directory");
    int result=::fsync(fd); ::close(fd); if(result) fail("Cannot flush native directory");
  }
  static void mkdirs(const fs::path& p) {
    if (exists(p)) { capability(p); return; }
    mkdirs(p.parent_path());
    if (::mkdir(p.c_str(),0755)) fail("Cannot create native directory");
    sync(p.parent_path());
  }
  static void write(const fs::path& p,const std::string& data,mode_t permissions,bool exclusive=false) {
    capability(p.parent_path()); ancestors(p);
    auto temp=p.parent_path()/(p.filename().string()+"."+randomHex()+".tmp");
    int fd=::open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0) fail("Cannot create native temporary file");
    size_t done=0;
    while(done<data.size()) { auto n=::write(fd,data.data()+done,data.size()-done); if(n<0&&errno==EINTR) continue; if(n<=0) {::close(fd);fail("Cannot write native file");} done+=size_t(n); }
    int result=::fchmod(fd,permissions); if(!result) result=::fsync(fd); ::close(fd);
    if(result) fail("Cannot flush native file");
    ancestors(p);
    if (exclusive) {
      if (::link(temp.c_str(),p.c_str())) fail("Native destination exists or cannot be linked; files retained");
      if (::unlink(temp.c_str())) fail("Cannot remove native temporary link");
    } else if (::rename(temp.c_str(),p.c_str())) fail("Cannot publish native file");
    sync(p.parent_path());
  }
  static std::string randomHex() {
    int fd=::open("/dev/urandom",O_RDONLY); if(fd<0) fail("Native random source unavailable");
    unsigned char bytes[16]; size_t n=0;
    while(n<sizeof(bytes)) { auto got=::read(fd,bytes+n,sizeof(bytes)-n); if(got<0&&errno==EINTR) continue; if(got<=0) {::close(fd);fail("Native random source failed");} n+=size_t(got); } ::close(fd);
    std::string s; for(auto b:bytes) {s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];} return s;
  }
  static mode_t mode(const std::string& p) { return p=="eboot.bin"||p=="sce_module/libc.prx" ? 0755:0644; }
  static json identity(const std::string& data) {
    auto p=json::parse(data); auto title=p.value("localizedParameters",json::object()).value("en-US",json::object()).value("titleName","");
    if(p.value("titleId","")!="PPSA99071" || p.value("contentId","")!="UP9000-PPSA99071_00-BOTTYNATIVE00001" || !std::regex_match(p.value("contentVersion",""),std::regex("[0-9]{2}\\.[0-9]{3}\\.[0-9]{3}")) || (title!="Botty+"&&title!="Botty Native Preview"&&title!="Botty Native")) fail("Unrecognized native identity");
    return p;
  }
  static void validate(const json& m) {
    if(m.at("schema")!=1 || m.at("titleId")!="PPSA99071" || !std::regex_match(m.at("version").get<std::string>(),std::regex("[0-9]{2}\\.[0-9]{3}\\.[0-9]{3}")) || !m.at("files").is_array() || m.at("files").size()!=13) fail("Invalid native manifest");
    std::vector<std::string> seen;
    for(const auto& f:m.at("files")) {
      auto path=f.at("path").get<std::string>();
      if(std::find(files().begin(),files().end(),path)==files().end() || std::find(seen.begin(),seen.end(),path)!=seen.end() || !f.at("size").is_number_integer() || f.at("size").get<int64_t>()<1 || f.at("size").get<int64_t>()>int64_t(maxFileBytes)) fail("Invalid native manifest file");
      checkHash(f.at("sha256").get<std::string>()); seen.push_back(path);
    }
  }
  static void verify(const std::string& data,const json& f) { if(data.size()!=f.at("size").get<size_t>() || nativeSha256(data)!=f.at("sha256")) fail("Native file hash mismatch"); }
  static bool matchesFile(const fs::path& p,const json& f) { auto data=read(p,maxFileBytes); return data && data->size()==f.at("size").get<size_t>() && nativeSha256(*data)==f.at("sha256"); }
  static bool matches(const fs::path& root,const json& m) { for(const auto& f:m.at("files")) if(!matchesFile(root/f.at("path").get<std::string>(),f)) return false; return true; }
  fs::path stageRoot(const std::string& hash) const { return cfg.stateRoot/hash/"PPSA99071"; }
  std::optional<json> journal() const {
    auto data=read(cfg.stateRoot/"update.json",8192); if(!data) return {};
    auto j=json::parse(*data); auto status=j.at("status").get<std::string>();
    checkHash(j.at("target").get<std::string>()); checkHash(j.at("previous").get<std::string>());
    auto p=fs::path(j.at("backup").get<std::string>());
    const bool legacyComplete=status=="complete"&&(p==cfg.stateRoot/"backups/botty-131-20261003/PPSA99071"||p==cfg.stateRoot/"backups/botty-131-stackfix-20261003/PPSA99071");
    if(j.at("schema")!=1 || (status!="pending"&&status!="complete"&&status!="rolled-back") || p.parent_path().parent_path()!=cfg.stateRoot/"backups" || p.filename()!="PPSA99071" || (!legacyComplete&&!std::regex_match(p.parent_path().filename().string(),std::regex("[a-f0-9]{32}")))) fail("Damaged native journal; files retained");
    ancestors(p); return j;
  }
  void saveJournal(const json& j) { mkdirs(cfg.stateRoot); write(cfg.stateRoot/"update.json",j.dump()+"\n",0600); }
  static void requireGate(const Gate& gate) { if(!gate) fail("Native publication requires a gate"); gate(); }
  void point(const std::string& name) { if(cfg.checkpoint) cfg.checkpoint(name); }
  static void move(const fs::path& from,const fs::path& to) {
    ancestors(from); capability(from.parent_path()); capability(to.parent_path());
    if (::mkdir(to.c_str(),0755)) fail("Native destination exists; files retained");
    sync(to.parent_path());
    if(::rename(from.c_str(),to.c_str())) fail("Native rename failed; files retained");
    sync(from.parent_path()); sync(to.parent_path());
  }
  static void inspectTree(const fs::path& root) {
    ancestors(root);
    for(const auto& e:fs::recursive_directory_iterator(root)) {
      ancestors(e.path()); auto s=e.symlink_status();
      if(!fs::is_directory(s)&&!fs::is_regular_file(s)) fail("Unexpected native tree entry");
      if(fs::is_directory(s)) capability(e.path());
      if(fs::is_regular_file(s)&&e.file_size()>maxFileBytes) fail("Oversized native backup entry");
    }
  }
  static void inspectStage(const fs::path& root) {
    inspectTree(root);
    for(const auto& e:fs::recursive_directory_iterator(root)) {
      const auto relative=e.path().lexically_relative(root).generic_string();
      if(e.is_directory()) {
        if(relative!="assets"&&relative!="sce_module"&&relative!="sce_sys")fail("Unexpected directory in native staging; files retained");
      }else if(std::find(files().begin(),files().end(),relative)==files().end())fail("Unexpected file in native staging; files retained");
    }
  }
  static void copyTree(const fs::path& from,const fs::path& to) {
    inspectTree(from);
    mkdirs(to);
    std::vector<std::pair<fs::path,mode_t>> directories;
    struct stat root{};
    if(::lstat(from.c_str(),&root)||!S_ISDIR(root.st_mode))fail("Cannot inspect backup root mode");
    directories.emplace_back(to,root.st_mode&07777);
    for(const auto& e:fs::recursive_directory_iterator(from)) {
      auto dest=to/e.path().lexically_relative(from);
      if(e.is_directory()) {
        struct stat st{};if(::lstat(e.path().c_str(),&st)||!S_ISDIR(st.st_mode))fail("Cannot inspect backup directory mode");
        mkdirs(dest);directories.emplace_back(dest,st.st_mode&07777);
      }
      else {
        auto data=read(e.path(),maxFileBytes);struct stat st{};if(::lstat(e.path().c_str(),&st))fail("Cannot inspect backup mode");write(dest,*data,st.st_mode&0777);
        auto restored=read(dest,maxFileBytes);if(!restored||restored->size()!=data->size()||nativeSha256(*restored)!=nativeSha256(*data))fail("Restored backup verification failed; backup retained");
      }
    }
    for(auto it=directories.rbegin();it!=directories.rend();++it) {
      ancestors(it->first);
      if(::chmod(it->first.c_str(),it->second))fail("Cannot restore backup directory mode");
      sync(it->first);
    }
  }
  void metadata(const json& m,const fs::path& backup) {
    std::vector<fs::path> paths;
    for(const auto& root:cfg.metadataRoots) for(const auto& name:{"param.json","icon0.png","pic0.dds"}) paths.push_back(root/name);
    paths.push_back(cfg.metadataRoots[0].parent_path()/"icon0.png");
    for(const auto& root:cfg.metadataRoots) paths.push_back(root/"snd0.at9");
    std::array<bool,3> eligible{};
    for(size_t i=0;i<3;++i) {
      auto p=read(cfg.metadataRoots[i]/"param.json",16384);
      if(p) {
        if(identity(*p).at("contentVersion").get<std::string>()>m.at("version").get<std::string>()) fail("Metadata downgrade refused");
        capability(cfg.metadataRoots[i]);eligible[i]=true;
        for(const auto& name:{"param.json","icon0.png","pic0.dds","snd0.at9"}) read(cfg.metadataRoots[i]/name,maxFileBytes);
        if(i==0) {capability(cfg.metadataRoots[0].parent_path());read(cfg.metadataRoots[0].parent_path()/"icon0.png",maxFileBytes);}
      }
    }
    for(size_t i=0;i<paths.size();++i) {
      size_t rootIndex=i<9?i/3:i==9?0:i-10;
      if(!eligible[rootIndex]) continue;
      const auto& path=paths[i]; std::string relative="sce_sys/"+path.filename().string();
      auto f=std::find_if(m.at("files").begin(),m.at("files").end(),[&](const json& value){return value.at("path")==relative;});
      auto source=read(cfg.nativeRoot/relative,maxFileBytes); if(!source) fail("Native metadata source unavailable"); verify(*source,*f);
      auto old=read(path,maxFileBytes); if(old&&nativeSha256(*old)==f->at("sha256")) continue;
      auto saved=backup.parent_path()/"metadata"/(std::to_string(i)+".bin");
      if(old) {
        auto existing=read(saved,maxFileBytes);
        if(!existing) {
          mkdirs(saved.parent_path());write(saved,*old,0600);existing=read(saved,maxFileBytes);
          if(!existing || nativeSha256(*existing)!=nativeSha256(*old)) fail("Metadata backup verification failed");
        }
        if(!existing) fail("Metadata backup unavailable");
      }
      write(path,*source,0644); if(!matchesFile(path,*f)) fail("Registered metadata verification failed");
    }
  }
};
inline std::string nativeHash(const std::string& bytes) { return nativeSha256(bytes); }
inline std::string nativeTransactionId() { return NativeTransaction::randomHex(); }
inline bool nativeFileAllowed(const std::string& path) {
  const auto& files=NativeTransaction::files();
  return std::find(files.begin(),files.end(),path)!=files.end();
}
inline void validateNativeManifest(const json& manifest) { NativeTransaction::validate(manifest); }
inline std::string nativeRead(const fs::path& path, size_t limit) {
  auto data=NativeTransaction::read(path,limit);
  if (!data) throw std::runtime_error("Native file unavailable");
  return *data;
}
inline void nativeWrite(const fs::path& path,const std::string& bytes,bool exclusive=false) {
  if (bytes.size()>NativeTransaction::maxFileBytes) throw std::runtime_error("Native write exceeds fixed bound");
  NativeTransaction::capability(path.parent_path());
  if (exclusive && NativeTransaction::exists(path)) throw std::runtime_error("Native destination already exists");
  NativeTransaction::mkdirs(path.parent_path());
  NativeTransaction::write(path,bytes,0600,exclusive);
}
}
