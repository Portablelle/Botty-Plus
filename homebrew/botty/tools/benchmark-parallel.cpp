// Standalone, bounded console benchmark. Samples are intentionally partial;
// CRC correctness is tested separately with complete original fixtures.
#include "../src/rar.cpp"
#include <chrono>
#include <cstdio>
#include <fstream>
namespace botty {
uint64_t sample(const fs::path& archive,const fs::path& output,const std::string& name,uint64_t limit){
 Context c;c.parent=archive.parent_path();bool enough=false;
 c.report=[&](const Progress&p){if(p.bytes>=limit){enough=true;throw std::runtime_error("Benchmark sample complete");}};
 Archive a(archive,RAR_OM_EXTRACT,c);
 for(;;){RARHeaderDataEx h{};int rc=RARReadHeaderEx(a.handle,&h);if(rc==ERAR_END_ARCHIVE)throw std::runtime_error("Benchmark member missing");checkCode(rc,c);checkHeader(h);
  if(nameOf(h)!=name||(h.Flags&RHDF_SPLITBEFORE)){checkCode(RARProcessFile(a.handle,RAR_SKIP,nullptr,nullptr),c);continue;}
  c.expected=(uint64_t(h.UnpSizeHigh)<<32)|h.UnpSize;c.fd=open(output.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(c.fd<0)throw std::runtime_error("Cannot create benchmark output");
  rc=RARProcessFile(a.handle,RAR_TEST,nullptr,nullptr);int flush=fsync(c.fd);close(c.fd);c.fd=-1;
  if(!enough||flush)throw std::runtime_error("Benchmark failed before sample completed: "+std::to_string(rc));return c.written;
 }
}
uint64_t digest(const fs::path& p){std::ifstream f(p,std::ios::binary);char b[65536];uint64_t hash=14695981039346656037ULL;while(f){f.read(b,sizeof(b));for(std::streamsize i=0;i<f.gcount();++i){hash^=static_cast<unsigned char>(b[i]);hash*=1099511628211ULL;}}return hash;}
}
int main(){using namespace botty;FILE*f=fopen("/data/botty/manager/parallel-benchmark.jsonl","w");if(!f)return 1;
 try{
  std::ifstream config("/data/botty/manager/parallel-benchmark-request.json");json request;config>>request;
  const fs::path archive=request.at("archive").get<std::string>();auto names=request.at("files").get<std::vector<std::string>>();if(names.size()!=2)throw std::runtime_error("Need two members");
  const fs::path root="/data/botty/manager/parallel-benchmark-20261001";if(!fs::create_directory(root))throw std::runtime_error("Benchmark directory already exists");
  uint64_t expected[2]={0,0};
  for(unsigned workers:{1U,2U,2U,1U}){
   std::exception_ptr failure;std::mutex mutex;uint64_t bytes[2]={0,0};auto start=std::chrono::steady_clock::now();
   auto run=[&](unsigned n){try{bytes[n]=sample(archive,root/std::to_string(n),names[n],128ULL*1024*1024);}catch(...){std::lock_guard<std::mutex>g(mutex);if(!failure)failure=std::current_exception();}};
   if(workers==2){std::thread t([&]{run(1);});run(0);t.join();}else{run(0);run(1);}
   if(failure)std::rethrow_exception(failure);
   double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
   for(unsigned i=0;i<2;++i){auto h=digest(root/std::to_string(i));if(expected[i]&&expected[i]!=h)throw std::runtime_error("Sample mismatch");expected[i]=h;fs::remove(root/std::to_string(i));}
   json result={{"workers",workers},{"seconds",seconds},{"bytes",bytes[0]+bytes[1]},{"MBps",double(bytes[0]+bytes[1])/seconds/1000000},{"sampleMatch",true}};fprintf(f,"%s\n",result.dump().c_str());fflush(f);
  }
  fs::remove(root);fprintf(f,"{\"complete\":true}\n");
 }catch(const std::exception&e){json error={{"error",e.what()}};fprintf(f,"%s\n",error.dump().c_str());fclose(f);return 1;}
 fclose(f);return 0;
}
