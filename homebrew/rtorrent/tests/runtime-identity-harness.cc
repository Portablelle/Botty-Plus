#include <sys/time.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/wait.h>

static const char* runtime_path;
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
#include "../ps5-entry.hpp"

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    runtime_path = argv[2];
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
