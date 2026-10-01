#ifndef SM_PLATFORM_H
#define SM_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
uint32_t kernel_get_fw_version(void);
int kernel_dynlib_handle(pid_t pid, const char *name, uint32_t *handle);
void *kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *name);
int sceAppInstUtilAppInstallAll(void *reserved);
#endif
