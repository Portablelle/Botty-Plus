#pragma once
#include "service-updater.hpp"
#include "rtorrent.hpp"
#include "native-process.hpp"
#include <csignal>
#include <sys/file.h>
#ifdef __PS5__
#include <sys/sysctl.h>
#endif

namespace botty {
inline json installationBoot() {
#ifdef __PS5__
  timeval boot{};size_t size=sizeof(boot);
  if(sysctlbyname("kern.boottime",&boot,&size,nullptr,0)||size!=sizeof(boot)||boot.tv_sec<=0||boot.tv_usec<0||boot.tv_usec>999999)throw std::runtime_error("Cannot establish the current console boot identity.");
  return {{"seconds",boot.tv_sec},{"microseconds",boot.tv_usec}};
#else
  throw std::runtime_error("Production installation identity requires PS5 runtime validation.");
#endif
}
inline std::string installationBootId() {
  const auto boot=installationBoot();
  return std::to_string(boot.at("seconds").get<int64_t>())+":"+std::to_string(boot.at("microseconds").get<int64_t>());
}
inline json installationHttp(int port,const std::string& path,const json* body=nullptr,const std::string& capability="") {
  if((port!=8088&&port!=5910)||path.empty()||path.front()!='/'||path.size()>512)throw std::runtime_error("Unexpected local installation endpoint.");
  CURL* raw=curl_easy_init();if(!raw)throw std::runtime_error("Cannot initialize installation transport.");
  std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> client(raw,curl_easy_cleanup);
  const auto url="http://127.0.0.1:"+std::to_string(port)+path;std::string response,payload=body?body->dump():"";
  curl_easy_setopt(raw,CURLOPT_URL,url.c_str());curl_easy_setopt(raw,CURLOPT_PROXY,"");curl_easy_setopt(raw,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(raw,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(raw,CURLOPT_CONNECTTIMEOUT,2L);curl_easy_setopt(raw,CURLOPT_TIMEOUT,5L);
  curl_slist* headers=nullptr;
  if(body){headers=curl_slist_append(headers,"Content-Type: application/json");curl_easy_setopt(raw,CURLOPT_POSTFIELDS,payload.c_str());curl_easy_setopt(raw,CURLOPT_POSTFIELDSIZE,long(payload.size()));}
  if(!capability.empty())headers=curl_slist_append(headers,("X-Botty-Update-Token: "+capability).c_str());
  std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> owned(headers,curl_slist_free_all);curl_easy_setopt(raw,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(raw,CURLOPT_WRITEDATA,&response);
  curl_easy_setopt(raw,CURLOPT_WRITEFUNCTION,+[](char* bytes,size_t n,size_t m,void* opaque)->size_t {
    auto& output=*static_cast<std::string*>(opaque);if(n&&m>SIZE_MAX/n)return 0;const auto size=n*m;if(size>1024*1024-output.size())return 0;output.append(bytes,size);return size;
  });
  const auto result=curl_easy_perform(raw);long status=0;curl_easy_getinfo(raw,CURLINFO_RESPONSE_CODE,&status);
  if(result!=CURLE_OK||status!=200)throw std::runtime_error("Local service response was not confirmed. Automatic command repetition refused.");
  return json::parse(response);
}
inline std::string installationWorkerToken(const fs::path& root) {
  auto token=nativeRead(root/"compressor/token",128);while(!token.empty()&&(token.back()=='\n'||token.back()=='\r'))token.pop_back();
  if(!std::regex_match(token,std::regex("[a-fA-F0-9]{64}")))throw std::runtime_error("Worker authentication is unavailable.");return token;
}
inline json inspectInstallation(const fs::path& root,Rtorrent& engine) {
  try {
    const auto boot=installationBoot(),manager=installationHttp(8088,"/api/installation/status"),worker=installationHttp(5910,"/api/status?token="+installationWorkerToken(root)),runtime=json::parse(nativeRead(root/"rtorrent/state/runtime.json",16384));
    const auto record=json::parse(nativeRead(root/"manager-process.json",16384));
    if(manager.at("apiVersion")!=1||manager.at("updaterVersion")!="1.0.0"||record.at("pid")!=manager.at("pid")||record.at("version")!=manager.at("version")||worker.at("ok")!=true||runtime.at("schema")!=1||runtime.at("boot")!=boot)throw std::runtime_error("Unverified running service evidence.");
    const auto bootId=std::to_string(boot.at("seconds").get<int64_t>())+":"+std::to_string(boot.at("microseconds").get<int64_t>());
    json result={{"bootId",bootId},{"manager",{{"pid",manager.at("pid")},{"version",manager.at("version")}}},{"worker",{{"pid",worker.at("pid")},{"version",worker.at("version")},{"bottyWorker",worker.at("bottyWorker")}}},{"engine",{{"pid",runtime.at("pid")},{"rpcPid",engine.installationRpc("system.pid")},{"version",runtime.at("version")},{"bootId",bootId}}}};
    validateRunningInstallation(result);return result;
  }catch(const std::exception&) {throw std::runtime_error("Running service identity is unverified. Bootstrap modern manager, worker and rTorrent through Portal in an idle restarted session before using in-app updates.");}
}
inline bool installationPortClosed(int port) {
  const int fd=::socket(AF_INET,SOCK_STREAM,0);if(fd<0)throw std::runtime_error("Cannot inspect service port.");
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  const auto result=::connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)),error=errno;::close(fd);
  if(result==0)return false;if(error!=ECONNREFUSED)throw std::runtime_error("Service port exit evidence is unavailable.");return true;
}
inline bool installationExited(const std::string& service,int pid) {
  if(pid<=1||pid==getpid())throw std::runtime_error("Invalid service exit identity.");
  if(!::kill(pid,0)||errno!=ESRCH)return false;
  const int port=service=="manager"?8088:service=="worker"?5910:service=="engine"?5001:0;
  if(!port)throw std::runtime_error("Unexpected service exit query.");return installationPortClosed(port);
}
inline void installationLoad(const std::string& bytes) {
#ifdef __PS5__
  if(bytes.size()<4||bytes.size()>NativeTransaction::maxFileBytes||bytes.compare(0,4,"\177ELF"))throw std::runtime_error("Invalid updater ELF payload.");
  const int fd=::socket(AF_INET,SOCK_STREAM,0);if(fd<0)throw std::runtime_error("Cannot connect to the local ELF loader.");
  timeval timeout{5,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(9021);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(::connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))){::close(fd);throw std::runtime_error("Local ELF loader unavailable.");}
  size_t sent=0;while(sent<bytes.size()){const auto n=::send(fd,bytes.data()+sent,bytes.size()-sent,0);if(n<0&&errno==EINTR)continue;if(n<=0){::close(fd);throw std::runtime_error("ELF load outcome is unknown; automatic retry refused.");}sent+=size_t(n);}::shutdown(fd,SHUT_WR);::close(fd);
#else
  (void)bytes;throw std::runtime_error("Production loader calls are disabled in host builds.");
#endif
}
inline bool installationRetireAuthorized(const fs::path& root,const std::string& transaction,const std::string& capability) {
  if(!std::regex_match(transaction,std::regex("[a-f0-9]{32}"))||!std::regex_match(capability,std::regex("[a-f0-9]{64}")))return false;
  const auto handoff=installationRecord(root/"installation/handoff.json",256*1024);
  if(handoff.at("transaction")!=transaction||handoff.at("capability")!=capability||handoff.at("status")!="takeover"||handoff.at("running").at("manager").at("pid")!=getpid())return false;
  return installationHandoffOwned(root,handoff,installationBootId());
}
}
