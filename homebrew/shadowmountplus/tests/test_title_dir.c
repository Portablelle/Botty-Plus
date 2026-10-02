// Compile the production hook implementation and assembly against isolated
// process-memory doubles. No console, installed game, or user data is touched.
#include <assert.h>
#include <stdarg.h>
#include <sys/socket.h>
#include "sm_shellcore_protocol_defs.h"
#undef SOL_SOCKET
#undef SO_SNDTIMEO
#undef SO_RCVTIMEO
#define SOL_SOCKET SM_SHELLCORE_BRIDGE_SOL_SOCKET
#define SO_SNDTIMEO SM_SHELLCORE_BRIDGE_SO_SNDTIMEO
#define SO_RCVTIMEO SM_SHELLCORE_BRIDGE_SO_RCVTIMEO
#include "../build/src/sm_shellcore_hooks.c"

static uint8_t memory[8192];
static int read_failures, attach_count, write_count, dispatch_count;
static int fail_attach, fail_detach, fail_patch, corrupt_verify, failed_verify;
static int wrong_image, conflict_on_attach, bad_rollback;
static pid_t current_pid;
static sm_shellcore_firmware_offsets_t offsets;
static uintptr_t target;
static unsigned tests;
static bool kstuff_loaded = true;
static int lock_fail_at = -1;
static size_t locked_count;
static uintptr_t locked_pages[8];
bool sm_kstuff_is_loaded(void) { return kstuff_loaded; }
bool sm_kstuff_remote_mlock(pid_t pid, uintptr_t address, size_t size) {
  assert(pid == 42 && size == 0x4000 && (address & 0x3fff) == 0);
  assert(locked_count < 8);
  if ((int)locked_count == lock_fail_at) return false;
  locked_pages[locked_count++] = address;
  return true;
}
static void test_page_pinning(void) {
  sm_shellcore_firmware_offsets_t layout = {0};
  sm_shellcore_remote_t remote = {.pid=42, .offsets=&layout};
  remote.targets[SM_SHELLCORE_TARGET_LAUNCH_APP] = 0x1fff8;
  layout.targets[SM_SHELLCORE_TARGET_LAUNCH_APP].patch_size = 12;
  remote.targets[SM_SHELLCORE_TARGET_SANDBOX_READY] = 0x20040;
  layout.targets[SM_SHELLCORE_TARGET_SANDBOX_READY].patch_size = 5;
  remote.targets[SM_SHELLCORE_TARGET_INSTALL_ALL] = 0x28000;
  layout.targets[SM_SHELLCORE_TARGET_INSTALL_ALL].patch_size = 12;
  assert(lock_hook_pages(42, &remote, 3, 0x2bff0, 64));
  assert(locked_count == 4);
  assert(locked_pages[0] == 0x1c000 && locked_pages[1] == 0x20000 &&
         locked_pages[2] == 0x28000 && locked_pages[3] == 0x2c000);
  for (int i = 0; i < 4; ++i) {
    locked_count = 0; lock_fail_at = i;
    assert(!lock_hook_pages(42, &remote, 3, 0x2bff0, 64));
    assert(locked_count == (size_t)i);
  }
  lock_fail_at = -1; locked_count = 0; kstuff_loaded = false;
  assert(!lock_hook_pages(42, &remote, 3, 0x2bff0, 64));
  assert(locked_count == 0); kstuff_loaded = true;
  assert(!lock_hook_pages(42, &remote, 4, 0x2bff0, 64));
  locked_count = 0;
  assert(!lock_hook_pages(42, &remote, 3, UINTPTR_MAX-4, 64));
  locked_count = 0;
  assert(lock_hook_pages(42, &remote, 2, 0x20080, 64));
  assert(locked_count == 2); // No install hook on older firmware; deduplicated cave.
  locked_count = 0;
}


