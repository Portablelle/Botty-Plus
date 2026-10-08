#pragma once
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include "runtime-identity.hpp"

#ifndef BOTTY_RT_STATE_PATH
#define BOTTY_RT_STATE_PATH "/data/botty/rtorrent/state"
#endif

// The ELF loader supplies no shell environment. Explicit arguments are preserved
// for a supervised launcher; the no-argument payload uses Botty's private state.
static int botty_rtorrent_init(int& argc, char**& argv) {
    const char* state = BOTTY_RT_STATE_PATH;
    if (chdir(state) != 0) return 1;
    int lock = open("daemon.lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 1;
    // Keep the descriptor open for the process lifetime.
    int output = open("runtime.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (output < 0) return 1;
    dup2(output, STDOUT_FILENO);
    dup2(output, STDERR_FILENO);
    if (output > STDERR_FILENO) close(output);
    FILE* pid = fopen("rtorrent.pid", "w");
    if (!pid) return 1;
    fprintf(pid, "%d\n", getpid());
    fclose(pid);
    if (botty_rtorrent_publish_runtime(state) != 0) {
        fprintf(stderr, "Botty rTorrent runtime identity publication failed (pid %d, uid %d, errno %d)\n", getpid(), getuid(), errno);
        return 1;
    }
    fprintf(stderr, "Botty rTorrent payload entered (pid %d)\n", getpid());
    // Every invocation holds the kernel lock, including supervised launches.
    // Unlike a PID file, flock is released on exit and survives PID reuse safely.
    if (argc > 1) return 0;
    setenv("HOME", state, 1);
    static char name[] = "rtorrent";
    static char ignore[] = "-n";
    static char option[] = "-o";
    static char config[] = "import=/data/botty/rtorrent/state/rtorrent.rc";
    static char* arguments[] = {name, ignore, option, config, nullptr};
    argc = 4;
    argv = arguments;
    return 0;
}
