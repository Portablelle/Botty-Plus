// Host-only integration adapter. Executes the real native request implementation.
#include "probe.hpp"
#include "home_launch.hpp"
#include "platform.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
namespace botty::platform {
unsigned port=0;
std::uint64_t now() noexcept{return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void sleep(unsigned n) noexcept{std::this_thread::sleep_for(std::chrono::microseconds(n));}
void log(const char*) noexcept{}
int connectLocal() noexcept {
 int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
 timeval timeout{2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
 if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))){close(fd);return -1;}return fd;
}
// The service enforces its configured loopback Host. Replace only that header
// in this host adapter; the native client still writes the real PS5 port 8088.
int send(int fd,const void* data,std::size_t n) noexcept {
 std::string bytes(static_cast<const char*>(data),n);auto at=bytes.find("Host: 127.0.0.1:8088\r\n");
 if(at!=std::string::npos)bytes.replace(at,22,"Host: 127.0.0.1:"+std::to_string(port)+"\r\n");
 std::size_t sent=0;while(sent<bytes.size()){auto amount=::send(fd,bytes.data()+sent,bytes.size()-sent,0);if(amount<=0)return -1;sent+=amount;}return static_cast<int>(n);
}
int receive(int fd,void* data,std::size_t n) noexcept{return static_cast<int>(recv(fd,data,n,0));}
void closeSocket(int fd) noexcept{close(fd);}
bool startWorker(void* (*)(void*),void*,void**) noexcept{return false;}
void joinWorker(void*) noexcept{}
}
int main(int argc,char** argv){
 if(argc<3)return 2;botty::platform::port=static_cast<unsigned>(std::atoi(argv[1]));
 if(std::string_view(argv[2])=="--probe") {
  static botty::Catalog catalog;
  const auto connection=botty::probeConnection(&catalog);
  std::puts(botty::probeText(connection.status));
  return connection.status==botty::Probe::ready&&catalog.valid?0:1;
 }
 botty::Command cmd;
 // Stable host adapter names do not depend on the production enum's ordering.
 struct NamedOperation {std::string_view name;botty::Operation operation;};
 const NamedOperation operations[]={
  {"home",botty::Operation::emulationHome},{"launch",botty::Operation::emulationLaunch},
  {"pause",botty::Operation::pause},{"resume",botty::Operation::resume},
  {"verify",botty::Operation::verify},{"add",botty::Operation::add},
  {"extract",botty::Operation::extract},{"move",botty::Operation::move},
  {"remove",botty::Operation::remove},{"remove-library",botty::Operation::removeLibrary},
  {"compress",botty::Operation::compress},{"cancel-compression",botty::Operation::cancelCompression}
 };
 for(const auto& operation:operations)if(operation.name==argv[2])cmd.operation=operation.operation;
 if(std::string_view(argv[2])=="--from-home"&&botty::parseHomeLaunch(argc-3,argv+3,cmd)!=botty::HomeLaunch::valid)return 2;
 if(cmd.operation==botty::Operation::none){std::puts("Unknown integration action. Nothing was sent.");return 2;}
 if(argc>3&&std::string_view(argv[2])!="--from-home")std::snprintf(cmd.id.data(),cmd.id.size(),"%s",argv[3]);
 if(argc>4)std::snprintf(cmd.archive.data(),cmd.archive.size(),"%s",argv[4]);
 if(argc>5)std::snprintf(cmd.text.data(),cmd.text.size(),"%s",argv[5]);
 const auto result=botty::performCommand(cmd);std::puts(result.message.data());return result.status==botty::ActionResult::Status::success?0:1;
}
