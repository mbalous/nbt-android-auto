/* Exercise local resource-manager attachment and dispatch without a pathname. */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int (*message_handler)(void *, int, unsigned, void *);
typedef int (*attach_fn)(dispatch_t *, void *, int, int, message_handler, void *);
typedef int (*connect_fn)(dispatch_t *, int);
typedef int (*send_fn)(int, const void *, int, void *, int);
typedef int (*reply_fn)(int, int, const void *, int);
typedef int (*read_fn)(int, void *, int, int);
typedef int (*detach_fn)(int);

struct fixture { send_fn send; reply_fn reply; read_fn read; int coid, calls, result; };
struct request { uint16_t type, subtype; uint32_t value; };

static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "%s failed (errno=%d)\n", what, errno); exit(1); }
}

static int handle_message(void *context, int code, unsigned flags, void *arg) {
    struct fixture *f = (struct fixture *)arg;
    struct request message;
    int rcvid;
    uint32_t answer = 0x1234abcd;
    memcpy(&rcvid, context, sizeof(rcvid));
    require(rcvid > 0 && code == 0x5000 && flags == 0, "dispatch callback");
    require(f->read(rcvid, &message, sizeof(message), 0) == sizeof(message) &&
            message.type == 0x5000 && message.subtype == 0 && message.value == 0xdeadbeef,
            "message payload");
    ++f->calls;
    require(f->reply(rcvid, 0, &answer, sizeof(answer)) == 0, "message reply");
    return 0;
}

static void *send_message(void *arg) {
    struct fixture *f = (struct fixture *)arg;
    struct request message = {0x5000, 0, 0xdeadbeef};
    uint32_t reply = 0;
    f->result = f->send(f->coid, &message, sizeof(message), &reply, sizeof(reply));
    require(f->result == 0 && reply == 0x1234abcd, "message roundtrip");
    return NULL;
}

int main(int argc, char **argv) {
    dispatch_t *dp;
    dispatch_context_t *context;
    resmgr_attr_t ra;
    resmgr_connect_funcs_t connect;
    resmgr_io_funcs_t io;
    iofunc_attr_t attr;
    struct fixture f;
    pthread_t thread;
    unsigned long flags;
    char *end;
    int ids[6], i;
    attach_fn attach = (attach_fn)dlsym(RTLD_DEFAULT, "message_attach");
    connect_fn msg_connect = (connect_fn)dlsym(RTLD_DEFAULT, "message_connect");
    detach_fn msg_detach = (detach_fn)dlsym(RTLD_DEFAULT, "ConnectDetach");
    memset(&f, 0, sizeof(f));
    f.send = (send_fn)dlsym(RTLD_DEFAULT, "MsgSend");
    f.reply = (reply_fn)dlsym(RTLD_DEFAULT, "MsgReply");
    f.read = (read_fn)dlsym(RTLD_DEFAULT, "MsgRead");
    alarm(10);
    require(argc == 2, "local flag argument");
    errno = 0;
    flags = strtoul(argv[1], &end, 10);
    require(!errno && !*end && flags && flags <= 0xffffffffUL, "local flag");
    require(attach && msg_connect && msg_detach && f.send && f.reply && f.read, "message symbols");
#ifndef NBT_OPEN_TOOLCHAIN
    require((unsigned)flags == RESMGR_FLAG_ATTACH_LOCAL, "SDP local flag");
    require(offsetof(resmgr_context_t, rcvid) == 0, "receive ID offset");
#endif
    dp = dispatch_create();
    require(dp != NULL, "dispatch_create");
    memset(&ra, 0, sizeof(ra));
    { unsigned value = (unsigned)flags; memcpy(&ra, &value, sizeof(value)); }
    ra.nparts_max = 1;
    ra.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect, _RESMGR_IO_NFUNCS, &io);
    iofunc_attr_init(&attr, S_IFREG | 0555, NULL, NULL);
    for (i = 0; i < 6; ++i) {
        ids[i] = resmgr_attach(dp, &ra, NULL, _FTYPE_ANY, 0, &connect, &io, &attr);
        require(ids[i] != -1, "local resmgr_attach");
    }
    require(attach(dp, NULL, 0x5000, 0x5000, handle_message, &f) == 0, "message_attach");
    context = dispatch_context_alloc(dp);
    require(context != NULL, "dispatch_context_alloc");
    f.coid = msg_connect(dp, 0);
    require(f.coid != -1, "message_connect");
    require(pthread_create(&thread, NULL, send_message, &f) == 0, "sender thread");
    context = dispatch_block(context);
    require(context != NULL && dispatch_handler(context) == 0, "message dispatch");
    require(pthread_join(thread, NULL) == 0 && f.calls == 1, "join sender");
    require(msg_detach(f.coid) == 0, "connection detach");
    dispatch_context_free(context);
    for (i = 0; i < 6; ++i) require(resmgr_detach(dp, ids[i], _RESMGR_DETACH_ALL) == 0, "resmgr_detach");
    require(dispatch_destroy(dp) == 0, "dispatch_destroy");
    alarm(0);
    puts("RESOURCE_MESSAGE_OK local-attaches=6 messages=1 detached=6");
    return 0;
}
