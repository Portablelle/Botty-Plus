// Prowlarr access is confined to one configured origin; URLs never reach the UI.
#pragma once
#include "core.hpp"
#include <curl/curl.h>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cctype>
#include <set>
#include <map>
#include <ctime>
#include <cstdio>
#include <fstream>
namespace botty {
class Search {
  std::mutex mutex;
  json rows=json::array();
  std::map<std::string,std::string> covers;
  std::map<std::string,std::time_t> coverFailedAt;
  unsigned coversLoading=0;
  std::string query,error,notice,order;
  bool busy=false,adding=false;
  struct Buffer {std::string data;size_t limit;};
  static size_t receive(char* data,size_t size,size_t count,void* context){
    auto& out=*static_cast<Buffer*>(context);size_t n=size*count;
    if(n>out.limit-out.data.size())return 0;out.data.append(data,n);return n;
  }
  json config(const Paths& paths){
    auto c=json::parse(readText(paths.root/"prowlarr.json",8192));
    const auto url=c.at("url").get<std::string>(),key=c.at("apiKey").get<std::string>();
    if(url.rfind("https://",0)!=0 || url.find_first_of("?#@\r\n")!=std::string::npos || url.back()=='/' || key.size()!=32 || key.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)throw std::runtime_error("Invalid Prowlarr configuration");
    return c;
  }
  std::string fetch(const Paths& paths,const std::string& path,size_t limit,long timeout=100){
    auto c=config(paths);auto url=c.at("url").get<std::string>()+path;
    auto key="X-Api-Key: "+c.at("apiKey").get<std::string>();
    CURL* curl=curl_easy_init();if(!curl)throw std::runtime_error("Could not initialize HTTPS");
    Buffer data{{},limit};auto headers=curl_slist_append(nullptr,key.c_str());
    curl_easy_setopt(curl,CURLOPT_URL,url.c_str());curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(curl,CURLOPT_TIMEOUT,timeout);curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&data);
    const auto ca=c.value("caFile",std::string{});if(!ca.empty())curl_easy_setopt(curl,CURLOPT_CAINFO,ca.c_str());
    long status=0;auto result=curl_easy_perform(curl);curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);curl_slist_free_all(headers);curl_easy_cleanup(curl);
    if(result!=CURLE_OK)throw std::runtime_error("Prowlarr HTTPS request failed. Check connection and certificate configuration.");
    if(status!=200)throw std::runtime_error("Prowlarr rejected the request. Check its connection, API key and indexer status.");
    return data.data;
  }
