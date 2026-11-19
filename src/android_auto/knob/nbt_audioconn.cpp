// nbt_audioconn - an `audioconn`-compatible route tool for the ORIGINAL BMW HU_NBT, which has no audioconn.
//
// NBT-AAD's BrowserAudio (oneplus_native, browser audio mode) runs `audioconn -b 0x<conn>` when Android Auto starts
// playing and `audioconn -r 0x<conn>` after silence; exit status 0 = granted. On NBT Evo audioconn is a stock program.
// Here the same request is sent the way HmiMain3D itself sends it (offline analysis 2026-11-15, docs/AA-AUDIO-PLAN.md):
//
//   ConnectionMaster FBlock 0x03 (address and instance from the DSYSMOSTServer registry)
//     Fkt 0x053 BuildHighSyncConnection   OpType 0x6 StartResultAck   payload big-endian { u16 SenderHandle, u16 ConnID }
//     Fkt 0x054 RemoveHighSyncConnection  OpType 0x6 StartResultAck   same payload
//     reply: same Fkt, OpType 0xD ResultAck { u16 SenderHandle } = granted; OpType 0x9 ErrorAck / 0xF Error = refused
//     Fkt 0xC03 HighSyncConnectionTable   OpType 0x1 Get (read-only, used by -l)
//   HmiMain3D: proxy ctor 0x8b838fc, request builders 0x8b832e6 (053) / 0x8b83258 (054), table EAudioSource -> ConnID at
//   0x9879224 (src 0x53 -> 0xB8 front, 0x54 -> 0xB9, 0x55 -> 0xBA), getConnIDForAudioSource 0x899c068.
//
// Connection 0xB8 is the browser audio connection (NBT Evo: BrowserFront, A2I input_stereo_3). It is an interrupting
// source: while it is built the radio / Bluetooth source is paused. native_service therefore removes it after every
// session even when oneplus_native died without doing so (route state file, see below).
//
// usage: nbt_audioconn -b 0x<conn>      build    exit 0 granted, 1 refused, 3 no answer, 4 no ConnectionMaster / link
//        nbt_audioconn -r 0x<conn>      remove   same exit codes
//        nbt_audioconn -l               print the ConnectionMaster's connection table (Get 0xC03, read-only)
// Only the browser connections 0xB8, 0xB9 and 0xBA are accepted (exit 2 otherwise): this tool cannot be used to switch
// arbitrary car audio connections. NBT_AUDIOCONN_ANY=1 lifts that limit for investigation.
//
// State files in the runtime directory (RAM, gone after a reboot):
//   audio-route.built    a build may have been sent and no remove has been granted since
//   audio-route.failed   the last build was refused or not answered: native_service keeps Bluetooth audio for the next
//                        sessions until the next reboot (or until the file is removed)
// Needs no LD_LIBRARY_PATH: the DSI library is opened by absolute path (ref/dsi_client.cpp).
#include "most/client.hpp"
#include "most/connmaster.hpp"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

namespace cm = nbt::most::connmaster;
#ifndef NBT_AUDIO_RUNTIME_DIR
#define NBT_AUDIO_RUNTIME_DIR "/fs/ram3/nbt-aa-native"
#endif
const char* const kRuntimeDir = NBT_AUDIO_RUNTIME_DIR;
const uint8_t kFBlockConnMaster = cm::kFBlock;
const uint16_t kFktBuild = cm::kFktBuild, kFktRemove = cm::kFktRemove, kFktTable = cm::kFktTable;

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

long long nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// ---- registry: where the ConnectionMaster lives ------------------------------------------------------------------
pthread_mutex_t g_stateLock = PTHREAD_MUTEX_INITIALIZER;
unsigned g_cmAddr = 0, g_cmInst = 0, g_regSeen = 0;
void onInfo(uint32_t id, const uint8_t* p, uint32_t len, void*) {
    if (id != nbt::most::kAttrRegistry || len < 4) return;
    uint32_t n = nbt::most::rdLe32(p);
    unsigned addr = 0, inst = 0;
    for (uint32_t i = 0; i < n && 4 + i * 4 + 4 <= len; ++i) {
        unsigned a = nbt::most::rdLe16(p + 4 + i * 4), key = nbt::most::rdLe16(p + 6 + i * 4);
        if ((key >> 8) == kFBlockConnMaster && !addr) { addr = a; inst = key & 0xff; }
    }
    pthread_mutex_lock(&g_stateLock);
    g_cmAddr = addr;
    g_cmInst = inst;
    g_regSeen = 1;
    pthread_mutex_unlock(&g_stateLock);
}

