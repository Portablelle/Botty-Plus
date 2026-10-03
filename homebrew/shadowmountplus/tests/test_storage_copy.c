#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_path_utils.h"
bool should_stop_requested(void) { return false; }
bool runtime_sleep_mode_active(void) { return false; }
bool path_matches_root_or_child(const char *path, const char *root) {
  size_t n = strlen(root); return strncmp(path, root, n) == 0 && (!path[n] || path[n] == '/');
}
static int cross_device_rename(const char *from, const char *to) {
  (void)from; (void)to; errno = EXDEV; return -1;
}
#define rename cross_device_rename
#include "../build/src/sm_storage.c"
#undef rename
static bool cancel_requested;
static const char *mutate_source;
static bool cancelled(void *ctx) { (void)ctx; return cancel_requested; }
static void progress(uint64_t bytes, uint64_t files, void *ctx) {
  (void)files;
  if (bytes && ctx) cancel_requested = true;
  if (bytes && mutate_source) {
    int fd = open(mutate_source, O_WRONLY | O_APPEND);assert(fd >= 0);
    assert(write(fd, "x", 1) == 1);close(fd);mutate_source = NULL;
  }
}
static void fixture(const char *path) {
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);assert(fd >= 0);
  assert(write(fd, "original", 8) == 8);assert(close(fd) == 0);
}
int main(void) {
  char root[] = "/tmp/botty-storage-XXXXXX";assert(mkdtemp(root));
  char from[256], to[256], alias[256];
  snprintf(from, sizeof(from), "%s/source", root);snprintf(to, sizeof(to), "%s/destination", root);snprintf(alias, sizeof(alias), "%s/alias", root);
  fixture(from);
  assert(sm_storage_move_path_progress(from,to,progress,cancelled,NULL,(void*)1,NULL) != 0);
  assert(access(from,F_OK) == 0 && access(to,F_OK) != 0);
  cancel_requested = false;mutate_source = from;
  assert(sm_storage_move_path_progress(from,to,progress,cancelled,NULL,NULL,NULL) != 0);
  assert(access(from,F_OK) == 0 && access(to,F_OK) != 0);
  assert(symlink(from,alias) == 0);assert(sm_storage_copy_path(alias,to) != 0);
  assert(access(from,F_OK) == 0 && access(to,F_OK) != 0);unlink(alias);
  fixture(to);assert(sm_storage_move_path(from,to) != 0);assert(access(from,F_OK) == 0);unlink(to);
  assert(sm_storage_move_path(from,to) == 0);assert(access(from,F_OK) != 0 && access(to,F_OK) == 0);
  assert(sm_storage_delete_path(to) == 0);assert(rmdir(root) == 0);
  puts("Production storage: cancellation, changing source, symlinks, no overwrite, cross-volume move and deletion passed");
  return 0;
}
