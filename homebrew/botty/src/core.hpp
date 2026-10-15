#pragma once
#include "json.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace botty {
namespace fs = std::filesystem;
using json = nlohmann::json;
struct Paths {
  fs::path root, complete, extracted, jobs, library;
  explicit Paths(fs::path data = "/data/botty", fs::path games = "/data/homebrew")
    : root(std::move(data)), complete(root/"downloads/complete"), extracted(root/"extracted"), jobs(root/"jobs"), library(std::move(games)) {}
};
// Host test builds may shorten the 5 s background polling with BOTTY_TEST_POLL_MS
// (50..5000); PS5 builds always poll every 5 s.
inline std::chrono::milliseconds backgroundPollInterval(){
#ifndef __PS5__
  if(const char* value=std::getenv("BOTTY_TEST_POLL_MS")){char* end=nullptr;const long ms=std::strtol(value,&end,10);if(end!=value&&!*end&&ms>=50&&ms<=5000)return std::chrono::milliseconds(ms);}
#endif
  return std::chrono::seconds(5);
}
inline std::string asciiLower(std::string text){for(char& c:text)if(c>='A'&&c<='Z')c=char(c+32);return text;}
int makeDirectoryRelative(int,const char*,unsigned);
int openFileRelative(int,const char*,int,unsigned=0);
std::string readText(const fs::path&, size_t limit = 1024*1024);
void writeJson(const fs::path&, const json&);
fs::path safeRelative(const std::string&);
fs::path containedExisting(const fs::path& root, const fs::path& path);
uint64_t freeBytes(const fs::path&);
using DeleteProgress = std::function<void(uint64_t,uint64_t,const std::string&)>;
// Exact torrent members only; never recurse into a download or library folder.
void downloadedFiles(const fs::path& root, const std::vector<fs::path>& files, bool remove, const DeleteProgress& progress = {});
std::string randomId();
json classify(const fs::path& extracted);
json movePrepared(const Paths&, json job, const Paths* destination = nullptr, const std::function<void()>& check = {});
void prepareLibraryPermissions(const fs::path&,const std::function<void()>& check={});
void deleteGameDirectory(const fs::path& root,const fs::path& name, const DeleteProgress& progress = {});
void deleteLibraryGame(const Paths&, const json& job, const DeleteProgress& progress = {});
struct Progress { std::string phase, file; uint64_t bytes = 0, total = 0; };
using Reporter = std::function<void(const Progress&)>;
// Progress callbacks are serialized; cancellation may be queried concurrently.
void extractRar(const fs::path& archive, const fs::path& destination, Reporter report, const std::string& password = "", std::function<bool()> cancelled = {}, unsigned workers = 0, bool resume = false);
}