// ---- replies from FBlock 03 to us --------------------------------------------------------------------------------
unsigned g_myDev = 0;
int g_wantFkt = -1, g_wantHandle = -1;
int g_result = 0;          // 0 waiting, 1 ResultAck, -1 error
uint8_t g_errBytes[8];
unsigned g_errLen = 0;
bool g_list = false;

void onMsg(const nbt::most::View& m, nbt::most::MsgClass, void*) {
    if (m.fblock != kFBlockConnMaster) return;
    pthread_mutex_lock(&g_stateLock);
    if (g_list && m.fkt == kFktTable && m.op == 0xC) {
        printf("table status (%u bytes):", (unsigned)m.len);
        for (size_t i = 0; i < m.len && i < 200; ++i) printf("%s%02x", i % 5 ? " " : "  |", m.data[i]);
        printf("\n");
        if (!g_myDev || m.dst == g_myDev) g_result = 1;
        pthread_mutex_unlock(&g_stateLock);
        return;
    }
    if ((g_myDev && m.dst != g_myDev) || (int)m.fkt != g_wantFkt) {
        pthread_mutex_unlock(&g_stateLock);
        return;
    }
    if (m.op == cm::kOpResultAck) {
        int h = cm::resultAckHandle(m.data, m.len);
        if (g_wantHandle >= 0 && h != g_wantHandle) {
            printf("ResultAck for another handle 0x%04x (ours 0x%04x): ignored\n", (unsigned)h, (unsigned)g_wantHandle);
            pthread_mutex_unlock(&g_stateLock);
            return;
        }
        g_result = 1;
    } else if (m.op == cm::kOpErrorAck || m.op == cm::kOpError) {
        g_errLen = m.len < sizeof(g_errBytes) ? (unsigned)m.len : (unsigned)sizeof(g_errBytes);
        memcpy(g_errBytes, m.data, g_errLen);
        g_result = -1;
    } else {
        printf("reply fkt=%03X op=%X len=%u (not final)\n", m.fkt, m.op, (unsigned)m.len);
    }
    pthread_mutex_unlock(&g_stateLock);
}

int result() {
    pthread_mutex_lock(&g_stateLock);
    int value = g_result;
    pthread_mutex_unlock(&g_stateLock);
    return value;
}

bool stateFile(const char* name, bool present, const char* text) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", kRuntimeDir, name);
    if (!present) {
        if (unlink(path) == 0 || errno == ENOENT) return true;
        perror(path);
        return false;
    }
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)getpid());
    FILE* f = fopen(tmp, "w");
    if (!f) { perror(tmp); return false; }
    bool ok = fprintf(f, "%s\n", text) >= 0;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) == 0) return true;
    perror("audio route state");
    unlink(tmp);
    return false;
}

bool lockRoute() {
    char path[96];
    snprintf(path, sizeof(path), "%s/audio-route.lock", kRuntimeDir);
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror(path); return false; }
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLKW, &lock) == 0) return true;
    perror("audio route lock");
    close(fd);
    return false;
}

bool compatiblePending(unsigned long conn) {
    char path[96], line[32], *end;
    snprintf(path, sizeof(path), "%s/audio-route.built", kRuntimeDir);
    FILE* f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return true;
        perror(path);
        return false;
    }
    bool ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok) return false;
    unsigned long pending = strtoul(line, &end, 0);
    return end != line && (*end == '\n' || *end == '\0') && pending == conn;
}

