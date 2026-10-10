// SPDX-License-Identifier: GPL-3.0-only
// Adapted from atreus04-GG/ps5tailscale (v1.0.3); see README.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sdk_shim.h"
#include "../botty/src/rest-companion-protocol.h"
#include <sys/stat.h>
#include <sys/file.h>

#define TARGET_PROCESS "SceJSCd"
#define THREAD_NAME BOTTY_REST_THREAD
#define STATUS_PATH "/data/botty/rest-mode-injector.status"
#define INIT_FAILURE_PATH "/data/botty/rest-mode-init.failed"
#define PENDING_PATH "/data/botty/rest-mode-injection.pending"
#define MAX_THREADS 2048U

extern void port_outer_init_mutexes(void);
extern int find_proc_pid_by_name(const char *name);
extern int sys_proc_call_remote_func(int pid, void *elf_buf,
                                     uint64_t a3_unused,
                                     void *thread_name);
extern const uint8_t embedded_inner_start[];
extern const uint64_t embedded_inner_size;

static int manager_authorized(void)
{
    char marker[32] = {0};
    int fd = open("/data/botty/rest-mode-companion.enabled", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    ssize_t n = read(fd, marker, sizeof(marker));
    close(fd);
    if (n != 18 || memcmp(marker, "experimental-fw13\n", 18)) return 0;
    botty_rest_control control;
    fd = open(BOTTY_REST_CONTROL, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat info;
    int valid = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size == (off_t)sizeof(control);
    n = valid ? read(fd, &control, sizeof(control)) : -1;
    close(fd);
    if (n != (ssize_t)sizeof(control)) return 0;
    struct timeval boot;
    struct timespec now;
    size_t length = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &length, NULL, 0) || length != sizeof(boot) ||
        clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0) return 0;
    return botty_rest_control_active(&control, (uint64_t)now.tv_sec,
                                    (uint64_t)boot.tv_sec, (uint64_t)boot.tv_usec) &&
           (kill((pid_t)control.pid, 0) == 0 || errno == EPERM);
}