void log_debug(const char *fmt, ...) { (void)fmt; }
pid_t find_pid_by_name(const char *name, bool exclude) {
  (void)name; (void)exclude; return current_pid;
}
uint32_t kernel_get_fw_version(void) { return 0x13000000; }
int kernel_dynlib_handle(pid_t pid, const char *name, uint32_t *handle) {
  (void)pid; (void)name; *handle=1; return 0;
}
void *kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *name) {
  (void)pid; (void)handle; (void)name; return (void *)0x1000;
}
const char *sm_shellcore_target_name(sm_shellcore_target_t value) {
  (void)value; return "test";
}
bool sm_shellcore_remote_resolve(pid_t pid, sm_shellcore_remote_t *out) {
  *out=g_hooks.remote; out->pid=pid;
  if(wrong_image) out->image_base++;
  return true;
}
bool sm_remote_process_attach(pid_t pid) {
  assert(pid==42); attach_count++;
  if(conflict_on_attach) memory[target]=0xcc;
  return !fail_attach;
}
bool sm_remote_process_detach(pid_t pid) {
  assert(pid==42); return !fail_detach;
}
bool sm_remote_process_read(pid_t pid, uintptr_t address, void *data, size_t size) {
  assert(pid==42); assert(address+size<=sizeof(memory));
  if(address==target && read_failures) {read_failures--; return false;}
  if(address==target && failed_verify) {failed_verify=0; return false;}
  memcpy(data,memory+address,size); return true;
}
bool sm_remote_process_write(pid_t pid, uintptr_t address, const void *data, size_t size) {
  assert(pid==42); assert(address+size<=sizeof(memory)); write_count++;
  if(address==target && fail_patch) {
    // Model a partial write before failure and independently failing rollback.
    memory[address]=0x48;
    if(!bad_rollback) fail_patch=0;
    return false;
  }
  memcpy(memory+address,data,size);
  if(address==target && corrupt_verify) {failed_verify=1; corrupt_verify=0;}
  return true;
}
int sceAppInstUtilAppInstallAll(void *reserved) {
  assert(reserved==NULL); dispatch_count++;
  assert(memory[remote_bridge_symbol(sm_shellcore_bridge_install_armed)]==1);
  assert(remote_hook_matches(42,target,remote_bridge_symbol(sm_shellcore_bridge_install_all_hook),16));
  return 0;
}
static void reset(bool installed) {
  memset(&g_hooks,0,sizeof(g_hooks)); memset(memory,0,sizeof(memory));
  read_failures=attach_count=write_count=dispatch_count=0;
  fail_attach=fail_detach=fail_patch=corrupt_verify=failed_verify=0;
  wrong_image=conflict_on_attach=bad_rollback=0; current_pid=42;
  g_hooks.status=SHELLCORE_HOOKS_READY;
  g_hooks.remote.pid=42; g_hooks.remote.image_base=100;
  g_hooks.remote.offsets=&offsets; g_hooks.hook_count=3;
  g_hooks.bridge_address=1024;
  g_hooks.bridge_size=sm_shellcore_bridge_blob_end-sm_shellcore_bridge_blob_start;
  assert(g_hooks.bridge_size<=sizeof(g_hooks.expected_bridge));
  memcpy(g_hooks.expected_bridge,sm_shellcore_bridge_blob_start,g_hooks.bridge_size);
  memcpy(memory+g_hooks.bridge_address,g_hooks.expected_bridge,g_hooks.bridge_size);
  target=g_hooks.remote.targets[SM_SHELLCORE_TARGET_INSTALL_ALL]=4096;
  g_hooks.hooks[2].target=SM_SHELLCORE_TARGET_INSTALL_ALL;
  g_hooks.hooks[2].original_size=16;
  memset(g_hooks.hooks[2].original,0x90,16);
  memcpy(g_hooks.hooks[2].original,k_expected_function_prologue,4);
  memcpy(memory+target,g_hooks.hooks[2].original,16);
  if(installed) assert(patch_remote_jump(42,target,remote_bridge_symbol(sm_shellcore_bridge_install_all_hook),16));
  write_count=0;
}
static bool install(void) {
  int result=-1;
  bool ok=sm_shellcore_install_title_dir("PPSA99998","/data/test",&result);
  if(ok) assert(result==0);
  tests++;
  return ok;
}
static void *parallel_install(void *unused) {
  (void)unused; int result=-1;
  assert(sm_shellcore_install_title_dir("PPSA99998", "/data/test", &result));
  assert(result==0); return NULL;
}
int main(void) {
  test_page_pinning();
  reset(true); assert(install()); assert(attach_count==0 && dispatch_count==1);
  reset(true); read_failures=1; assert(install()); assert(attach_count==0);
  reset(true); read_failures=2; assert(!install());
  assert(g_hooks.status==SHELLCORE_HOOKS_READY && write_count==0);
  assert(install()); // No permanent failure after a transient read error.
  reset(false); assert(install()); assert(attach_count==1 && dispatch_count==1);
  assert(g_hooks.status==SHELLCORE_HOOKS_READY);
  assert(install()); assert(attach_count==1); // Repair exactly once.
  reset(false); g_hooks.status=SHELLCORE_HOOKS_STALE; assert(install());
  reset(false); memory[remote_bridge_symbol(sm_shellcore_bridge_install_title_id_0)]=17;
  memory[remote_bridge_symbol(sm_shellcore_bridge_install_dir_0)]=99;
  assert(install()); // Previous request data is intentionally mutable.
  reset(false); memory[target]=0xcc; assert(!install()); assert(write_count==0 && attach_count==0);
  reset(false); memory[g_hooks.bridge_address]^=1; assert(!install()); assert(write_count==0);
  reset(false); memory[remote_bridge_symbol(sm_shellcore_bridge_install_armed)]=1;
  assert(!install()); assert(write_count==0);
  reset(false); wrong_image=1; assert(!install()); assert(write_count==0);
  reset(false); conflict_on_attach=1; assert(!install()); assert(write_count==0);
  reset(false); fail_attach=1; assert(!install()); assert(write_count==0);
  fail_attach=0; assert(install());
  reset(false); fail_patch=1; assert(!install()); assert(dispatch_count==0);
  assert(memcmp(memory+target,g_hooks.hooks[2].original,16)==0); assert(install());
  reset(false); corrupt_verify=1; assert(!install()); assert(dispatch_count==0);
  assert(memcmp(memory+target,g_hooks.hooks[2].original,16)==0);
  reset(false); fail_patch=bad_rollback=1; assert(!install());
  assert(g_hooks.status==SHELLCORE_HOOKS_ROLLBACK_PENDING);
  int before=write_count; assert(!install()); assert(write_count==before);
  reset(false); fail_detach=1; assert(!install()); assert(dispatch_count==0);
  assert(g_hooks.status==SHELLCORE_HOOKS_ROLLBACK_PENDING);
  assert(!install()); assert(dispatch_count==0);
  reset(false); current_pid=43; assert(!install()); assert(write_count==0);
  reset(false); g_hooks.status=SHELLCORE_HOOKS_ROLLBACK_PENDING;
  assert(!install()); assert(write_count==0);
  reset(false); pthread_t workers[8];
  for(unsigned i=0;i<8;i++) assert(pthread_create(&workers[i],NULL,parallel_install,NULL)==0);
  for(unsigned i=0;i<8;i++) assert(pthread_join(workers[i],NULL)==0);
  assert(attach_count==1 && dispatch_count==8);
  printf("TitleDir production-code regressions passed (%u calls).\n",tests);
  return 0;
}
