#pragma once
#include <sys/stat.h>
#include <sys/time.h>
#ifndef BOTTY_RT_BOOT_QUERY
#include <sys/sysctl.h>
#define BOTTY_RT_BOOT_QUERY sysctlbyname
#endif
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>

static int botty_rtorrent_publish_runtime(const char* path = "/data/botty/rtorrent/state") {
    struct timeval boot = {};
    size_t size = sizeof(boot);
    if (BOTTY_RT_BOOT_QUERY("kern.boottime", &boot, &size, nullptr, 0) != 0 ||
        size != sizeof(boot) || boot.tv_sec <= 0 || boot.tv_usec < 0 || boot.tv_usec >= 1000000)
        return 1;
    pid_t pid = getpid();
    if (pid <= 0 || !path || path[0] != '/' || strlen(path) >= 1024) return 1;
    char directory[1024];
    strcpy(directory, path + 1);
    int dir = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) return 1;
    char* cursor = directory;
    while (*cursor) {
        char* slash = strchr(cursor, '/');
        if (slash) *slash = '\0';
        if (!*cursor || !strcmp(cursor, ".") || !strcmp(cursor, "..")) { close(dir); return 1; }
        int next = openat(dir, cursor, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(dir);
        if (next < 0) return 1;
        dir = next;
        if (!slash) break;
        cursor = slash + 1;
        if (!*cursor) { close(dir); return 1; }
    }
    struct stat st;
    if (!*directory || fstat(dir, &st) != 0 || st.st_uid != getuid() || fchmod(dir, 0700) != 0) {
        close(dir); return 1;
    }
    if (fstatat(dir, "runtime.json", &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1) { close(dir); return 1; }
    } else if (errno != ENOENT) { close(dir); return 1; }
    char body[256], temporary[64];
    int length = snprintf(body, sizeof(body),
        "{\"schema\":1,\"pid\":%jd,\"version\":\"0.16.24-botty5\",\"boot\":{\"seconds\":%jd,\"microseconds\":%jd}}\n",
        (intmax_t)pid, (intmax_t)boot.tv_sec, (intmax_t)boot.tv_usec);
    int name_length = snprintf(temporary, sizeof(temporary), "runtime.json.tmp.%jd", (intmax_t)pid);
    if (length <= 0 || (size_t)length >= sizeof(body) || name_length <= 0 || (size_t)name_length >= sizeof(temporary)) {
        close(dir); return 1;
    }
    int fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { close(dir); return 1; }
    int result = fchmod(fd, 0600) != 0;
    size_t written = 0;
    while (!result && written < (size_t)length) {
        ssize_t count = write(fd, body + written, (size_t)length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = 1; break; }
        written += (size_t)count;
    }
    if (!result && fsync(fd) != 0) result = 1;
    if (close(fd) != 0) result = 1;
    if (!result && renameat(dir, temporary, dir, "runtime.json") != 0) result = 1;
    if (!result && fsync(dir) != 0) result = 1;
    if (result) unlinkat(dir, temporary, 0);
    if (close(dir) != 0) result = 1;
    return result;
}
