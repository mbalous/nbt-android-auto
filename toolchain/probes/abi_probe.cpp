#include <sys/types.h>
#include <sys/usbdi.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>

#ifdef NBT_OPEN_TOOLCHAIN
#include "qnx_spawn_compat.h"
struct ProbeIov { void *base; unsigned length; };
#else
#include <devctl.h>
#include <spawn.h>
#include <sys/neutrino.h>
typedef iov_t ProbeIov;
#endif

#define SIZE(T) printf("size " #T " %u %u\n", (unsigned)sizeof(T), (unsigned)__alignof__(T))
#define OFFSET(T, F) printf("offset " #T "." #F " %u\n", (unsigned)offsetof(T, F))
#define CONSTANT(C) printf("constant " #C " %lu\n", (unsigned long)(C))

static int initialized;

struct Lifetime {
    Lifetime() { initialized = 1; puts("constructor"); }
    ~Lifetime() { puts("destructor"); }
};
static Lifetime lifetime;

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "ABI_PROBE_FAIL: %s\n", message);
        exit(1);
    }
}

static void *thread_main(void *)
{
    errno = 0;
    int fd = open("abi-probe-file-that-must-not-exist", O_RDONLY);
    require(fd == -1 && errno == ENOENT, "thread libc errno");
    errno = EACCES;
    return NULL;
}

static void check_spawn(char *path)
{
    struct inheritance inherit;
    memset(&inherit, 0, sizeof(inherit));
    inherit.flags = SPAWN_SETGROUP | SPAWN_SETSIGMASK | SPAWN_SETSIGDEF | SPAWN_NOZOMBIE;
    inherit.pgroup = SPAWN_NEWPGROUP;
    require(sigemptyset(&inherit.sigmask) == 0, "spawn empty mask");
    require(sigfillset(&inherit.sigdefault) == 0, "spawn default signals");
    sigset_t parent_mask, previous_mask;
    require(sigemptyset(&parent_mask) == 0 && sigaddset(&parent_mask, SIGUSR1) == 0, "parent signal set");
    require(sigprocmask(SIG_BLOCK, &parent_mask, &previous_mask) == 0, "parent block signal");
    void (*previous_handler)(int) = signal(SIGUSR2, SIG_IGN);
    require(previous_handler != SIG_ERR, "parent ignore signal");
    int channel[2];
    require(pipe(channel) == 0, "spawn pipe");
    int nul = open("/dev/null", O_RDONLY);
    require(nul >= 0, "spawn stdin");
    int fd_map[3] = {nul, channel[1], channel[1]};
    char arg[] = "--spawn-child";
    char *args[] = {path, arg, NULL};
    char *env[] = {NULL};
    require(spawn(path, 3, fd_map, &inherit, args, env) > 0, "spawn child");
    require(close(nul) == 0 && close(channel[1]) == 0, "close spawn descriptors");
    require(signal(SIGUSR2, previous_handler) != SIG_ERR, "restore signal handler");
    require(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0, "restore signal mask");
    char output[512];
    size_t used = 0;
    ssize_t count;
    while ((count = read(channel[0], output + used, sizeof(output) - used - 1)) > 0) {
        used += (size_t)count;
        require(used < sizeof(output) - 1, "spawn child output too long");
    }
    require(count == 0 && close(channel[0]) == 0, "read spawn child");
    output[used] = '\0';
    require(strstr(output, "SPAWN_CHILD_OK") != NULL, "spawn child behavior");
    puts("spawn group, signal reset and fd map OK");
}

