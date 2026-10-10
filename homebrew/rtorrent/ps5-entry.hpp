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
        fprintf(stderr, "Botty rTorrent runtime identity publication failed (pid %d, uid %u)\n", getpid(), static_cast<unsigned>(getuid()));
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
    // Older managers preserve state/rtorrent.rc during an in-app update. Apply
    // the revision's allocation before importing it (and opening SCGI), then
    // reapply it before upstream restores sessions even if the config overrides it.
    static char allocation[] = "system.sockets.files.max_alloc.set=64,system.sockets.adjust_alloc=";
    static char* arguments[] = {name, ignore, option, allocation, option, config, option, allocation, nullptr};
    argc = 8;
    argv = arguments;
    return 0;
}
