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
}
