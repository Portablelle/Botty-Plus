#include <assert.h>
#include "sm_botty_rebase.h"
int main(void) {
  char path[MAX_PATH] = "/mnt/shadowmnt/pfsc/game_old/inner.exfat";
  assert(sm_botty_rebase(path, "/mnt/shadowmnt/pfsc/game_old", "/mnt/shadowmnt/pfsc/game_new"));
  assert(strcmp(path,"/mnt/shadowmnt/pfsc/game_new/inner.exfat") == 0);
  strcpy(path,"/mnt/shadowmnt/inner_old/Game");
  assert(sm_botty_rebase(path,"/mnt/shadowmnt/inner_old","/mnt/shadowmnt/inner_new"));
  assert(strcmp(path,"/mnt/shadowmnt/inner_new/Game") == 0);
  strcpy(path,"/mnt/shadowmnt/inner_old_other/Game");
  assert(sm_botty_rebase(path,"/mnt/shadowmnt/inner_old","/new"));
  assert(strcmp(path,"/mnt/shadowmnt/inner_old_other/Game") == 0);
  char huge[MAX_PATH];memset(huge,'x',sizeof(huge)-1);huge[sizeof(huge)-1]=0;
  strcpy(path,"/old/child");assert(!sm_botty_rebase(path,"/old",huge));assert(strcmp(path,"/old/child") == 0);
  return 0;
}
