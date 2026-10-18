#include <assert.h>
#include <stdint.h>
#include <string.h>
static const char *loaded;
int sceKernelUsleep(uint32_t us) { (void)us; return 0; }
int sceKernelDebugOutText(int channel, const char *text) { (void)channel; (void)text; return 0; }
int sceSystemServiceLoadExec(const char *path, const char **args) { assert(!args); loaded=path; return -1; }
void eden_set_botty_session(int active);
int eden_restart_app(void);
int eden_exit_app(void);
int main(void) {
    assert(eden_restart_app()==-1 && !strcmp(loaded,"/app0/emulation/eboot.bin"));
    eden_set_botty_session(1);
    assert(eden_restart_app()==-1 && !strcmp(loaded,"/app0/eboot.bin"));
    assert(eden_exit_app()==-1 && !strcmp(loaded,"/app0/eboot.bin"));
}
