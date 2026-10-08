#define CURL_DISABLE_TYPECHECK
#include <curl/curl.h>
#include <cassert>
#include <condition_variable>
#include <future>
#include <iostream>
#include <unistd.h>
#include <type_traits>

namespace {
std::mutex gate;
std::condition_variable wake;
bool writing=false,released=false;
bool performing=false,performReleased=false;
curl_write_callback receive=nullptr;
void* context=nullptr;
}
template<class T> CURLcode fixtureSetopt(CURL*,CURLoption option,T value) {
  if constexpr(std::is_same_v<T,curl_write_callback>){if(option==CURLOPT_WRITEFUNCTION)receive=value;}
  else if constexpr(std::is_pointer_v<T>){if(option==CURLOPT_WRITEDATA)context=const_cast<void*>(static_cast<const void*>(value));}
  return CURLE_OK;
}
inline CURLcode fixtureGetinfo(CURL*,CURLINFO,long* value){*value=200;return CURLE_OK;}
#undef curl_easy_setopt
#undef curl_easy_getinfo
#define curl_easy_setopt fixtureSetopt
#define curl_easy_getinfo fixtureGetinfo
#include "search.hpp"
extern "C" {
CURL* curl_easy_init(){return reinterpret_cast<CURL*>(1);}
void curl_easy_cleanup(CURL*){}
CURLcode curl_easy_perform(CURL*){{std::unique_lock<std::mutex> lock(gate);performing=true;wake.notify_all();wake.wait(lock,[]{return performReleased;});}char bytes[]="[]";assert(receive(bytes,1,2,context)==2);return CURLE_OK;}
curl_slist* curl_slist_append(curl_slist*,const char*){static curl_slist header{};return &header;}
void curl_slist_free_all(curl_slist*){}
}
namespace botty {
std::string readText(const fs::path& path,size_t) {
  if(path.filename()=="prowlarr.json")return json({{"url","https://fixture.invalid"},{"apiKey",std::string(32,'a')}}).dump();
  throw std::runtime_error("No cached fixture");
}
std::string randomId(){return std::string(32,'a');}
void writeJson(const fs::path&,const json&) {
  std::unique_lock<std::mutex> lock(gate);writing=true;wake.notify_all();wake.wait(lock,[]{return released;});
}
}
int main() {
  const auto root=botty::fs::temp_directory_path()/("botty-search-idle-"+std::to_string(getpid()));botty::fs::create_directory(root);
  botty::Search search;search.start(botty::Paths(root),"PS5","newest",true);
  {std::unique_lock<std::mutex> lock(gate);assert(wake.wait_for(lock,std::chrono::seconds(2),[]{return performing;}));}
  bool busyRejected=false;try{search.requireInstallationIdle();}catch(const std::runtime_error&){busyRejected=true;}assert(busyRejected);
  {std::lock_guard<std::mutex> lock(gate);performReleased=true;}wake.notify_all();
  {std::unique_lock<std::mutex> lock(gate);assert(wake.wait_for(lock,std::chrono::seconds(2),[]{return writing;}));}
  auto idle=std::async(std::launch::async,[&]{search.requireInstallationIdle();});
  assert(idle.wait_for(std::chrono::milliseconds(100))==std::future_status::timeout);
  {std::lock_guard<std::mutex> lock(gate);released=true;}wake.notify_all();idle.get();assert(!search.state().at("busy").get<bool>());
  botty::fs::remove_all(root);std::cout<<"Search idle gate waits through mutex-protected cache flush\n";
}