static void write_status(const char *status)
{
    // Exclusive creation prevents following symlinks or truncating hard links.
    char temporary[] = "/data/botty/rest-mode-injector.status.XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0) return;
    char data[160];
    int length = snprintf(data, sizeof(data), "%s\n", status);
    size_t offset = 0;
    while (length > 0 && length < (int)sizeof(data) && offset < (size_t)length) {
        ssize_t n = write(fd, data + offset, (size_t)length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += (size_t)n;
    }
    int failed = fsync(fd);
    if (close(fd)) failed = 1;
    if (!failed && length > 0 && offset == (size_t)length) (void)rename(temporary, STATUS_PATH);
    (void)unlink(temporary);
}

static int treatment_thread_present(int pid)
{
    intptr_t kproc = kernel_get_proc((pid_t)pid);
    unsigned long kthread = 0;
    unsigned count = 0;

    if (!kproc || kernel_copyout(kproc + 0x10, &kthread, sizeof(kthread)) != 0) {
        return -1;
    }
    while (kthread != 0 && count < MAX_THREADS) {
        unsigned long next = 0;
        char name[33];
        memset(name, 0, sizeof(name));
        if (kernel_copyout(kthread + 0x10, &next, sizeof(next)) != 0 ||
            kernel_copyout(kthread + 0x294, name, 32) != 0) {
            return -1;
        }
        name[32] = '\0';
        if (strcmp(name, THREAD_NAME) == 0) return 1;
        if (next == kthread) return -1;
        kthread = next;
        count++;
    }
    if (kthread != 0) return -1;
    return 0;
}

static int wait_for_resident_thread(int pid)
{
    unsigned attempt;
    for (attempt = 0; attempt < 50U; attempt++) {
        int census = treatment_thread_present(pid);
        if (census > 0) return 0;
        (void)usleep(100000);
    }
    return -1;
}

// Persist uncertainty across injector exits. Never reinject in the same boot
// after an unresolved initialization, even when the named thread is absent.
static int mark_injection_pending(int pid) {
    struct timeval boot;
    size_t length = sizeof(boot);
    uint64_t record[3], previous[3];
    if (sysctlbyname("kern.boottime", &boot, &length, NULL, 0) || length != sizeof(boot)) return -1;
    record[0] = (uint64_t)boot.tv_sec;record[1] = (uint64_t)boot.tv_usec;record[2] = (uint64_t)pid;
    int fd = open(PENDING_PATH, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
        ssize_t n = read(fd, previous, sizeof(previous));close(fd);
        if (n != sizeof(previous)) return -1;
        if (previous[0] == record[0] && previous[1] == record[1]) return -1;
        if (unlink(PENDING_PATH)) return -1;
    } else if (errno != ENOENT) return -1;
    char temporary[] = PENDING_PATH ".XXXXXX";
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    size_t offset = 0;
    while (offset < sizeof(record)) {
        ssize_t n = write(fd, (const char *)record + offset, sizeof(record) - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += (size_t)n;
    }
    int failed = offset != sizeof(record);
    if (fsync(fd)) failed = 1;
    if (close(fd)) failed = 1;
    // Publish complete bytes atomically before any target mutation.
    if (!failed && rename(temporary, PENDING_PATH)) failed = 1;
    (void)unlink(temporary);
    return failed ? -1 : 0;
}
static int initialization_failed(void) {
    char marker[8];
    int fd = open(INIT_FAILURE_PATH, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat info;
    int regular = !fstat(fd, &info) && S_ISREG(info.st_mode);
    ssize_t count = regular ? read(fd, marker, sizeof(marker)) : -1;
    close(fd);
    return count == 7 && !memcmp(marker, "failed\n", 7);
}
static intptr_t saved_filter_addr;
static unsigned long saved_filter[2];

static int restore_target_syscall_filter(void)
{
    return !saved_filter_addr ? 0 : kernel_copyin(saved_filter, saved_filter_addr + 0xf0, sizeof(saved_filter));
}

static int unlock_target_syscall_filter(int pid)
{
    intptr_t kproc = kernel_get_proc((pid_t)pid);
    intptr_t filter_addr = 0;
    unsigned long value;

    if (!kproc) return -1;
    if (kernel_copyout(kproc + 0x3e8, &filter_addr, sizeof(filter_addr)) != 0 ||
        ((uint64_t)filter_addr >> 48) != 0xffff) return -1;
    if (kernel_copyout(filter_addr + 0xf0, saved_filter, sizeof(saved_filter)) != 0) return -1;
    saved_filter_addr = filter_addr;
    value = 0UL;
    if (kernel_copyin(&value, filter_addr + 0xf0, sizeof(value)) != 0) {
        return restore_target_syscall_filter() == 0 ? -1 : -2;
    }
    value = (unsigned long)-1L;
    if (kernel_copyin(&value, filter_addr + 0xf8, sizeof(value)) != 0) {
        return restore_target_syscall_filter() == 0 ? -1 : -2;
    }
    return 0;
}

int main(void)
{
    int pid;
    int census;
    int rc;

    // The inherited kernel offsets are an experimental FW 13.00 profile only.
    if ((kernel_get_fw_version() & 0xffff0000U) != BOTTY_REST_FW) {
        write_status("error:firmware-profile-not-supported");
        return 10;
    }
    if (!manager_authorized()) {
        write_status("error:manager-control-not-authorized");
        return 11;
    }
    // Serialize the census/injection sequence across concurrent payload launches.
    // The kernel releases this lock when the injector process exits.
    int lock_fd = open("/data/botty/rest-mode-injector.lock", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) {
        write_status("error:injector-busy-or-lock-unavailable");
        return 12;
    }
    port_outer_init_mutexes();
    pid = find_proc_pid_by_name(TARGET_PROCESS);
    if (pid < 0) {
        write_status("error:scejscd-not-found");
        payload_exit(1);
        return 1;
    }

    census = treatment_thread_present(pid);
    if (census > 0) {
        write_status("ok:already-resident-scejscd");
        payload_exit(0);
        return 0;
    }
    if (census < 0) {
        write_status("error:thread-census-incomplete");
        payload_exit(2);
        return 2;
    }
    if (mark_injection_pending(pid)) {
        write_status("error:initialization-pending-or-state-unavailable");
        return 13;
    }
    if (unlink(INIT_FAILURE_PATH) && errno != ENOENT) {
        write_status(unlink(PENDING_PATH) ? "error:initialization-state-unavailable-pending-clear-failed" : "error:initialization-state-unavailable");
        return 14;
    }
    if (!manager_authorized()) {
        write_status(unlink(PENDING_PATH) ? "error:authorization-expired-pending-clear-failed" : "error:manager-authorization-expired-before-mutation");
        return 15;
    }
    rc = unlock_target_syscall_filter(pid);
    if (rc != 0) {
        if (rc == -1 && unlink(PENDING_PATH)) {
            write_status("error:safe-unlock-failure-pending-clear-failed");
        } else {
            write_status(rc == -2 ? "error:filter-rollback-failed-reboot-required" : "error:syscall-filter-unlock-no-mutation-or-restored");
        }
        payload_exit(3);
        return 3;
    }

    if (!manager_authorized()) {
        if (restore_target_syscall_filter()) {
            write_status("error:authorization-revoked-filter-restore-failed-reboot-required");
        } else if (unlink(PENDING_PATH)) {
            write_status("error:authorization-revoked-filter-restored-pending-clear-failed");
        } else {
            write_status("error:authorization-revoked-filter-restored");
        }
        return 16;
    }
    rc = sys_proc_call_remote_func(pid, (void *)embedded_inner_start, 0, NULL);
    if (rc != 0) {
        // The upstream function can fail detaching after setting execution
        // registers. Its result alone cannot prove the resident will not run.
        if (initialization_failed()) {
            write_status(restore_target_syscall_filter() == 0 ? "error:resident-init-failed-filter-restored" : "error:resident-init-failed-filter-restore-failed");
        } else {
            write_status("error:remote-execution-unresolved-reboot-required");
        }
        payload_exit(4);
        return 4;
    }
    if (wait_for_resident_thread(pid) != 0) {
        if (initialization_failed()) {
            write_status(restore_target_syscall_filter() == 0 ? "error:resident-init-failed-filter-restored" : "error:resident-init-failed-filter-restore-failed");
        } else {
            // A timeout is ambiguous: keep the pending boot record and refuse
            // subsequent launches rather than altering a possibly live resident.
            write_status("error:resident-init-unresolved-reboot-required");
        }
        payload_exit(5);
        return 5;
    }
    write_status("ok:injected-scejscd-ready");
    payload_exit(0);
    return 0;
}
