/* Bounded PS5 runtime check. Only touches newly created files under this directory. */
#include <sys/types.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

static void report(int fd, const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) write(fd, buffer, n < (int)sizeof(buffer) ? (size_t)n : sizeof(buffer) - 1);
}
#define dprintf report

int main(void) {
    const char *root = "/data/botty/rtorrent-probe";
    char output[160], data[160];
    snprintf(output, sizeof(output), "%s/result-%ld-%d.txt", root, (long)time(NULL), getpid());
    int log = open(output, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (log < 0) return 1;
    dprintf(log, "pagesize=%d open_max=%ld\n", getpagesize(), sysconf(_SC_OPEN_MAX));
    int kq = kqueue();
    dprintf(log, "kqueue=%d errno=%d\n", kq, kq < 0 ? errno : 0);
    if (kq >= 0) {
        struct kevent change, event;
        struct timespec timeout = {1, 0};
        EV_SET(&change, 7, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
        int r = kevent(kq, &change, 1, NULL, 0, NULL);
        dprintf(log, "kevent_add=%d errno=%d\n", r, r < 0 ? errno : 0);
        EV_SET(&change, 7, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        r = kevent(kq, &change, 1, &event, 1, &timeout);
        dprintf(log, "kevent_trigger=%d errno=%d ident=%lu\n", r, r < 0 ? errno : 0, r > 0 ? (unsigned long)event.ident : 0);
        close(kq);
    }
    snprintf(data, sizeof(data), "%s/data-%d.bin", root, getpid());
    int fd = open(data, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { dprintf(log, "open_data errno=%d\n", errno); close(log); return 1; }
    const size_t length = 4 * 1024 * 1024;
    int r = ftruncate(fd, length);
    dprintf(log, "ftruncate=%d errno=%d\n", r, r < 0 ? errno : 0);
    unsigned char *p = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    dprintf(log, "mmap=%s errno=%d\n", p == MAP_FAILED ? "failed" : "ok", p == MAP_FAILED ? errno : 0);
    if (p != MAP_FAILED) {
        for (size_t i = 0; i < length; ++i) p[i] = (unsigned char)(i * 31 + i / 16384);
        char residency[4096];
        r = mincore(p, length, residency);
        dprintf(log, "mincore=%d errno=%d\n", r, r < 0 ? errno : 0);
        r = madvise(p, length, MADV_RANDOM);
        dprintf(log, "madvise=%d errno=%d\n", r, r < 0 ? errno : 0);
        r = msync(p, length, MS_SYNC);
        dprintf(log, "msync=%d errno=%d\n", r, r < 0 ? errno : 0);
        munmap(p, length);
        unsigned char buf[16384];
        int valid = 1;
        for (size_t offset = 0; offset < length; offset += sizeof(buf)) {
            if (pread(fd, buf, sizeof(buf), offset) != sizeof(buf)) { valid = 0; break; }
            for (size_t i = 0; i < sizeof(buf); ++i)
                if (buf[i] != (unsigned char)((offset + i) * 31 + (offset + i) / 16384)) valid = 0;
        }
        dprintf(log, "readback=%s\n", valid ? "ok" : "FAILED");
    }
    close(fd);
    dprintf(log, "complete\n");
    close(log);
    return 0;
}