int main(int argc, char **argv)
{
    alarm(30);
    require(initialized == 1, "constructor did not run");
    if (argc == 2 && strcmp(argv[1], "--spawn-child") == 0) {
        sigset_t mask;
        require(getpgrp() == getpid(), "spawn process group");
        require(sigprocmask(SIG_SETMASK, NULL, &mask) == 0, "child signal mask");
        require(sigismember(&mask, SIGUSR1) == 0, "child signal unblocked");
        require(signal(SIGUSR2, SIG_DFL) == SIG_DFL, "child signal default");
        puts("SPAWN_CHILD_OK");
        return 0;
    }
    require(argc == 1, "unexpected arguments");
    errno = EBUSY;
    pthread_t thread;
    require(pthread_create(&thread, NULL, thread_main, NULL) == 0, "pthread_create");
    require(pthread_join(thread, NULL) == 0, "pthread_join");
    require(errno == EBUSY, "thread errno changed main errno");
    errno = 0;
    int fd = open("abi-probe-file-that-must-not-exist", O_RDONLY);
    require(fd == -1 && errno == ENOENT, "main libc errno");
    puts("errno threads and libc OK");

    SIZE(usbd_device_ident_t);
    SIZE(usbd_device_instance_t);
    SIZE(usbd_funcs_t);
    SIZE(usbd_connect_parm_t);
    SIZE(usbd_device_descriptor_t);
    SIZE(usbd_configuration_descriptor_t);
    SIZE(usbd_interface_descriptor_t);
    SIZE(usbd_endpoint_descriptor_t);
    SIZE(usbd_descriptors_t);
    OFFSET(usbd_device_instance_t, ident);
    OFFSET(usbd_device_instance_t, config);
    OFFSET(usbd_connect_parm_t, vusb);
    OFFSET(usbd_connect_parm_t, vusbd);
    OFFSET(usbd_connect_parm_t, ident);
    OFFSET(usbd_connect_parm_t, funcs);
    OFFSET(usbd_connect_parm_t, connect_wait);
    OFFSET(usbd_device_descriptor_t, idVendor);
    OFFSET(usbd_configuration_descriptor_t, wTotalLength);
    OFFSET(usbd_endpoint_descriptor_t, wMaxPacketSize);
    OFFSET(usbd_endpoint_descriptor_t, bInterval);
    CONSTANT(USB_VERSION);
    CONSTANT(USBD_VERSION);
    CONSTANT(USB_DESC_DEVICE);
    CONSTANT(USB_DESC_ENDPOINT);
    CONSTANT(URB_DIR_IN);
    CONSTANT(URB_DIR_OUT);
    CONSTANT(URB_SHORT_XFER_OK);
    CONSTANT(USBD_URB_STATUS_MASK);
    CONSTANT(USBD_STATUS_CMP);

    SIZE(sigset_t);
    SIZE(struct inheritance);
    OFFSET(struct inheritance, flags);
    OFFSET(struct inheritance, pgroup);
    OFFSET(struct inheritance, sigmask);
    OFFSET(struct inheritance, sigdefault);
    CONSTANT(SPAWN_SETGROUP);
    CONSTANT(SPAWN_SETSIGMASK);
    CONSTANT(SPAWN_SETSIGDEF);
    CONSTANT(SPAWN_NOZOMBIE);
    CONSTANT(SPAWN_NEWPGROUP);
    check_spawn(argv[0]);

    ProbeIov iov;
    memset(&iov, 0, sizeof(iov));
#ifdef NBT_OPEN_TOOLCHAIN
    iov.base = reinterpret_cast<void *>(0x12345678);
    iov.length = 0x11223344;
#else
    SETIOV(&iov, reinterpret_cast<void *>(0x12345678), 0x11223344);
#endif
    SIZE(ProbeIov);
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&iov);
    printf("iov bytes");
    for (size_t i = 0; i < sizeof(iov); ++i) printf(" %02x", bytes[i]);
    putchar('\n');

    typedef int (*Devctlv)(int, int, int, int, const ProbeIov *, const ProbeIov *, int *);
    Devctlv fn = reinterpret_cast<Devctlv>(dlsym(RTLD_DEFAULT, "devctlv"));
    require(fn != NULL, "devctlv lookup");
#ifndef NBT_OPEN_TOOLCHAIN
    Devctlv direct = &devctlv;
    require(direct == fn, "devctlv direct and dynamic symbols differ");
#endif
    int info = 0;
    errno = 0;
    int result = fn(-1, 0, 0, 0, NULL, NULL, &info);
    require(result != 0, "devctlv accepted invalid fd");
    printf("devctlv invalid-fd result %d errno %d info %d\n", result, errno, info);
    puts("ABI_PROBE_MAIN_OK");
    return 0;
}
