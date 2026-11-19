#define main fifo_http_main
#include "../nbt_fifo_http.c"
#undef main

#include <pthread.h>
#include <stdlib.h>

static char payload[65536];

static void require(int ok, const char* message)
{
    if (!ok) {
        fprintf(stderr, "FIFO HTTP: %s\n", message);
        exit(1);
    }
}

static void open_pair(int sockets[2])
{
    int size = 4096;
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "open socket pair");
    require(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0, "constrain send buffer");
    require(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0, "set writer nonblocking");
}

static void* read_payload(void* arg)
{
    int fd = *(int*)arg;
    char buf[1024];
    size_t off = 0;
    while (off < sizeof(payload)) {
        ssize_t got = recv(fd, buf, sizeof(buf), 0);
        require(got > 0 && (size_t)got <= sizeof(payload) - off, "read complete stream");
        require(memcmp(buf, payload + off, got) == 0, "preserve stream bytes across partial writes");
        off += got;
    }
    require(recv(fd, buf, 1, 0) == 0, "end stream without extra bytes");
    return NULL;
}

int main(void)
{
    int sockets[2];
    pthread_t reader;
    size_t i;
    long long start;
    alarm(15);
    signal(SIGPIPE, SIG_IGN);
    for (i = 0; i < sizeof(payload); ++i)
        payload[i] = (char)(i % 251);

    open_pair(sockets);
    require(pthread_create(&reader, NULL, read_payload, &sockets[1]) == 0, "start reader");
    require(send_all(sockets[0], payload, sizeof(payload)), "send complete stream");
    close(sockets[0]);
    require(pthread_join(reader, NULL) == 0, "join reader");
    close(sockets[1]);

    open_pair(sockets);
    start = monotonic_ms();
    require(start >= 0, "read clock");
    require(!send_all(sockets[0], payload, sizeof(payload)), "time out when peer does not read");
    require(monotonic_ms() - start >= 1800, "allow time for a stalled peer to resume");
    close(sockets[1]);
    require(!send_all(sockets[0], payload, sizeof(payload)), "fail when peer disconnects");
    close(sockets[0]);
    puts("FIFO HTTP OK: exact stream, stalled peer timeout, and disconnect");
    return 0;
}
