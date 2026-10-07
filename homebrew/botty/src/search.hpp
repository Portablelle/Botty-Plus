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
  json previousRows=json::array();
  std::map<std::string,std::string> covers;
  std::map<std::string,std::string> catalogTitles;
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
    for(auto row:rows){const auto key=gameKey(row.value("name",""));if(!order.empty()&&(row.value("added",false)||(!key.empty()&&owned.count(key))))continue;row.erase("download");for(auto& source:row["sources"])source.erase("download");safe.push_back(row);}
    return {{"results",safe},{"query",query},{"sort",order},{"busy",busy},{"adding",adding},{"error",error},{"notice",notice}};
  }
  void start(const Paths& paths,const std::string& text,const std::string& sort="",bool refresh=false){
    if(!sort.empty()&&sort!="seeders"&&sort!="completed"&&sort!="newest")throw std::runtime_error("Invalid Explore sort");
    if(text.empty()||text.size()>200||text.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Enter a search from 1 to 200 characters");
    std::lock_guard<std::mutex> g(mutex);
    if(busy||adding)throw std::runtime_error("Wait for the current search or download request");
    json settings;
    try{settings=config(paths);}catch(const std::exception&){
      // Search setup is optional. Report it in the tab state so an automatic
      // Explore request at startup never opens a blocking action error.
      query=text;order=sort;rows=json::array();previousRows=json::array();notice.clear();
      error="Search is unavailable. Check the Prowlarr configuration. Other tabs remain available.";
      return;
    }
    const bool sameSelection=!sort.empty()&&query==text&&order==sort;
    previousRows=sort.empty()?json::array():rows;
    busy=true;query=text;order=sort;if(!sameSelection)rows=json::array();error.clear();notice.clear();
    const auto cache=paths.root/"cache"/("explore-"+sort+".json");
    const auto scope=settings.at("url").get<std::string>()+"#all-torrents-console-sources-v3";
    if(!sort.empty())try{
      auto saved=json::parse(readText(cache,16*1024*1024));
      if(saved.at("source")==scope&&saved.at("results").is_array()&&saved.at("results").size()<=100){
        if(!sameSelection||rows.empty())rows=saved.at("results");const auto age=std::time(nullptr)-saved.at("saved").get<long long>();
        if(!refresh){busy=false;notice=age>=0&&age<600?"Cached results. Square: Refresh.":"Saved results may be outdated. Square: Refresh.";return;}
        notice="Refreshing results. You can still browse and select games.";
      }
    }catch(...){}
    for(auto it=covers.begin();it!=covers.end();)if(it->second.empty())it=covers.erase(it);else ++it;

    try{std::thread([this,paths,text,sort,cache,scope]{
      try{
        std::string encoded;const char* hex="0123456789ABCDEF";for(unsigned char ch:text){if(std::isalnum(ch)||ch=='-'||ch=='_'||ch=='.')encoded+=ch;else{encoded+='%';encoded+=hex[ch>>4];encoded+=hex[ch&15];}}
        // Prowlarr expands -2 to every enabled torrent indexer, including future providers.
        auto data=json::parse(fetch(paths,"/api/v1/search?query="+encoded+"&indexerIds=-2&categories=1000&type=search&limit=100",16*1024*1024));
        if(!data.is_array())throw std::runtime_error("Invalid search response");json found=json::array();std::map<std::string,size_t> releases;
        for(const auto& row:data){
          try{
            const int indexer=row.value("indexerId",0);bool category=false;
            for(const auto& cat:row.value("categories",json::array())){const int id=cat.value("id",0);if(id>=1000&&id<2000)category=true;}
            if(!category||indexer<1||row.value("protocol","")!="torrent")continue;
            const auto title=row.value("title",std::string{});if(title.empty()||(!sort.empty()&&!ps5Title(title)))continue;
            const auto url=row.value("downloadUrl",std::string{});const auto scheme=url.find("://"),slash=scheme==std::string::npos?std::string::npos:url.find('/',scheme+3);
            if(slash==std::string::npos)continue;auto path=url.substr(slash);if(path.rfind("/"+std::to_string(indexer)+"/download?",0)!=0||path.find_first_of("\r\n#")!=std::string::npos)continue;
            // Drop embedded credentials; downloads always use our configured origin and API header.
            const auto pos=path.find("apikey=");if(pos!=std::string::npos){const auto end=path.find('&',pos);path.erase(pos,end==std::string::npos?std::string::npos:end-pos+1);}
            auto count=[&](const char* key){const auto& value=row.at(key);return value.is_number_integer()?std::max(0LL,std::min(2147483647LL,value.get<long long>())):0LL;};
            auto metric=[&](const char* key){return row.contains(key)?count(key):0LL;};
            const auto size=row.value("size",0ULL);std::string releaseKey;bool space=false;
            for(unsigned char ch:title){if(std::isspace(ch)){space=!releaseKey.empty();continue;}if(space){releaseKey+=' ';space=false;}releaseKey+=static_cast<char>(std::tolower(ch));}
            releaseKey+=':'+std::to_string(size);
            const auto hash=row.contains("infoHash")&&row.at("infoHash").is_string()?row.at("infoHash").get<std::string>():std::string{};
            if((hash.size()==40||hash.size()==64)&&hash.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos){releaseKey="hash:"+hash;std::transform(releaseKey.begin(),releaseKey.end(),releaseKey.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});}
            json result={{"id",randomId()},{"name",title.substr(0,500)},{"size",size},{"seeders",metric("seeders")},{"leechers",metric("leechers")},{"completed",metric("grabs")},{"published",row.contains("publishDate")&&row.at("publishDate").is_string()?row.at("publishDate").get<std::string>():std::string{}},{"download",path},{"added",false}};
            if(!sort.empty())releaseKey="game:"+gameKey(title);
            result["tracker"]=row.value("indexer",std::string("Indexer ")+std::to_string(indexer));
            const auto duplicate=releases.find(releaseKey);
            if(duplicate==releases.end()){releases[releaseKey]=found.size();auto group=result;if(!sort.empty())group["id"]=randomId();group["sources"]=json::array({result});found.push_back(std::move(group));}
            else{
              auto& old=found[duplicate->second];auto& sources=old["sources"];bool repeated=false;
              for(auto& source:sources)if(source.at("download")==path){source["completed"]=std::max(source.at("completed").get<long long>(),result.at("completed").get<long long>());repeated=true;break;}
              if(!repeated)sources.push_back(result);
              const auto grabs=std::max(old.at("completed").get<long long>(),result.at("completed").get<long long>());
              const auto published=std::max(old.at("published").get<std::string>(),result.at("published").get<std::string>());
              // Keep the strongest source as the default for older clients; retain alternatives for the chooser.
              if(result.at("seeders")>old.at("seeders")){auto alternatives=std::move(sources);const auto id=old.at("id");old=result;if(!sort.empty())old["id"]=id;old["sources"]=std::move(alternatives);}
              old["completed"]=grabs;old["published"]=published;
            }
          }catch(const json::exception&){continue;} // One malformed provider row must not discard the other providers.
        }
        std::stable_sort(found.begin(),found.end(),[&](const json& a,const json& b){if(sort=="newest")return a.at("published").template get<std::string>()>b.at("published").template get<std::string>();const char* key=sort=="completed"?"completed":"seeders";return a.at(key).template get<int>()>b.at(key).template get<int>();});
        if(found.size()>100)found.erase(found.begin()+100,found.end());
        for(auto& row:found){auto& sources=row["sources"];std::stable_sort(sources.begin(),sources.end(),[](const json& a,const json& b){return a.at("seeders")>b.at("seeders");});if(sources.size()>32)sources.erase(sources.begin()+32,sources.end());}
        std::lock_guard<std::mutex> guard(mutex);
        for(auto& row:found)for(const auto& old:rows){
          if((!sort.empty()&&gameKey(row.at("name"))==gameKey(old.at("name")))||(row.at("download")==old.at("download")&&row.at("name")==old.at("name"))){row["id"]=old.at("id");row["added"]=old.value("added",false);}
          for(auto& source:row["sources"])for(const auto& prior:old.value("sources",json::array()))if(source.at("download")==prior.at("download")&&source.at("name")==prior.at("name")&&source.at("size")==prior.at("size"))source["id"]=prior.at("id");
        }
        previousRows=sort.empty()?json::array():rows;rows=std::move(found);busy=false;notice=sort.empty()?"":"Results updated. Square: Refresh.";
        if(!sort.empty())try{fs::create_directories(cache.parent_path());writeJson(cache,{{"source",scope},{"saved",std::time(nullptr)},{"results",rows}});}catch(...){}

      }catch(...){std::lock_guard<std::mutex> guard(mutex);error="Search failed. Check Prowlarr, its API key and the configured indexers.";busy=false;}
    }).detach();}catch(...){busy=false;throw;}
  }
  void registerCatalogArtwork(const json& torrents,const json& jobs){
    std::lock_guard<std::mutex> guard(mutex);catalogTitles.clear();
    unsigned count=0;
    for(const auto& row:torrents){if(count++==256)break;catalogTitles["t:"+std::to_string(row.at("id").get<int>())]=row.value("name","");}
    count=0;for(const auto& row:jobs){if(count++==256)break;catalogTitles["j:"+row.at("id").get<std::string>()]=row.value("name","");}
  }
  std::string artwork(const Paths& paths,const std::string& id,bool catalog=false){
    std::lock_guard<std::mutex> guard(mutex);std::string title;
    if(catalog){const auto found=catalogTitles.find(id);if(found!=catalogTitles.end())title=gameKey(found->second);}
    else for(const auto& row:rows)if(row.at("id")==id)title=gameKey(row.value("name",""));
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
    std::string path,groupId;
    for(auto* snapshot:{&rows,&previousRows}){
      for(const auto& row:*snapshot){
        for(const auto& source:row.value("sources",json::array()))if(source.at("id")==id){
          if(row.value("added",false))return;
          path=source.at("download");groupId=row.at("id");break;
        }
        if(!path.empty())break;
      }
      if(!path.empty())break;
    }
    if(path.empty())for(auto* snapshot:{&rows,&previousRows}){
      for(const auto& row:*snapshot)if(row.at("id")==id){
        if(row.value("added",false))return;
        path=row.at("download");groupId=row.at("id");break;
      }
      if(!path.empty())break;
    }
    for(const auto& row:rows)if(row.at("id")==groupId&&row.value("added",false))return;
    if(path.empty())throw std::runtime_error("Search result expired. Search again.");adding=true;error.clear();notice="Adding torrent...";
    try{std::thread([this,paths,path,groupId,rpc]{
      try{
        auto bytes=fetch(paths,path,4*1024*1024);if(bytes.empty()||bytes.front()!='d'||bytes.back()!='e')throw std::runtime_error("Invalid torrent file");
        const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string base64;unsigned value=0,bits=0;
        for(unsigned char ch:bytes){value=(value<<8)|ch;bits+=8;while(bits>=6){bits-=6;base64+=alphabet[(value>>bits)&63];}}if(bits)base64+=alphabet[(value<<(6-bits))&63];while(base64.size()%4)base64+='=';
        rpc("torrent-add",json{{"metainfo",base64},{"download-dir",paths.complete.string()},{"paused",false}});
        std::lock_guard<std::mutex> guard(mutex);for(auto* snapshot:{&rows,&previousRows})for(auto& row:*snapshot)if(row.at("id")==groupId)row["added"]=true;adding=false;notice="Torrent added or already present. Open Torrents to follow progress.";
      }catch(...){std::lock_guard<std::mutex> guard(mutex);adding=false;notice.clear();error="Could not confirm the download. Check Torrents before retrying; verify Prowlarr and the torrent service.";}
    }).detach();}catch(...){adding=false;throw;}
  }
};
}
