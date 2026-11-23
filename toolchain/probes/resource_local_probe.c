/* Exercise resource-manager setup without registering a pathname. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    dispatch_t *dp;
    dispatch_context_t *context, *received;
    resmgr_attr_t ra;
    resmgr_connect_funcs_t connect;
    resmgr_io_funcs_t io;
    iofunc_attr_t attr;
    struct timespec tick = {0, 100000000L}, before, after;
    unsigned flags;
    int id, result = 1, i, handler_result, handler_errno;

    alarm(5);
#ifdef NBT_OPEN_TOOLCHAIN
    {
        char *end;
        unsigned long value;
        if (argc != 2) return 2;
        errno = 0;
        value = strtoul(argv[1], &end, 10);
        if (errno || *end || !value || value > 0xffffffffUL) return 2;
        flags = (unsigned)value;
    }
#else
    (void)argc;
    (void)argv;
    flags = RESMGR_FLAG_ATTACH_LOCAL;
#endif
    dp = dispatch_create();
    if (!dp) {
        perror("dispatch_create");
        return 2;
    }
    memset(&ra, 0, sizeof(ra));
    /* The open subset leaves this field opaque; the flag is measured by SDP. */
    memcpy(&ra, &flags, sizeof(flags));
    ra.nparts_max = 1;
    ra.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect, _RESMGR_IO_NFUNCS, &io);
    iofunc_attr_init(&attr, S_IFREG | 0555, NULL, NULL);
    id = resmgr_attach(dp, &ra, NULL, _FTYPE_ANY, 0, &connect, &io, &attr);
    if (id == -1) {
        perror("resmgr_attach");
        goto destroy;
    }
    context = dispatch_context_alloc(dp);
    if (!context) {
        perror("dispatch_context_alloc");
        goto detach;
    }
    if (dispatch_timeout(dp, &tick) != 0) {
        perror("dispatch_timeout");
        goto free_context;
    }
    for (i = 0; i < 3; ++i) {
        if (clock_gettime(CLOCK_MONOTONIC, &before) != 0) {
            perror("clock_gettime");
            goto free_context;
        }
        received = dispatch_block(context);
        if (!received) {
            perror("dispatch_block");
            goto free_context;
        }
        context = received;
        if (clock_gettime(CLOCK_MONOTONIC, &after) != 0) {
            perror("clock_gettime");
            goto free_context;
        }
        if ((after.tv_sec - before.tv_sec) +
            (after.tv_nsec - before.tv_nsec) / 1e9 < 0.05) {
            fputs("dispatch_block returned before the timeout\n", stderr);
            goto free_context;
        }
        errno = 0;
        handler_result = dispatch_handler(context);
        handler_errno = errno;
        printf("timeout %d handler=%d errno=%d\n", i + 1, handler_result, handler_errno);
    }
    printf("local-only flag=%u attached timeouts=3\n", flags);
    result = 0;
free_context:
    dispatch_context_free(context);
detach:
    if (resmgr_detach(dp, id, _RESMGR_DETACH_ALL) != 0) {
        perror("resmgr_detach");
        result = 1;
    }
destroy:
    if (dispatch_destroy(dp) != 0) {
        perror("dispatch_destroy");
        result = 1;
    }
    alarm(0);
    return result;
}
