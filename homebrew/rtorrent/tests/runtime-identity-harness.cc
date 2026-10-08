#include <sys/time.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static const char* runtime_path;
static unsigned random_calls;
static int boot_query(const char* name, void* output, size_t* length, const void* input, size_t input_length) {
    if (strcmp(name, "kern.boottime") || input || input_length) return -1;
    const char* mode = getenv("BOOT_TEST_MODE");
    if (mode && !strcmp(mode, "unavailable")) { errno = ENOENT; return -1; }
    timeval value = {1700000000, 123456};
    if (mode && !strcmp(mode, "seconds")) value.tv_sec = 0;
    if (mode && !strcmp(mode, "negative")) value.tv_usec = -1;
    if (mode && !strcmp(mode, "microseconds")) value.tv_usec = 1000000;
    memcpy(output, &value, sizeof(value));
    *length = mode && !strcmp(mode, "size") ? sizeof(value) - 1 : sizeof(value);
    return 0;
}
#define BOTTY_RT_BOOT_QUERY boot_query
#define BOTTY_RT_STATE_PATH runtime_path
struct botty_rt_syscall_result;
static botty_rt_syscall_result test_at_syscall(long, long, long, long, long);
static void test_random_bytes(void* output, size_t size) {
    memset(output, random_calls++, size);
}
#define BOTTY_RT_AT_SYSCALL test_at_syscall
#define BOTTY_RT_RANDOM_BYTES test_random_bytes
#include "../ps5-entry.hpp"

static botty_rt_syscall_result test_at_syscall(long number, long first, long second, long third, long fourth) {
    const char* failure = getenv("AT_TEST_FAILURE");
    if (failure && (!strcmp(failure, "all") || strtol(failure, nullptr, 10) == number)) return {EACCES, true};
    long result;
    switch (number) {
    case 499: result = openat((int)first, (const char*)second, (int)third, (mode_t)fourth); break;
    case 493: result = fstatat((int)first, (const char*)second, (struct stat*)third, (int)fourth); break;
    case 501: result = renameat((int)first, (const char*)second, (int)third, (const char*)fourth); break;
    case 503: result = unlinkat((int)first, (const char*)second, (int)third); break;
    default: return {ENOSYS, true};
    }
    return {result < 0 ? errno : result, result < 0};
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    runtime_path = argv[2];
    if (!strcmp(argv[1], "owners")) {
        if (!botty_rtorrent_trusted_owner(0, 1) || !botty_rtorrent_trusted_owner(1, 1) ||
            botty_rtorrent_trusted_owner(2, 1) || botty_rtorrent_trusted_owner(1, 0)) return 12;
        return 0;
    }
    if (!strcmp(argv[1], "at-errors")) {
        setenv("AT_TEST_FAILURE", "all", 1);
        struct stat info = {};
        errno = 0;
        if (botty_rt_openat(-1, "missing", O_RDONLY) != -1 || errno != EACCES) return 9;
        errno = 0;
        if (botty_rt_fstatat(-1, "missing", &info, AT_SYMLINK_NOFOLLOW) != -1 || errno != EACCES) return 9;
        errno = 0;
        if (botty_rt_renameat(-1, "missing", -1, "new") != -1 || errno != EACCES) return 9;
        errno = 0;
        if (botty_rt_unlinkat(-1, "missing", 0) != -1 || errno != EACCES) return 9;
        return 0;
    }
    if (!strcmp(argv[1], "stale")) {
        char legacy[1200], collision[1200];
        snprintf(legacy, sizeof(legacy), "%s/runtime.json.tmp.%jd", runtime_path, (intmax_t)getpid());
        snprintf(collision, sizeof(collision), "%s/runtime.json.tmp.%jd.%032d", runtime_path, (intmax_t)getpid(), 0);
        FILE* old = fopen(legacy, "wx");
        if (!old) return 10;
        fputs("preserve stale file", old);
        if (fclose(old) || symlink(legacy, collision)) return 10;
        const int result=botty_rtorrent_publish_runtime(runtime_path);
        if(!result&&random_calls<2)return 11;
        return result;
    }
    if (!strcmp(argv[1], "publish")) return botty_rtorrent_publish_runtime(runtime_path);
    bool supervised = !strcmp(argv[1], "supervised");
    char name[] = "rtorrent", flag[] = "-n";
    char* arguments[] = {name, flag, nullptr};
    char** entry_argv = arguments;
    int entry_argc = supervised ? 2 : 1;
    if (botty_rtorrent_init(entry_argc, entry_argv)) return 1;
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, runtime_path)) return 3;
    if (supervised) {
        if (entry_argc != 2 || entry_argv != arguments) return 4;
    } else if (entry_argc != 4 || strcmp(entry_argv[0], "rtorrent") || strcmp(entry_argv[1], "-n") ||
               strcmp(entry_argv[2], "-o") || strcmp(entry_argv[3], "import=/data/botty/rtorrent/state/rtorrent.rc") ||
               entry_argv[4] || !getenv("HOME") || strcmp(getenv("HOME"), runtime_path)) return 5;
    pid_t child = fork();
    if (child < 0) return 6;
    if (!child) _exit(botty_rtorrent_init(entry_argc, entry_argv) == 1 ? 0 : 7);
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 8;
    return 0;
}