public:
  static bool ps5Title(const std::string& text){
    for(size_t i=0;i+3<=text.size();++i)if((text[i]=='p'||text[i]=='P')&&(text[i+1]=='s'||text[i+1]=='S')&&text[i+2]=='5'&&(i==0||!std::isalnum(static_cast<unsigned char>(text[i-1])))&&(i+3==text.size()||!std::isalnum(static_cast<unsigned char>(text[i+3]))))return true;
    return false;
  }
  static std::string gameKey(const std::string& text){
    std::string key,word;auto flush=[&](){if(word.empty())return false;if(word=="ps5")return true;if(!key.empty())key+=' ';key+=word;word.clear();return false;};
    for(unsigned char c:text){if(std::isalnum(c)||c>=128)word+=static_cast<char>(std::tolower(c));else if(flush())return key;}
    flush();return key;
  }
  json state(const std::set<std::string>& owned={}){
    std::lock_guard<std::mutex> g(mutex);json safe=json::array();
    for(auto row:rows){const auto key=gameKey(row.value("name",""));if(!order.empty()&&(row.value("added",false)||(!key.empty()&&owned.count(key))))continue;row.erase("download");safe.push_back(row);}
    return {{"results",safe},{"query",query},{"sort",order},{"busy",busy},{"adding",adding},{"error",error},{"notice",notice}};
  }
  void start(const Paths& paths,const std::string& text,const std::string& sort="",bool refresh=false){
    if(!sort.empty()&&sort!="seeders"&&sort!="completed"&&sort!="newest")throw std::runtime_error("Invalid Explore sort");
    if(text.empty()||text.size()>200||text.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Enter a search from 1 to 200 characters");
    auto settings=config(paths);int indexer=1;if(!sort.empty())indexer=settings.at("exploreIndexers").at(sort).get<int>();
    if(indexer<1||indexer>100000)throw std::runtime_error("Invalid Explore indexer");
    std::lock_guard<std::mutex> g(mutex);
    if(busy||adding)throw std::runtime_error("Wait for the current search or download request");
    busy=true;query=text;order=sort;rows=json::array();error.clear();notice.clear();
    const auto cache=paths.root/"cache"/("explore-"+sort+".json");
    const auto scope=settings.at("url").get<std::string>()+"#"+std::to_string(indexer);
    if(!sort.empty())try{
      auto saved=json::parse(readText(cache,2*1024*1024));
      if(saved.at("source")==scope&&saved.at("results").is_array()&&saved.at("results").size()<=100){
        rows=saved.at("results");const auto age=std::time(nullptr)-saved.at("saved").get<long long>();
        if(!refresh&&age>=0&&age<600){busy=false;notice="Cached results. Press Square to refresh.";return;}
        notice="Showing cached results while refreshing...";
      }
    }catch(...){}
    for(auto it=covers.begin();it!=covers.end();)if(it->second.empty())it=covers.erase(it);else ++it;

    try{std::thread([this,paths,text,sort,indexer,cache,scope]{
      try{
        std::string encoded;const char* hex="0123456789ABCDEF";for(unsigned char ch:text){if(std::isalnum(ch)||ch=='-'||ch=='_'||ch=='.')encoded+=ch;else{encoded+='%';encoded+=hex[ch>>4];encoded+=hex[ch&15];}}
        auto data=json::parse(fetch(paths,"/api/v1/search?query="+encoded+"&indexerIds="+std::to_string(indexer)+"&categories=1080&type=search&limit=100",2*1024*1024));
        if(!data.is_array())throw std::runtime_error("Invalid search response");json found=json::array();
        for(const auto& row:data){
          bool category=false;for(const auto& cat:row.value("categories",json::array()))if(cat.value("id",0)==1080)category=true;
          if(!category||row.value("indexerId",0)!=indexer||row.value("protocol","")!="torrent")continue;
          if(!sort.empty()&&!ps5Title(row.value("title","")))continue;
          const auto url=row.value("downloadUrl","");const auto scheme=url.find("://"),slash=scheme==std::string::npos?std::string::npos:url.find('/',scheme+3);
          if(slash==std::string::npos)continue;auto path=url.substr(slash);if(path.rfind("/"+std::to_string(indexer)+"/download?",0)!=0||path.find_first_of("\r\n")!=std::string::npos)continue;
          // Drop embedded credentials; the configured API key is sent as a header.
          const auto pos=path.find("apikey=");if(pos!=std::string::npos){const auto end=path.find('&',pos);path.erase(pos,end==std::string::npos?std::string::npos:end-pos+1);}
          found.push_back({{"id",randomId()},{"name",row.value("title","").substr(0,500)},{"size",row.value("size",0ULL)},{"seeders",row.value("seeders",0)},{"leechers",row.value("leechers",0)},{"completed",row.value("grabs",0)},{"published",row.value("publishDate","")},{"download",path},{"added",false}});
        }
        std::stable_sort(found.begin(),found.end(),[&](const json& a,const json& b){if(sort=="newest")return a.at("published").template get<std::string>()>b.at("published").template get<std::string>();const char* key=sort=="completed"?"completed":"seeders";return a.at(key).template get<int>()>b.at(key).template get<int>();});
        if(found.size()>100)found.erase(found.begin()+100,found.end());
        std::lock_guard<std::mutex> guard(mutex);
        for(auto& row:found)for(const auto& old:rows)if(row.at("download")==old.at("download")&&row.at("name")==old.at("name")){row["id"]=old.at("id");row["added"]=old.value("added",false);break;}
        rows=std::move(found);busy=false;notice.clear();
        if(!sort.empty())try{fs::create_directories(cache.parent_path());writeJson(cache,{{"source",scope},{"saved",std::time(nullptr)},{"results",rows}});}catch(...){}

      }catch(...){std::lock_guard<std::mutex> guard(mutex);error="Search failed. Check Prowlarr, its API key and the IPTorrents session.";busy=false;}
    }).detach();}catch(...){busy=false;throw;}
  }
  std::string artwork(const Paths& paths,const std::string& id){
    std::lock_guard<std::mutex> guard(mutex);std::string title;
    for(const auto& row:rows)if(row.at("id")==id)title=gameKey(row.value("name",""));
    if(title.empty())return {};if(title.size()>200)title.resize(200);
    unsigned long long hash=14695981039346656037ULL;for(unsigned char c:title){hash^=c;hash*=1099511628211ULL;}
    char key[17];std::snprintf(key,sizeof(key),"%016llx",hash);
    const auto folder=paths.root/"cache/covers-v4",file=folder/(std::string(key)+".rgb"),meta=folder/(std::string(key)+".json");
    const auto found=covers.find(title);if(found!=covers.end()){if(!found->second.empty()||std::time(nullptr)-coverFailedAt[title]<60)return found->second;covers.erase(found);coverFailedAt.erase(title);}
    if(coversLoading>=6)return "pending";
    if(covers.size()>=36){for(auto it=covers.begin();it!=covers.end();++it)if(it->second!="pending"){covers.erase(it);break;}}
    try{auto saved=json::parse(readText(meta,4096));if(saved.at("title")==title&&std::time(nullptr)-saved.at("saved").get<long long>()<2592000){auto data=readText(file,160*240*3);if(data.size()==160*240*3){covers[title]=data;return data;}}}catch(...){}
    covers[title]="pending";++coversLoading;
    try{std::thread([this,paths,title,folder,file,meta]{
      std::string data;try{std::string encoded;const char* hex="0123456789ABCDEF";for(unsigned char c:title){if(std::isalnum(c))encoded+=c;else{encoded+='%';encoded+=hex[c>>4];encoded+=hex[c&15];}}
        data=fetch(paths,"/artwork?title="+encoded,160*240*3,12);if(data.size()!=160*240*3)data.clear();
      }catch(...){}
      if(!data.empty())try{
        fs::create_directories(folder);fs::permissions(folder,fs::perms::owner_all);
        const auto temp=file.string()+".tmp";{std::ofstream out(temp,std::ios::binary);out.write(data.data(),data.size());if(!out)throw std::runtime_error("Cover cache write failed");}
        fs::rename(temp,file);writeJson(meta,{{"title",title},{"saved",std::time(nullptr)}});
        // Bound disk usage to roughly 15 MiB; concurrent writers only prune old entries.
        std::lock_guard<std::mutex> cacheGuard(mutex);std::vector<fs::path> files;
        for(const auto& item:fs::directory_iterator(folder))if(item.path().extension()==".rgb")files.push_back(item.path());
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return fs::last_write_time(a)<fs::last_write_time(b);});
        while(files.size()>128){auto oldest=files.front();fs::remove(oldest);oldest.replace_extension(".json");fs::remove(oldest);files.erase(files.begin());}
      }catch(...){}
      std::lock_guard<std::mutex> finished(mutex);if(data.empty())coverFailedAt[title]=std::time(nullptr);covers[title]=std::move(data);--coversLoading;
    }).detach();}catch(...){covers.erase(title);--coversLoading;return {};}
    return "pending";
  }
  template<class RPC> void add(const Paths& paths,const std::string& id,RPC rpc){
    std::lock_guard<std::mutex> g(mutex);if(busy||adding)throw std::runtime_error("Wait for the current request");
    std::string path;for(auto& row:rows)if(row.at("id")==id){if(row.value("added",false))return;path=row.at("download");}
    if(path.empty())throw std::runtime_error("Search result expired. Search again.");adding=true;error.clear();notice="Adding torrent to Transmission...";
    try{std::thread([this,paths,path,id,rpc]{
      try{
        auto bytes=fetch(paths,path,4*1024*1024);if(bytes.empty()||bytes.front()!='d'||bytes.back()!='e')throw std::runtime_error("Invalid torrent file");
        const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string base64;unsigned value=0,bits=0;
        for(unsigned char ch:bytes){value=(value<<8)|ch;bits+=8;while(bits>=6){bits-=6;base64+=alphabet[(value>>bits)&63];}}if(bits)base64+=alphabet[(value<<(6-bits))&63];while(base64.size()%4)base64+='=';
        rpc("torrent-add",json{{"metainfo",base64},{"download-dir",paths.complete.string()},{"paused",false}});
        std::lock_guard<std::mutex> guard(mutex);for(auto& row:rows)if(row.at("id")==id)row["added"]=true;adding=false;notice="Torrent added or already present. Open Torrents to follow progress.";
      }catch(...){std::lock_guard<std::mutex> guard(mutex);adding=false;notice.clear();error="Could not confirm the download. Check Torrents before retrying; verify Prowlarr and Transmission.";}
    }).detach();}catch(...){adding=false;throw;}
  }
};
}
