#pragma once
#include "json.hpp"
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
std::string readText(const fs::path&, size_t limit = 1024*1024);
void writeJson(const fs::path&, const json&);
fs::path safeRelative(const std::string&);
fs::path containedExisting(const fs::path& root, const fs::path& path);
uint64_t freeBytes(const fs::path&);
std::string randomId();
json classify(const fs::path& extracted);
json movePrepared(const Paths&, json job);
struct Progress { std::string phase, file; uint64_t bytes = 0, total = 0; };
using Reporter = std::function<void(const Progress&)>;
// Progress callbacks are serialized; cancellation may be queried concurrently.
void extractRar(const fs::path& archive, const fs::path& destination, Reporter report, const std::string& password = "", std::function<bool()> cancelled = {}, unsigned workers = 0);
}
