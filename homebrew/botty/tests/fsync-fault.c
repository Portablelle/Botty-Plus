#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
// Linux integration-only injection. Fail a selected directory sync until the
// fixture removes its control file; production has no fault-injection hooks.
int fsync(int fd) {
  static int (*real_fsync)(int);
  if (!real_fsync) real_fsync = dlsym(RTLD_NEXT, "fsync");
  const char *control = getenv("BOTTY_FAIL_DIRECTORY_SYNC");
  if (control) {
    FILE *input = fopen(control, "r");
    if (input) {
      char expected[4096], actual[4096], prerequisite[4096] = {0}, link[64];
      if (fgets(expected, sizeof expected, input)) {
        expected[strcspn(expected, "\n")] = 0;
        if (fgets(prerequisite, sizeof prerequisite, input)) prerequisite[strcspn(prerequisite, "\n")] = 0;
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, actual, sizeof actual - 1);
        if (n >= 0) {
          actual[n] = 0;
          if (!strcmp(expected, actual) && (!prerequisite[0] || !access(prerequisite, F_OK))) { fclose(input); errno = EIO; return -1; }
        }
      }
      fclose(input);
    }
  }
  return real_fsync(fd);
}