int usage(const char* me) {
    fprintf(stderr, "usage: %s -b 0x<conn> | -r 0x<conn> | -l\n", me);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    char op = 0;
    unsigned long conn = 0;
    if (argc == 2 && !strcmp(argv[1], "-l")) op = 'l';
    else if (argc == 3 && (!strcmp(argv[1], "-b") || !strcmp(argv[1], "-r"))) {
        char* end = 0;
        op = argv[1][1];
        conn = strtoul(argv[2], &end, 0);
        if (end == argv[2] || *end || conn == 0 || conn > 0xffff) return usage(argv[0]);
    } else return usage(argv[0]);
    const char* any = getenv("NBT_AUDIOCONN_ANY");
    if (op != 'l' && !(any && !strcmp(any, "1")) && !cm::isBrowserConn(conn)) {
        fprintf(stderr, "connection 0x%lx refused: only the browser connections 0xb8..0xba are allowed\n", conn);
        return 2;
    }
    g_list = op == 'l';
    signal(SIGTERM, onSignal); signal(SIGINT, onSignal); signal(SIGPIPE, SIG_IGN);
    if (op != 'l' && !lockRoute()) {
        if (op == 'b') stateFile("audio-route.failed", true, "cannot lock audio route");
        return 4;
    }
    if (op == 'b' && !compatiblePending(conn)) {
        fprintf(stderr, "a different or invalid pending audio route must be cleaned up first\n");
        stateFile("audio-route.failed", true, "pending audio route");
        return 4;
    }

    static nbt::most::Client most;
    if (!most.setTap(&onMsg, NULL) || !most.setInfoTap(&onInfo, NULL)) return 4;
    if (!most.start("NBTPROJ_AUDIOCONN")) {
        if (op == 'b') stateFile("audio-route.failed", true, "MOST client could not start");
        return 4;
    }
    for (int w = 0; w < 50 && most.state() != nbt::most::kUp && !g_stop; ++w) usleep(100000);
    if (most.state() != nbt::most::kUp) {
        printf("MOST link not up: nothing sent\n");
        if (op == 'b') stateFile("audio-route.failed", true, "MOST link not up");
        fflush(stdout); _exit(4);
    }
    unsigned cmAddr = 0, cmInst = 0, regSeen = 0;
    for (int w = 0; w < 30 && !g_stop; ++w) {
        pthread_mutex_lock(&g_stateLock);
        cmAddr = g_cmAddr; cmInst = g_cmInst; regSeen = g_regSeen;
        pthread_mutex_unlock(&g_stateLock);
        if (regSeen) break;
        usleep(100000);
    }
    if (!cmAddr) {
        printf("ConnectionMaster FBlock 03 not in the MOST registry%s: nothing sent\n", regSeen ? "" : " (no registry)");
        if (op == 'b') stateFile("audio-route.failed", true, "FBlock 03 not in registry");
        fflush(stdout); _exit(4);
    }
    unsigned myDev = most.deviceId();
    pthread_mutex_lock(&g_stateLock);
    g_myDev = myDev;
    if (op == 'l') g_wantFkt = kFktTable;
    pthread_mutex_unlock(&g_stateLock);
    printf("ConnectionMaster 03/%02X at 0x%04X, own device 0x%04X\n", cmInst, cmAddr, myDev);

    if (op == 'l') {
        most.send(cm::tableGet((uint16_t)cmAddr, (uint8_t)cmInst));
        for (long long t = nowMs() + 2000; nowMs() < t && !result() && !g_stop;) usleep(50000);
        fflush(stdout);
        _exit(result() ? 0 : 3);
    }

    const uint16_t fkt = op == 'b' ? kFktBuild : kFktRemove;
    const unsigned handle = 0x4E00u | (unsigned)(getpid() & 0xff);   // own range, away from the HMI's 0xB1xx / counters
    pthread_mutex_lock(&g_stateLock);
    g_wantFkt = fkt;
    g_wantHandle = (int)handle;
    pthread_mutex_unlock(&g_stateLock);
    printf("%s connection 0x%lx (fkt %03X, handle 0x%04X)\n", op == 'b' ? "build" : "remove", conn, fkt, handle);
    if (op == 'b' && !stateFile("audio-route.built", true, argv[2])) {
        stateFile("audio-route.failed", true, "cannot record pending route");
        fflush(stdout); _exit(4);
    }
    if (!most.send(cm::connectionRequest(op == 'b', (uint16_t)cmAddr, (uint8_t)cmInst, (uint16_t)handle,
                                         (uint16_t)conn))) {
        if (op == 'b') stateFile("audio-route.failed", true, "send failed");
        fflush(stdout); _exit(4);
    }
    for (long long t = nowMs() + 3000; nowMs() < t && !result() && !g_stop;) usleep(50000);

    char why[64];
    int rc;
    pthread_mutex_lock(&g_stateLock);
    if (g_result == 1) {
        printf("granted\n");
        rc = 0;
    } else if (g_result == -1) {
        int n = snprintf(why, sizeof(why), "refused:");
        for (unsigned i = 0; i < g_errLen && n < (int)sizeof(why) - 4; ++i) n += snprintf(why + n, sizeof(why) - n, " %02x", g_errBytes[i]);
        printf("%s\n", why);
        rc = 1;
    } else {
        snprintf(why, sizeof(why), "no answer within 3 s");
        printf("%s\n", why);
        rc = 3;
    }
    pthread_mutex_unlock(&g_stateLock);
    if (op == 'b') {
        if (rc == 0) stateFile("audio-route.failed", false, 0);
        else stateFile("audio-route.failed", true, why);
    } else if (rc == 0 && compatiblePending(conn)) {
        stateFile("audio-route.built", false, 0);
    }
    fflush(stdout);
    _exit(rc);
}
