#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef BOTTY_PS5
#include <sys/sysctl.h>
/* FW 13.00 disassembly: a NUL-terminated reason goes to IPMI method 3. */
extern int sceSystemStateMgrRequestToKeepMainOnStandby(const char *reason);
#endif
#ifndef REST_PROBE_KEEP_MAIN
#define REST_PROBE_KEEP_MAIN 0
#endif

/* Default mode observes only. The separate experimental build renews a
 * SystemStateMgr request; it never patches memory or changes credentials. */
static volatile sig_atomic_t stopped;
static void stop_probe(int signal_number) { (void)signal_number; stopped = 1; }
static int64_t milliseconds(clockid_t clock_id) {
    struct timespec value;
    if (clock_gettime(clock_id, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
static int tcp_error(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return errno;
    int result = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        result = errno;
    } else {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(0x7f000001U);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            result = errno;
            if (result == EINPROGRESS) {
                struct pollfd item = {fd, POLLOUT, 0};
                int ready = poll(&item, 1, 100);
                if (ready > 0) {
                    socklen_t size = sizeof(result);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &size) < 0) result = errno;
                } else result = ready == 0 ? ETIMEDOUT : errno;
            }
        }
    }
    close(fd);
    return result;
}
static int listener(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 4) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        int error = errno; close(fd); errno = error; return -1;
    }
    return fd;
}
static void serve(int fd, const char *state) {
    int client = accept(fd, NULL, NULL);
    if (client < 0) return;
    if (fcntl(client, F_SETFL, O_NONBLOCK) == 0) {
        /* Drain a normal small HTTP request before closing the connection. */
        struct pollfd input = {client, POLLIN, 0};
        if (poll(&input, 1, 100) > 0 && (input.revents & POLLIN)) {
            char request[1024];
            (void)recv(client, request, sizeof(request), 0);
        }
        char response[1024];
        int length = snprintf(response, sizeof(response),
            "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n%s", strlen(state), state);
        if (length > 0 && (size_t)length < sizeof(response)) {
            size_t sent = 0;
            while (sent < (size_t)length) {
                ssize_t count = send(client, response + sent, (size_t)length - sent, 0);
                if (count <= 0) break;
                sent += (size_t)count;
            }
        }
    }
    close(client);
}
static unsigned number(const char *input, unsigned minimum, unsigned maximum) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(input, &end, 10);
    if (errno || end == input || *end || value < minimum || value > maximum) return 0;
    return (unsigned)value;
}
int main(int argc, char **argv) {
    unsigned duration = 600, port = 2122;
    const int keep_main_requested = REST_PROBE_KEEP_MAIN;
    int keep_main_enabled = keep_main_requested, keep_main_rc = 0;
    int64_t next_keep_request = 0;
#ifdef BOTTY_PS5
    const char *directory = "/data/botty/manager";
#else
    const char *directory = ".";
#endif
    int option;
    while ((option = getopt(argc, argv, "d:p:o:")) != -1) {
        switch (option) {
        case 'd': duration = number(optarg, 1, 600); break;
        case 'p': port = number(optarg, 1024, 65535); break;
        case 'o': directory = optarg; break;
        default: return 2;
        }
    }
    if (!duration || !port || optind != argc) return 2;
    int64_t start = milliseconds(CLOCK_REALTIME);
    if (start < 0) return 1;
    char path[1024];
    int size = snprintf(path, sizeof(path), "%s/rest-probe-%" PRId64 "-%ld.jsonl", directory, start, (long)getpid());
    if (size < 0 || (size_t)size >= sizeof(path)) return 1;
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output < 0) { perror("log open"); return 1; }
    FILE *log = fdopen(output, "w");
    if (!log) { close(output); return 1; }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, stop_probe);
    signal(SIGTERM, stop_probe);
    int server = listener((unsigned short)port);
    int listen_error = server < 0 ? errno : 0;
    fprintf(log, "{\"event\":\"start\",\"pid\":%ld,\"duration_seconds\":%u,\"port\":%u,\"listen_error\":%d}\n",
            (long)getpid(), duration, port, listen_error);
    fflush(log);
    fprintf(stdout, "Rest probe log: %s (limit %u seconds, port %u)\n", path, duration, port);
    uint64_t sequence = 0;
    int64_t previous_real = start, previous_mono = milliseconds(CLOCK_MONOTONIC);
    while (!stopped) {
        int64_t real = milliseconds(CLOCK_REALTIME), mono = milliseconds(CLOCK_MONOTONIC);
        if (real < 0 || mono < 0 || real - start >= (int64_t)duration * 1000) break;
        /* Also bound the number of iterations if wall time jumps backwards. */
        if (sequence >= duration) break;
        if (server < 0) {
            server = listener((unsigned short)port);
            listen_error = server < 0 ? errno : 0;
        }
        int standby_bootparam = -1, standby_query_error = 0;
#ifdef BOTTY_PS5
        size_t standby_size = sizeof(standby_bootparam);
        if (sysctlbyname("machdep.bootparams.is_main_on_standby", &standby_bootparam,
                         &standby_size, NULL, 0) != 0) standby_query_error = errno;
#endif
        if (keep_main_enabled && real >= next_keep_request) {
#ifdef BOTTY_PS5
            keep_main_rc = sceSystemStateMgrRequestToKeepMainOnStandby("BottyRestProbe");
#else
            keep_main_rc = -ENOTSUP;
#endif
            fprintf(log, "{\"event\":\"keep_main_request\",\"realtime_ms\":%" PRId64
                         ",\"rc\":%d}\n", real, keep_main_rc);
            next_keep_request = real + 10000;
            if (keep_main_rc != 0) keep_main_enabled = 0;
        }
        int ftp_error = tcp_error(2121), rpc_error = tcp_error(5001);
        char state[768];
        snprintf(state, sizeof(state),
            "{\"sequence\":%" PRIu64 ",\"pid\":%ld,\"realtime_ms\":%" PRId64 ",\"monotonic_ms\":%" PRId64
            ",\"real_gap_ms\":%" PRId64 ",\"mono_gap_ms\":%" PRId64 ",\"ftp_connect_errno\":%d,"
            "\"rtorrent_connect_errno\":%d,\"listen_errno\":%d,\"standby_bootparam\":%d,"
            "\"standby_query_errno\":%d,\"keep_main_requested\":%d,\"keep_main_enabled\":%d,\"keep_main_rc\":%d}\n",
            sequence++, (long)getpid(), real, mono, real - previous_real, mono - previous_mono,
            ftp_error, rpc_error, listen_error, standby_bootparam, standby_query_error,
            keep_main_requested, keep_main_enabled, keep_main_rc);
        fputs(state, log);
        if (fflush(log) != 0) break;
        previous_real = real; previous_mono = mono;
        int64_t next = milliseconds(CLOCK_MONOTONIC) + 1000;
        while (!stopped && milliseconds(CLOCK_MONOTONIC) < next) {
            if (server >= 0) {
                struct pollfd item = {server, POLLIN, 0};
                int ready = poll(&item, 1, 100);
                if (ready > 0) {
                    if (item.revents & POLLIN) serve(server, state);
                    if (item.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                        close(server); server = -1;
                    }
                } else if (ready < 0 && errno != EINTR) {
                    listen_error = errno; close(server); server = -1;
                }
            } else {
                struct timespec pause = {0, 100000000};
                nanosleep(&pause, NULL);
            }
        }
    }
    fprintf(log, "{\"event\":\"stop\",\"realtime_ms\":%" PRId64 ",\"sequence\":%" PRIu64 "}\n",
            milliseconds(CLOCK_REALTIME), sequence);
    if (server >= 0) close(server);
    return fclose(log) == 0 ? 0 : 1;
}
