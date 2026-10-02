/* Short-lived, file-backed diagnostic bridge to console-local SCGI.
 * No network listener. Exits after one hour or when its stop file appears.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

static int send_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t sent = send(fd, p, n, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        p += sent; n -= sent;
    }
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    if (chdir("/data/botty/rtorrent-probe")) return 1;
    int lock = open("rpc.lock", O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 1;
    FILE* ready = fopen("rpc.pid", "w");
    if (!ready) return 1;
    fprintf(ready, "%d\n", getpid()); fclose(ready);
    time_t start = time(NULL);
    char *body = malloc(1024 * 1024);
    if (!body) return 1;
    while (time(NULL) - start < 3600 && access("rpc.stop", F_OK)) {
        int input = open("request.json", O_RDONLY);
        if (input < 0) { usleep(200000); continue; }
        struct stat info;
        if (fstat(input, &info) || info.st_size <= 0 || info.st_size > 1024 * 1024) { close(input); break; }
        size_t length = (size_t)info.st_size, have = 0;
        while (have < length) {
            ssize_t n = read(input, body + have, length - have);
            if (n <= 0) break;
            have += n;
        }
        close(input);
        if (have != length) break;
        int output = open("response.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0) break;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct timeval timeout = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET; addr.sin_port = htons(5001);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        char header[256], prefix[32];
        int size = snprintf(header, sizeof(header), "CONTENT_LENGTH%c%zu%cSCGI%c1%cCONTENT_TYPE%capplication/json%c", 0, length, 0, 0, 0, 0, 0);
        int digits = snprintf(prefix, sizeof(prefix), "%d:", size);
        if (fd < 0 || connect(fd, (struct sockaddr*)&addr, sizeof(addr)) ||
            send_all(fd, prefix, digits) || send_all(fd, header, size) ||
            send_all(fd, ",", 1) || send_all(fd, body, length)) {
            const char* error = "SCGI transport unavailable\n";
            write(output, error, strlen(error));
        } else {
            char buffer[16384];
            size_t total = 0;
            while (total < 4 * 1024 * 1024) {
                ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                if (write(output, buffer, n) != n) break;
                total += n;
            }
        }
        if (fd >= 0) close(fd);
        close(output);
        // Remove request before publishing response, allowing a sequential caller
        // to submit its next command as soon as it sees the completed response.
        unlink("request.json");
        if (rename("response.tmp", "response.txt")) break;
    }
    free(body);
    close(lock);
    return 0;
}
