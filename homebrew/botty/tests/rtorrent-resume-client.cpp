#include "rtorrent.hpp"
#include <iostream>

int main(int argc,char** argv) {
  if(argc!=4)return 2;
  try {
    botty::Rtorrent rpc(botty::Paths(argv[1]),std::stoi(argv[2]));
    rpc.request(argv[3],{{"ids",{1}}});
    std::cout<<rpc.request("torrent-get",botty::json::object()).dump()<<'\n';
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
