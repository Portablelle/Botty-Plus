#include "core.hpp"
#include <cstdio>
#include <thread>
#include <atomic>
using namespace botty;
int main(){FILE*f=fopen("/data/botty/manager/parallel-smoke.log","w");if(!f)return 1;
try{
 const fs::path fixtures="/data/botty/manager/unicode-smoke-20261001";
 const fs::path root="/data/botty/manager/parallel-smoke-20261001";if(!fs::create_directory(root))throw std::runtime_error("Test directory already exists");
 bool two=false;extractRar(fixtures/"app.rar",root/"good",[&](const Progress&p){if(p.phase.find("2 workers")!=std::string::npos)two=true;},"",{},2);
 if(!two||readText(root/"good/Demo/eboot.bin").size()!=7600)throw std::runtime_error("Parallel fixture mismatch");
 fprintf(f,"PS5 two-worker full extraction and CRC passed\n");fflush(f);
 bool badFailed=false;std::exception_ptr goodFailure;
 std::thread bad([&]{try{extractRar(fixtures/"bad-crc.rar",root/"bad",[](const Progress&){},"",{},2);}catch(...){badFailed=true;}});
 try{extractRar(fixtures/"app.rar",root/"isolated",[](const Progress&){},"",{},2);}catch(...){goodFailure=std::current_exception();}bad.join();
 if(goodFailure)std::rethrow_exception(goodFailure);if(!badFailed)throw std::runtime_error("CRC isolation failed");
 fprintf(f,"PS5 concurrent CRC error isolation passed\n");fflush(f);
 std::atomic<bool> stop{false};bool cancelled=false;
 try{extractRar(fixtures/"app.rar",root/"cancelled",[&](const Progress&p){if(p.bytes)stop=true;},"",[&]{return stop.load();},2);}catch(...){cancelled=true;}
 if(!cancelled||!fs::exists(fixtures/"app.rar"))throw std::runtime_error("Cancellation failed");
 fs::remove_all(root);fprintf(f,"PS5 parallel cancellation and source preservation passed\n");
}catch(const std::exception&e){fprintf(f,"FAILED: %s\n",e.what());fclose(f);return 1;}fclose(f);return 0;}
