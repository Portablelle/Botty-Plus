#pragma once
#include "core.hpp"
#include "rartypes.hpp"
#include "sha1.hpp"
#include "progress.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <map>
#include <regex>
#include <set>
#include <thread>

namespace botty {
// Keep Botty's torrent model stable while translating to rTorrent's local SCGI API.
class Rtorrent {
  std::mutex mutex_;
  Paths paths_;
  int port_;
  bool policyApplied_=false;
  struct DownloadEstimate {ExtractionEstimate progress;ExtractionEstimate::Clock::time_point updated{};};
  std::map<std::string,DownloadEstimate> estimates_;
  struct Socket { int fd; ~Socket(){if(fd>=0)::close(fd);} };
  json call(const std::string& method,const json& params=json::array({""})) {
    Socket socket{::socket(AF_INET,SOCK_STREAM,0)};
    if(socket.fd<0)throw std::runtime_error("Cannot open rTorrent connection");
    timeval timeout{10,0};
    setsockopt(socket.fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(socket.fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port_);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(::connect(socket.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))throw std::runtime_error("rTorrent is unavailable. Start it from the Botty portal.");
    const auto body=json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump();
    std::string header;
    for(const auto& part:std::vector<std::string>{"CONTENT_LENGTH",std::to_string(body.size()),"SCGI","1","CONTENT_TYPE","application/json"}){header+=part;header+='\0';}
    const auto request=std::to_string(header.size())+":"+header+","+body;
    size_t sent=0;
    while(sent<request.size()) {auto n=::send(socket.fd,request.data()+sent,request.size()-sent,0);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("rTorrent write failed");sent+=n;}
    std::string response;char buffer[16384];
    for(;;){auto n=::recv(socket.fd,buffer,sizeof(buffer),0);if(n<0&&errno==EINTR)continue;if(n<0)throw std::runtime_error("rTorrent response timed out");if(!n)break;response.append(buffer,n);if(response.size()>16*1024*1024)throw std::runtime_error("rTorrent response too large");}
    auto offset=response.find("\r\n\r\n");size_t skip=4;
    if(offset==std::string::npos){offset=response.find("\n\n");skip=2;}
    if(offset==std::string::npos)throw std::runtime_error("Invalid rTorrent response");
    auto result=json::parse(response.substr(offset+skip));
    if(result.contains("error"))throw std::runtime_error("rTorrent: "+result["error"].value("message","RPC failed"));
    return result.at("result");
  }
  static std::string lower(std::string value){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return std::tolower(c);});return value;}
  int identity(const std::string& hash) {
    const auto path=paths_.root/"rtorrent/state/botty-ids.json";
    auto ids=fs::exists(path)?json::parse(readText(path)):json::object();
    if(!ids.is_object())throw std::runtime_error("Invalid torrent ID record");
    int next=1;std::set<int> seen;
    for(auto it=ids.begin();it!=ids.end();++it){int id=it.value().get<int>();if(id<=0||id>=2147483646||!seen.insert(id).second)throw std::runtime_error("Invalid torrent ID record");next=std::max(next,id+1);}
    if(ids.contains(hash))return ids.at(hash).get<int>();
    ids[hash]=next;writeJson(path,ids);return next;
  }
  json list() {
    auto rows=call("d.multicall",{"","main","d.hash=","d.name=","d.is_active=","d.complete=","d.left_bytes=","d.size_bytes=","d.down.rate=","d.up.rate=","d.directory=","d.is_multi_file=","d.hashing=","d.is_hash_checked=","d.message=","d.peers_connected="});
    json result=json::array();
    const auto now=ExtractionEstimate::Clock::now();std::set<std::string> present;
    for(const auto& row:rows) {
      const auto hash=lower(row[0].get<std::string>());const auto name=row[1].get<std::string>();
      bool complete=row[3].get<int>()&&row[11].get<int>()&&!row[10].get<int>();
      uint64_t left=row[4],total=row[5],rate=row[6];
      int status=row[10].get<int>()?2:(row[2].get<int>()?(complete?6:4):0);
      present.insert(hash);double eta=-1;
      if(status==4&&left<=total){
        auto& estimate=estimates_[hash];
        if(now-estimate.updated>std::chrono::seconds(30))estimate.progress=ExtractionEstimate{};
        estimate.progress.update(now,total-left,total);estimate.updated=now;eta=estimate.progress.eta;
      }else estimates_.erase(hash);
      fs::path directory=row[8].get<std::string>();bool multi=row[9].get<int>()!=0;
      const auto base=multi?directory.parent_path():directory;
      json files=json::array();
      for(const auto& file:call("f.multicall",{hash,"","f.path=","f.size_bytes=","f.completed_chunks=","f.size_chunks="})) {
        fs::path relative=safeRelative(file[0].get<std::string>());if(multi)relative=directory.filename()/relative;
        uint64_t size=file[1],chunks=file[3],done=file[2];
        files.push_back({{"name",relative.generic_string()},{"length",size},{"bytesCompleted",complete?size:(chunks?std::min(size,uint64_t((long double)size*done/chunks)):uint64_t(0))}});
      }
      int receiving=0,sending=0;
      for(const auto& peer:call("p.multicall",{hash,"","p.down_rate=","p.up_rate="})){receiving+=peer[0].get<uint64_t>()>0;sending+=peer[1].get<uint64_t>()>0;}
      const auto error=row[12].get<std::string>();
      result.push_back({{"id",identity(hash)},{"hashString",hash},{"name",name},{"status",status},{"percentDone",total?double(total-left)/total:0.0},{"leftUntilDone",left},{"totalSize",total},{"sizeWhenDone",total},{"eta",eta},{"downloadDir",base.string()},{"rateDownload",rate},{"rateUpload",row[7]},{"error",error.empty()?0:1},{"errorString",error},{"files",files},{"peersConnected",row[13]},{"peersSendingToUs",receiving},{"peersGettingFromUs",sending}});
    }
    for(auto it=estimates_.begin();it!=estimates_.end();)if(!present.count(it->first))it=estimates_.erase(it);else ++it;
    return result;
  }
  // Parse bounded bencode, keeping the exact info slice used for the v1 hash.
  static json bdecode(const std::string& data,size_t& at,unsigned depth=0) {
    if(depth>64||at>=data.size())throw std::runtime_error("Invalid torrent metadata");
    char kind=data[at++];
    if(kind=='i'){auto end=data.find('e',at);if(end==std::string::npos)throw std::runtime_error("Invalid torrent integer");auto text=data.substr(at,end-at);if(!std::regex_match(text,std::regex("-?(0|[1-9][0-9]*)")))throw std::runtime_error("Invalid torrent integer");at=end+1;return std::stoll(text);}
    if(kind=='l'||kind=='d') {json out=kind=='l'?json::array():json::object();while(at<data.size()&&data[at]!='e'){if(kind=='l')out.push_back(bdecode(data,at,depth+1));else{auto key=bdecode(data,at,depth+1).get<std::string>();if(out.contains(key))throw std::runtime_error("Duplicate torrent key");out[key]=bdecode(data,at,depth+1);}}if(at>=data.size())throw std::runtime_error("Truncated torrent");++at;return out;}
    --at;auto colon=data.find(':',at);if(colon==std::string::npos||colon-at>10)throw std::runtime_error("Invalid torrent string");auto digits=data.substr(at,colon-at);if(!std::regex_match(digits,std::regex("0|[1-9][0-9]*")))throw std::runtime_error("Invalid torrent length");size_t length=std::stoull(digits);at=colon+1;if(length>data.size()-at)throw std::runtime_error("Truncated torrent string");auto value=data.substr(at,length);at+=length;return value;
  }
  static std::string metadataHash(const std::string& data) {
    if(data.empty()||data[0]!='d')throw std::runtime_error("Invalid torrent");size_t at=1,start=0,end=0;
    while(at<data.size()&&data[at]!='e'){auto key=bdecode(data,at).get<std::string>();size_t begin=at;auto value=bdecode(data,at);if(key=="info"){if(start)throw std::runtime_error("Duplicate info");start=begin;end=at;auto name=value.at("name").get<std::string>();if(safeRelative(name).has_parent_path())throw std::runtime_error("Invalid torrent name");if(value.contains("files"))for(const auto& f:value.at("files")){fs::path member;for(const auto& part:f.at("path")){auto p=safeRelative(part.get<std::string>());if(p.has_parent_path())throw std::runtime_error("Invalid torrent component");member/=p;}safeRelative(member.string());}}}
    if(!start||at+1!=data.size()||data[at]!='e')throw std::runtime_error("Invalid torrent metadata");
    sha1_context context;uint32 digest[5];sha1_init(&context);sha1_process(&context,reinterpret_cast<const byte*>(data.data()+start),end-start);sha1_done(&context,digest);
    char hash[41];for(int i=0;i<5;++i)snprintf(hash+8*i,9,"%08x",digest[i]);return hash;
  }
  static std::string decode64(const std::string& text) {
    const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string out;unsigned value=0,bits=0;bool padding=false;
    for(unsigned char c:text){if(c=='='){padding=true;continue;}auto digit=alphabet.find(c);if(digit==std::string::npos||padding)throw std::runtime_error("Invalid torrent encoding");value=(value<<6)|digit;bits+=6;if(bits>=8){bits-=8;out+=char((value>>bits)&255);}}
    if(out.empty()||out.size()>2*1024*1024)throw std::runtime_error("Invalid torrent size");return out;
  }
public:
  Rtorrent(const Paths& paths,int port):paths_(paths),port_(port) {fs::create_directories(paths_.root/"rtorrent/state/incoming");}
  json request(const std::string& method,const json& args) {
    std::lock_guard<std::mutex> guard(mutex_);
    if(!policyApplied_){call("pieces.hash.on_completion.set",{"",0});policyApplied_=true;}
    if(method=="session-get"){auto version=call("system.client_version");return {{"version",version},{"incomplete-dir-enabled",false},{"download-dir",paths_.complete.string()}};}
    if(method=="torrent-get")return {{"torrents",list()}};
    if(method=="torrent-add") {
      std::string hash,source;
      if(args.contains("metainfo")) {
        auto data=decode64(args.at("metainfo"));hash=metadataHash(data);auto path=paths_.root/"rtorrent/state/incoming"/(hash+".torrent");
        std::ofstream output(path,std::ios::binary|std::ios::trunc);output.write(data.data(),data.size());output.close();if(!output)throw std::runtime_error("Cannot save torrent metadata");source=path.string();
      } else {
        source=args.at("filename").get<std::string>();std::smatch match;
        if(source.size()>16384||!std::regex_search(source,match,std::regex("^magnet:\\?.*xt=urn:btih:([a-fA-F0-9]{40})(?:&|$)")))throw std::runtime_error("A v1 magnet with a hexadecimal info hash is required");hash=lower(match[1]);
      }
      bool duplicate=false;for(const auto& item:list())if(item.at("hashString")==hash)duplicate=true;
      if(!duplicate){
        const auto directory=args.value("download-dir",paths_.complete.string());
        // An RPC command string is parsed by rTorrent: accept only managed paths.
        if(directory.find_first_of("\"\\\n\r;,$")!=std::string::npos||!fs::is_directory(directory))throw std::runtime_error("Invalid download directory");
        call(args.value("paused",false)?"load.normal":"load.start",{"",source,"d.directory.set=\""+directory+"\""});call("session.save");
      }
      return {{duplicate?"torrent-duplicate":"torrent-added",{{"id",identity(hash)},{"hashString",hash}}}};
    }
    const auto items=list();
    for(const auto& id:args.at("ids")) {
      std::string hash;
      for(const auto& item:items)if((id.is_number_integer()&&item.at("id")==id)||(id.is_string()&&item.at("hashString")==lower(id.get<std::string>())))hash=item.at("hashString");
      if(hash.empty())throw std::runtime_error("Torrent no longer exists");
      if(method=="torrent-stop")call("d.stop",{hash});
      else if(method=="torrent-start"||method=="torrent-start-now")call("d.start",{hash});
      else if(method=="torrent-verify"){call("d.stop",{hash});call("d.check_hash",{hash});}
      else if(method=="torrent-close"){call("d.stop",{hash});call("d.close",{hash});}
      else if(method=="torrent-set-location"){
        const auto directory=args.at("location").get<std::string>();
        if(!fs::is_directory(directory))throw std::runtime_error("Destination is unavailable");
        call("d.stop",{hash});call("d.close",{hash});call("d.directory.set",{hash,directory});
      }
      else if(method=="torrent-remove"){if(args.value("delete-local-data",false))throw std::runtime_error("Delete exact files through Botty first");call("d.erase",{hash});}
      else throw std::runtime_error("Unsupported torrent operation");
      estimates_.erase(hash);
    }
    call("session.save");return json::object();
  }
};
}
