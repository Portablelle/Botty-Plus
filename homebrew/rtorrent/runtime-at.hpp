#pragma once
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#ifdef __PS5__
#include <sys/syscall.h>
static_assert(SYS_openat == 499 && SYS_fstatat == 493 && SYS_renameat == 501 && SYS_unlinkat == 503);
#endif

struct botty_rt_syscall_result { long value; bool failed; };

static long botty_rt_at_call(long number, long first, long second, long third, long fourth = 0) {
#ifdef BOTTY_RT_AT_SYSCALL
    botty_rt_syscall_result result = BOTTY_RT_AT_SYSCALL(number, first, second, third, fourth);
#elif defined(__PS5__)
    register long argument asm("r10") = fourth;
    botty_rt_syscall_result result = {number, false};
    asm volatile("syscall" : "+a"(result.value), "=@ccc"(result.failed),
        "+D"(first), "+S"(second), "+d"(third), "+r"(argument)
        : : "rcx", "r8", "r9", "r11", "memory");
#else
    long value;
    switch (number) {
    case 499: value = openat((int)first, (const char*)second, (int)third, (mode_t)fourth); break;
    case 493: value = fstatat((int)first, (const char*)second, (struct stat*)third, (int)fourth); break;
    case 501: value = renameat((int)first, (const char*)second, (int)third, (const char*)fourth); break;
    case 503: value = unlinkat((int)first, (const char*)second, (int)third); break;
    default: errno = ENOSYS; return -1;
    }
    botty_rt_syscall_result result = {value < 0 ? errno : value, value < 0};
#endif
    if (result.failed) { errno = (int)result.value; return -1; }
    return result.value;
}

static int botty_rt_openat(int fd, const char* path, int flags, mode_t mode = 0) {
    return (int)botty_rt_at_call(499, fd, (long)path, flags, mode);
}
static int botty_rt_fstatat(int fd, const char* path, struct stat* info, int flags) {
    return (int)botty_rt_at_call(493, fd, (long)path, (long)info, flags);
}
static int botty_rt_renameat(int oldfd, const char* oldpath, int newfd, const char* newpath) {
    return (int)botty_rt_at_call(501, oldfd, (long)oldpath, newfd, (long)newpath);
}
static int botty_rt_unlinkat(int fd, const char* path, int flags) {
    return (int)botty_rt_at_call(503, fd, (long)path, flags);
}
