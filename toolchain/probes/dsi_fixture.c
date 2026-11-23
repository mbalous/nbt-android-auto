/* Synthetic DSI library for exercising the project's dynamically resolved ABI. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *client;
static unsigned char written[32];
static unsigned used;
static int notified, constructed;
void *dsi_vtable[34] __asm__("_ZTV10CDSIClient");

static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "DSI fixture: %s\n", what); exit(1); }
}

void conn_ctor(void *) __asm__("_ZN14CDSIConnectionC1Ev");
void conn_ctor(void *p) { require(p != NULL, "connection"); }
void cli_ctor(void *, const char *, const char *, int, int) __asm__("_ZN10CDSIClientC1EPKcS1_ii");
void cli_ctor(void *p, const char *iface, const char *role, int major, int minor) {
    int fd = 7;
    require(p && !strcmp(iface, "TestInterface") && !strcmp(role, "TestRole") && major == 1 && minor == 2,
            "client constructor");
    client = (char *)p;
    memcpy(client + 0x140, &fd, sizeof(fd));
}
int conn_add(void *, void *) __asm__("_ZN14CDSIConnection3addER10CDSIClient");
int conn_add(void *conn, void *p) { require(conn && p == client, "add client"); return 0; }
void set_notification(void *, unsigned) __asm__("_ZN10CDSIClient15setNotificationEj");
void set_notification(void *p, unsigned value) {
    require(p == client && value == 0x102, "notification"); ++notified;
}

static void check_ctor(void *stream, void *chan, void *tcp, unsigned type, unsigned command, unsigned id,
                       const void *source, const void *target, int tail) {
    require(stream && chan == client + 0x130 && tcp == client + 0x138 &&
            type == 0x100 && command == 7 && id == 0x1357 &&
            source == client + 0x110 && target == client + 0x118 && tail == 0, "stream constructor");
    used = 0;
    ++constructed;
}
#ifdef OLD_DSI
void stream_ctor(void *, void *, void *, unsigned, unsigned, unsigned, const void *, const void *, int)
    __asm__("_ZN11CDSIOStreamC1ER11CDSIChannelR13CDSITCPSocketN3DSI11RequestTypeENS4_7CommandEjRK8SPartyIDS9_i");
void stream_ctor(void *s, void *c, void *t, unsigned ty, unsigned cmd, unsigned id,
                 const void *src, const void *dst, int tail) {
    check_ctor(s, c, t, ty, cmd, id, src, dst, tail);
}
void stream_write_int(void *, const int *) __asm__("_ZN11CDSIOStream5writeIiEEvRKT_");
void stream_write_int(void *s, const int *value) {
    require(s && value && used + 4 <= sizeof(written), "word write");
    memcpy(written + used, value, 4); used += 4;
}
#else
void stream_ctor(void *, void *, void *, int, unsigned, unsigned, unsigned, const void *, const void *, int)
    __asm__("_ZN11CDSIOStreamC1ER11CDSIChannelP13CDSITCPSocketiN3DSI11RequestTypeENS4_7CommandEjRK8SPartyIDS9_i");
void stream_ctor(void *s, void *c, void *t, int fd, unsigned ty, unsigned cmd, unsigned id,
                 const void *src, const void *dst, int tail) {
    require(fd == 7, "IPC descriptor");
    check_ctor(s, c, t, ty, cmd, id, src, dst, tail);
}
void stream_write(void *, const void *, unsigned) __asm__("_ZN11CDSIOStream5writeEPKvj");
void stream_write(void *s, const void *value, unsigned size) {
    require(s && value && used + size <= sizeof(written), "byte write");
    memcpy(written + used, value, size); used += size;
}
#endif
int stream_send(void *, int) __asm__("_ZN11CDSIOStream4sendEi");
int stream_send(void *s, int flags) {
    static const unsigned char expected[] = {0x44, 0x33, 0x22, 0x11, 4, 0, 0, 0, 'a', 'b', 'c', 0};
    require(s && flags == -1 && used == sizeof(expected) && !memcmp(written, expected, sizeof(expected)),
            "serialized request");
    return 0;
}
void stream_dtor(void *) __asm__("_ZN11CDSIOStreamD1Ev");
void stream_dtor(void *s) { require(s && constructed == 1, "stream destructor"); }

void conn_start(void *) __asm__("_ZN14CDSIConnection5startEv");
void conn_start(void *conn) {
    union { uint64_t align; unsigned char input[0x50]; } storage;
    static const uint8_t payload[] = {0x11, 0x22, 0x33};
    const uint8_t *pointer = payload;
    uint32_t len = sizeof(payload), type = 0x100, id = 0x2468, seq = 9;
    void **vtable;
    require(conn && client, "start");
    vtable = *(void ***)client;
    ((void (*)(void *))vtable[3])(client);
    require(notified == 1 && constructed == 1, "attached callback");
    memset(&storage, 0, sizeof(storage));
    memcpy(storage.input + 0x08, &len, sizeof(len)); memcpy(storage.input + 0x10, &pointer, sizeof(pointer));
    memcpy(storage.input + 0x44, &type, sizeof(type)); memcpy(storage.input + 0x48, &id, sizeof(id));
    memcpy(storage.input + 0x4c, &seq, sizeof(seq));
    ((void (*)(void *, void *))vtable[7])(client, storage.input);
}
