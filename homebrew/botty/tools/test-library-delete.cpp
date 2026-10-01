// PS5 regression: compile with -D__PS5__ -O2 -pthread, src/core.cpp and -lkernel_sys.
// Send through the ELF loader. Uses only its isolated synthetic fixture directory.
#include "core.hpp"
#include <cstdio>
#include <fstream>
#include <thread>

int main() {
  using namespace botty;
  FILE* log=fopen("/data/botty/manager/library-delete-regression.log","w");
  if(!log)return 1;
  setvbuf(log,nullptr,_IONBF,0);
  int result=1;
  std::thread worker([&]{
    try {
      Paths paths("/data/botty/library-delete-regression", "/data/botty/library-delete-regression/library");
      fs::create_directories(paths.library);fs::create_directories(paths.jobs);
      const auto target=paths.library/"PPSA00001-app";
      if(fs::exists(target))throw std::runtime_error("Existing fixture; refusing to overwrite");
      std::ofstream(paths.library/"preserve.dat")<<"preserve";
      for(int round=0;round<8;++round) {
        fs::path nested=target;
        for(int depth=0;depth<16;++depth) {
          nested/="nested";fs::create_directories(nested);
          for(int file=0;file<16;++file)std::ofstream(nested/(std::to_string(file)+".dat"))<<"Synthetic fixture";
        }
        const auto id=randomId();
        json job={{"id",id},{"status","moved"},{"destination",target.string()},
          {"content",{{"kind","folder"},{"titleId","PPSA00001"},{"destination","PPSA00001-app"}}}};
        writeJson(paths.jobs/(id+".json"),job);
        deleteLibraryGame(paths,job);
        downloadedFiles(paths.jobs,{id+".json"},true);
        if(fs::exists(target)||fs::exists(paths.jobs/(id+".json"))||readText(paths.library/"preserve.dat")!="preserve")
          throw std::runtime_error("Fixture verification failed");
        fprintf(log,"Round %d passed\n",round+1);
      }
      fprintf(log,"PASS: 2048 files, 128 nested directories; unrelated file preserved\n");result=0;
    }catch(const std::exception& error){fprintf(log,"FAIL: %s\n",error.what());}
  });
  worker.join();fclose(log);return result;
}
