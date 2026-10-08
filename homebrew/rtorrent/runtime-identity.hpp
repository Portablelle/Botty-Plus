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
#include <cstdlib>
#include "runtime-at.hpp"
#include "runtime-version.hpp"
#ifndef BOTTY_RT_RANDOM_BYTES
#define BOTTY_RT_RANDOM_BYTES arc4random_buf
#endif

// Portal creates storage as root; ELF-loader payloads can have real uid 1.
// Accept the privileged installer or this process, never an unrelated owner.
static bool botty_rtorrent_trusted_owner(uid_t owner, uid_t process_uid = getuid()) {
    return owner == 0 || owner == process_uid;
}

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
        int next = botty_rt_openat(dir, cursor, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(dir);
        if (next < 0) return 1;
        dir = next;
        if (!slash) break;
        cursor = slash + 1;
        if (!*cursor) { close(dir); return 1; }
    }
    struct stat st;
    if (!*directory || fstat(dir, &st) != 0 || !botty_rtorrent_trusted_owner(st.st_uid) || fchmod(dir, 0700) != 0) {
        close(dir); return 1;
    }
    if (botty_rt_fstatat(dir, "runtime.json", &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode) || !botty_rtorrent_trusted_owner(st.st_uid) || st.st_nlink != 1) { close(dir); return 1; }
    } else if (errno != ENOENT) { close(dir); return 1; }
    char body[256], temporary[96];
    int length = snprintf(body, sizeof(body),
        "{\"schema\":1,\"pid\":%jd,\"version\":\"" BOTTY_RT_RUNTIME_VERSION "\",\"boot\":{\"seconds\":%jd,\"microseconds\":%jd}}\n",
        (intmax_t)pid, (intmax_t)boot.tv_sec, (intmax_t)boot.tv_usec);
    if (length <= 0 || (size_t)length >= sizeof(body)) {
        close(dir); return 1;
    }
    int fd = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        unsigned char random[16];
        char suffix[33];
        BOTTY_RT_RANDOM_BYTES(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); ++i) snprintf(suffix + i * 2, 3, "%02x", (unsigned)random[i]);
        int name_length = snprintf(temporary, sizeof(temporary), "runtime.json.tmp.%jd.%s", (intmax_t)pid, suffix);
        if (name_length <= 0 || (size_t)name_length >= sizeof(temporary)) { close(dir); return 1; }
        fd = botty_rt_openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
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
    if (!result && botty_rt_renameat(dir, temporary, dir, "runtime.json") != 0) result = 1;
    if (!result && fsync(dir) != 0) result = 1;
    if (result) botty_rt_unlinkat(dir, temporary, 0);
    if (close(dir) != 0) result = 1;
    return result;
}
