#include "rest-companion.hpp"
#include <cassert>
#include <cstdio>
#include <unistd.h>
using namespace botty;
int main(){
  char directory[]="/private/tmp/botty-rest-status-XXXXXX";
#ifdef __linux__
  std::strcpy(directory,"/tmp/botty-rest-status-XXXXXX");
#endif
  assert(mkdtemp(directory));const fs::path root=directory;
  const json boot={{"seconds",1000},{"microseconds",1}};
  assert(!restCompanionStatus(root,100,boot).at("live"));
  writeJson(root/"rest-mode-companion.json",{{"schema",1},{"firmware",BOTTY_REST_FW},{"boot",boot},{"monotonicSeconds",100},{"status","active"},{"ownerPid",42}});
  assert(restCompanionStatus(root,120,boot).at("live"));
  assert(!restCompanionStatus(root,121,boot).at("live"));
  assert(!restCompanionStatus(root,99,boot).at("live"));
  assert(!restCompanionStatus(root,100,{{"seconds",1001},{"microseconds",1}}).at("live"));
  writeJson(root/"rest-mode-companion.json",{{"schema",2}});
  assert(!restCompanionStatus(root,100,boot).at("live"));
  fs::remove_all(root);puts("Companion missing, stale, future, previous-boot and malformed status passed");
}
