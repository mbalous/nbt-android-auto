/* fifo_http: single-client loopback HTTP server that streams H.264 from a FIFO.
 *
 * The stock Cinemo player (started by decode_runner) cannot read a FIFO directly, but it can play an HTTP URL. This
 * program bridges the two: it waits for exactly one client on 127.0.0.1:18765, answers its request with an endless
 * "video/h264" response and then copies everything that arrives in the FIFO to that client.
 *
 * Usage: fifo_http <h264 fifo> <ready file>
 *   <ready file> is created (must not exist yet) once the client is connected, so the service knows the player is
 *   reading.
 *
 * Exit codes: 0 = stopped normally, 2 = bad arguments or setup error, 3 = client error.
 * Buffering is bounded: data is read in 64 KiB chunks and a send that cannot make progress for 2 s ends the stream
 * (backpressure instead of unbounded memory).
 */
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HTTP_PORT 18765

static volatile sig_atomic_t stop;

static void stopped(int signal_number)
{
    (void)signal_number;
    stop = 1;
}

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        perror("fifo_http: clock_gettime");
        return -1;
    }
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Send all n bytes; returns 1 on success, 0 if the client is gone or stalls for 2 s, or we are stopping. */
static int send_all(int fd, const char* data, unsigned n)
{
    long long deadline = 0;
    while (n && !stop) {
        ssize_t sent = send(fd, data, n, 0);
        fd_set set;
        struct timeval timeout;
        long long now, remaining;
        int ready;
        if (sent > 0) {
            data += sent;
            n -= sent;
            deadline = 0;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (!sent || (errno != EAGAIN && errno != EWOULDBLOCK) || stop)
            return 0;

        now = monotonic_ms();
        if (now < 0)
            return 0;
        if (!deadline)
            deadline = now + 2000;
        remaining = deadline - now;
        if (remaining <= 0)
            return 0;
        timeout.tv_sec = remaining / 1000;
        timeout.tv_usec = (remaining % 1000) * 1000;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        ready = select(fd + 1, 0, &set, 0, &timeout);
        if (ready == 0 || (ready < 0 && errno != EINTR))
            return 0;
    }
    return !n;
}

int main(int argc, char** argv)
{
    int fifo, server, client, flags, one = 1, used = 0;
    char request[4096], data[65536];
    sockaddr_in addr = {};
    const char* header = "HTTP/1.0 200 OK\r\nContent-Type: video/h264\r\nConnection: close\r\n\r\n";

    if (argc != 3)
        return 2;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGPIPE, SIG_IGN);

    /* O_RDWR keeps the FIFO open even while no writer is attached, so reads never see EOF between sessions */
    fifo = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fifo < 0)
        return 2;

    /* listen on loopback only, accept exactly one client, then stop listening */
    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
        return 2;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) || listen(server, 1))
        return 2;
    client = accept(server, 0, 0);
    close(server);
    if (client < 0)
        return 3;
    flags = fcntl(client, F_GETFL);
    if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fifo_http: set client nonblocking");
        close(client);
        return 3;
    }

    /* read the HTTP request header (whatever it asks for, the answer is the stream); 5 s timeout per read */
    while (used < (int)sizeof(request) - 1 && !stop) {
        fd_set set;
        struct timeval timeout = {5, 0};
        int n;
        FD_ZERO(&set);
        FD_SET(client, &set);
        if (select(client + 1, &set, 0, 0, &timeout) <= 0)
            return 3;
        n = recv(client, request + used, sizeof(request) - 1 - used, 0);
        if (n <= 0)
            return 3;
        used += n;
        request[used] = 0;
        if (strstr(request, "\r\n\r\n"))
            break;
    }
    if (!strstr(request, "\r\n\r\n") || !send_all(client, header, strlen(header)))
        return 3;

    /* tell the service that the player is connected */
    {
        int ready = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (ready < 0)
            return 3;
        write(ready, "ready\n", 6);
        close(ready);
    }

    /* copy FIFO -> client until stopped or the client goes away */
    while (!stop) {
        fd_set set;
        struct timeval timeout = {0, 200000};
        int n;
        FD_ZERO(&set);
        FD_SET(fifo, &set);
        if (select(fifo + 1, &set, 0, 0, &timeout) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (!FD_ISSET(fifo, &set))
            continue;
        n = read(fifo, data, sizeof(data));
        if (n > 0 && !send_all(client, data, n))
            break;
    }
    close(client);
    close(fifo);
    return 0;
}
