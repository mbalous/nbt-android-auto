/* native_service: the supervisor of Android Auto on the NBT (installed as NbtAndroidAutoRT/native_service).
 *
 * Started by boot.sh in /fs/ram3/nbt-aa-native (RAM). It owns only its own child processes and RAM files; the stock
 * io-nme media service and the HMI are never signalled.
 *
 * What it does, in a loop for as long as it runs:
 *   1. find exactly one phone on USB (find_phone)
 *   2. switch it to Android Open Accessory mode   (nbt_android_auto --switch-aa <bus> <device>)
 *   3. start the video chain for one session:
 *        fifo_http      serves annexb.fifo (H.264 from the phone) on http://127.0.0.1:18765
 *        decode_runner  plays that URL with the stock Cinemo player into the stock video window
 *        nbt_hwvideo    asks VideoApp (MOST) to show that window in front of iDrive
 *   4. run the Android Auto session                (nbt_android_auto --capture-video <bus> <device>)
 *   5. when the session ends, stop the chain and start over.
 * nbt_knob (iDrive controller) and accessory_window (keeps the stock USB stack away from the phone) run for the whole
 * service life.
 *
 * Behaviour switches are files in the runtime directory (written by boot.sh / the HMI plugin):
 *   hw.mode     hardware video path (the normal mode today)
 *   center800   phone canvas 800x480 1:1 in the middle of the display      split800  same, at the left edge
 *   anamorph    phone canvas 800x480 stretched 1.6x horizontally ("wide")  res720    1280x720 canvas, cropped
 *   gate.mode   only touch the phone after the HMI page was opened
 *   bt-audio    Bluetooth sound instead of Android Auto sound
 *   aa-audio    browser audio route instead of the default f6 media source (optional connection id)
 *   prompts-off disable prompts over other car sources in f6 mode
 *   peer.pin    pin the phone's TLS certificate instead of checking the chain
 *   shadow.path, wal.args   old experimental modes (see experiments/), not used by the installed package
 *
 * Status for the HMI: status.txt (private) and /fs/ram3/nbt-aa-state (world readable), see report().
 * Exit status: 2 = wrong arguments, not root, runtime directory missing or service already running; otherwise 0.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "service_lock.h"

#define RUNTIME_DIR "/fs/ram3/nbt-aa-native"
/* the package folder on flash: switch and config files the user creates live here (native_service itself runs in
 * RUNTIME_DIR, so relative names there would never see them) */
#define PKG_DIR "/var/opt/hmi/data/rw/NbtAndroidAutoRT/"
#define HMI_STATE_FILE "/fs/ram3/nbt-aa-state"
#define HMI_REQUEST_FILE "/fs/ram3/nbt-aa-request"
#define CINEMO_LIBRARY_PATH "/opt/mm/lib/cinemo:/opt/mm/lib:/lib:/usr/lib"
#define SYSTEM_LIBRARY_PATH "/opt/sys/lib:/lib:/usr/lib"

static volatile sig_atomic_t stopping;

static void stopped(int signal_number)
{
    stopping = signal_number;
}

static long long ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Default to the f6 main source and mediaout mixer. aa-audio selects the alternative BrowserAudio route;
 * bt-audio overrides both. A failed output or pending browser route falls back on the next session.
 * The knob reads its environment only at startup; its mode is tracked so it can be restarted between sessions. */
static int audio_knob_mode = -1;
static void set_audio_mode(void)
{
    static int saved;
    static char *play, *nav, *pause, *wheel, *media, *prompt, *prompt_conn;
    FILE* f;
    char line[32] = "";
    char* end = 0;
    unsigned long conn = 0xb8;
    int explicit_file = 0;
    const char* why = 0;
    if (!saved) {
#define SAVE_AUDIO_ENV(var, name) do { const char* v = getenv(name); if (v) var = strdup(v); } while (0)
        SAVE_AUDIO_ENV(play, "NBT_PLAY_AUX");
        SAVE_AUDIO_ENV(nav, "NBT_NAV_AUDIO");
        SAVE_AUDIO_ENV(pause, "NBT_AUX_PAUSE");
        SAVE_AUDIO_ENV(wheel, "NBT_WHEEL_AUX_ONLY");
        SAVE_AUDIO_ENV(media, "NBT_AA_MEDIA_OUT");
        SAVE_AUDIO_ENV(prompt, "NBT_AA_PROMPT_OUT");
        SAVE_AUDIO_ENV(prompt_conn, "NBT_PROMPT_CONN");
#undef SAVE_AUDIO_ENV
        saved = 1;
    }
    if (!access("bt-audio", F_OK))
        why = "bt-audio switch";
    else if (!access("audio-output.failed", F_OK))
        why = "audio output failed earlier (audio-output.failed)";
    else if (!access("audio-route.built", F_OK))
        why = "audio route cleanup pending";
    f = why ? 0 : fopen("aa-audio", "r");
    if (f) {
        explicit_file = 1;
        if (fgets(line, sizeof(line), f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p && *p != '\n' && *p != '\r') {
                unsigned long v = strtoul(p, &end, 0);
                /* only 0 (unrouted) or a browser connection 0xb8..0xba: anything else (e.g. 0xf6 left behind by the
                 * closed v0.9.0 package) is refused by audioconn, which marked the route failed and silently moved
                 * every later session to Bluetooth */
                if (end != p && (!*end || *end == '\n' || *end == '\r')) {
                    if (v == 0 || (v >= 0xb8 && v <= 0xba))
                        conn = v;
                    else
                        printf("audio: aa-audio connection 0x%lx ignored (only 0xb8..0xba), using 0xb8\n", v);
                }
            }
        }
        fclose(f);
    }
    if (!why && explicit_file && !access("audio-route.failed", F_OK))
        why = "audio route failed earlier (audio-route.failed)";
    unsetenv("NBT_AA_MEDIA_OUT");
    unsetenv("NBT_AA_PROMPT_OUT");
    unsetenv("NBT_AUDIOCONN");
    unsetenv("NBT_GUIDANCE_BT");
    if (!why && !explicit_file) {
        int prompts = access("prompts-off", F_OK) && access("audio-route.failed", F_OK) &&
                      !access("audioconn", X_OK) && (!prompt_conn || strtoul(prompt_conn, 0, 16));
        printf("audio: Android Auto main source f6\n");
        setenv("NBT_AUDIO_MODE", "bt", 1); /* mediaout, not BrowserAudio */
        setenv("NBT_SDR_DROP", "2,4", 1);
        setenv("NBT_AA_MEDIA_OUT", media && *media ? media : "input_stereo_1", 1);
        setenv("NBT_GUIDANCE_BT", "0", 1);
        if (prompts)
            setenv("NBT_AA_PROMPT_OUT", prompt && *prompt ? prompt : "input_stereo_3", 1);
        else
            printf("audio: prompts over other sources disabled\n");
        setenv("NBT_PROMPT_CONN", prompts ? (prompt_conn ? prompt_conn : "b8") : "0", 1);
        setenv("NBT_AUX_SOURCE", "f6", 1);
        setenv("NBT_PLAY_AUX", play ? play : "1", 1);
        setenv("NBT_NAV_AUDIO", nav ? nav : "1", 1);
        setenv("NBT_AUX_PAUSE", pause ? pause : "1", 1);
        setenv("NBT_WHEEL_AUX_ONLY", wheel ? wheel : "1", 1);
        audio_knob_mode = prompts ? 1 : 3;
        return;
    }
    /* BrowserAudio's output device is A2I input_stereo_3 through liba2i_client (src/android_auto/audio_a2i; the original
     * HU_NBT has no libasound). Car-verified: connection 0xb8 granted and input_stereo_3 audible. */
    if (!why && !explicit_file && access("/opt/sys/lib/liba2i_client.so", F_OK))
        why = "no /opt/sys/lib/liba2i_client.so on this NBT";
    if (!why && conn && access("audioconn", X_OK)) {
        if (explicit_file)
            conn = 0; /* explicit experiment: play unrouted */
        else
            why = "no audioconn program";
    }
    if (why) {
        printf("audio: Bluetooth (%s)\n", why);
        setenv("NBT_AUDIO_MODE", "bt", 1);
        setenv("NBT_SDR_DROP", "2,4,9", 1); /* services not offered to the phone (see NBT-AAD sdr_builder) */
    } else {
        snprintf(line, sizeof(line), "0x%lx", conn);
        printf("audio: Android Auto browser, connection %s\n", line);
        setenv("NBT_AUDIO_MODE", "browser", 1);
        setenv("NBT_SDR_DROP", "2,4", 1);
        setenv("NBT_BROWSER_CONN", line, 1);
        setenv("NBT_AUDIOCONN_RETRIES", "2", 1);
        if (conn)
            setenv("NBT_AUDIOCONN", RUNTIME_DIR "/audioconn", 1);
    }
    setenv("NBT_PROMPT_CONN", "0", 1);
    setenv("NBT_AUX_SOURCE", "bb", 1);
    setenv("NBT_PLAY_AUX", "0", 1);
    setenv("NBT_NAV_AUDIO", "0", 1);
    setenv("NBT_AUX_PAUSE", "0", 1);
    setenv("NBT_WHEEL_AUX_ONLY", "0", 1);
    audio_knob_mode = 0;
}

/* After a session: connection 0xb8 is an interrupting source (radio / Bluetooth paused while it is built). If
 * nbt_android_auto ended without removing it (crash, kill, unplugged mid-song), remove it here. nbt_audioconn keeps
 * audio-route.built before sending a build, until a remove is granted. */
static void audio_route_cleanup_impl(int force)
{
    char cmd[160];
    char line[32], *end;
    unsigned long conn;
    FILE* f;
    static long long last_attempt = -15000;
    int r;
    if (access("audio-route.built", F_OK) || access("audioconn", X_OK) || (!force && ms() - last_attempt < 15000))
        return;
    last_attempt = ms();
    f = fopen("audio-route.built", "r");
    if (!f) { perror("audio-route.built"); return; }
    if (!fgets(line, sizeof(line), f)) {
        fprintf(stderr, "audio: invalid pending route file\n");
        fclose(f);
        return;
    }
    fclose(f);
    conn = strtoul(line, &end, 0);
    if (end == line || (*end != '\n' && *end != '\0') || !conn || conn > 0xffff) {
        fprintf(stderr, "audio: invalid pending route file\n");
        return;
    }
    snprintf(cmd, sizeof(cmd), "./audioconn -r 0x%lx >> audioconn-cleanup.log 2>&1",
             conn);
    r = system(cmd);
    if (r)
        fprintf(stderr, "audio: route cleanup failed (%d), will retry\n", r);
}

static void audio_route_cleanup(void)
{
    audio_route_cleanup_impl(0);
}

/* ---- persistent event log ------------------------------------------------------------------------------------------
 * /fs/ram3 is lost on every head-unit restart, so after an iDrive reboot nothing showed what happened before it. A few
 * lines per session (start, end, how long, why) plus the tail of the HMI plugin's log are appended to the package
 * directory on flash. Bounded: at 128 KB the file is moved to events.log.1 (one generation kept). Written only on
 * session start/end and every 5 minutes while a session runs, so flash wear stays negligible. */
#define PERSIST_DIR "/var/opt/hmi/data/rw/NbtAndroidAutoRT"
#define PERSIST_LOG PERSIST_DIR "/events.log"

static void plog_rotate(void)
{
    struct stat st;
    if (!stat(PERSIST_LOG, &st) && st.st_size > 128 * 1024)
        rename(PERSIST_LOG, PERSIST_LOG ".1");
}

static void plog_line(const char* fmt, const char* a, unsigned session)
{
    FILE* f;
    time_t now = time(0);
    char when[32];
    if (access(PERSIST_DIR "/incident-logging", F_OK)) return;
    plog_rotate();
    f = fopen(PERSIST_LOG, "a");
    if (!f)
        return;
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s up=%llds session=%u ", when, ms() / 1000, session);
    fprintf(f, fmt, a ? a : "");
    fputc('\n', f);
    fclose(f);
}

/* the last 3 KB of a RAM log, framed, into the persistent log */
static void plog_tail(const char* path, const char* label)
{
    FILE *in, *out;
    char buf[3072];
    size_t n;
    long size;
    if (access(PERSIST_DIR "/incident-logging", F_OK)) return;
    in = fopen(path, "r");
    if (!in)
        return;
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, size > (long)sizeof(buf) ? size - (long)sizeof(buf) : 0, SEEK_SET);
    n = fread(buf, 1, sizeof(buf), in);
    fclose(in);
    plog_rotate();
    out = fopen(PERSIST_LOG, "a");
    if (!out)
        return;
    fprintf(out, "---- tail of %s ----\n", label);
    fwrite(buf, 1, n, out);
    if (n && buf[n - 1] != '\n')
        fputc('\n', out);
    fprintf(out, "---- end ----\n");
    fclose(out);
}

/* ---- incidents ----------------------------------------------------------------------------------------------------
 * Something went wrong: incident.sh (package folder) saves a snapshot (state, RAM log tails, our processes, the QNX
 * system log) to incidents/ and an INCIDENT line to events.log, so the logs can be pulled later from the Mac
 * (scripts/nbt_pull_logs.py) without anybody being connected when it happened. Detached (double fork: never blocks the
 * service, no zombie). The same kind is recorded at most once per min_gap_ms. */
static unsigned g_incident_session;

static void incident(const char* kind, const char* detail, long long min_gap_ms)
{
    static char kinds[8][24];
    static long long last[8];
    static unsigned nkinds;
    unsigned i;
    long long now = ms();
    pid_t p;
    if (access(PERSIST_DIR "/incident-logging", F_OK)) return;
    for (i = 0; i < nkinds; ++i)
        if (!strcmp(kinds[i], kind))
            break;
    if (i < nkinds && now - last[i] < min_gap_ms)
        return;
    if (i == nkinds && nkinds < 8)
        snprintf(kinds[nkinds++], sizeof(kinds[0]), "%s", kind);
    if (i < 8)
        last[i] = now;
    p = fork();
    if (p == 0) {
        if (fork() == 0) {
            char session[16];
            snprintf(session, sizeof(session), "%u", g_incident_session);
            setpgid(0, 0);
            execl("/bin/sh", "sh", PERSIST_DIR "/incident.sh", kind, session, detail ? detail : "", (char*)0);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0)
        while (waitpid(p, 0, 0) < 0 && errno == EINTR) {
        }
}

/* ---- child processes ------------------------------------------------------------------------------------------- */

typedef struct {
    pid_t pid; /* 0 = not running */
    int status;
    const char* name; /* args[0], for incident reports */
    pid_t started_pid;
} Child;

/* 1 while the child runs; reaps it (pid = 0) once it has ended */
static int alive(Child* c)
{
    int s;
    pid_t r;
    if (!c->pid)
        return 0;
    do {
        r = waitpid(c->pid, &s, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        char d[112];
        snprintf(d, sizeof(d), "%s pid=%ld errno=%d", c->name, (long)c->pid, errno);
        plog_line("waitpid failed: %s", d, g_incident_session);
        c->status = -1;
        c->pid = 0;
        return 0;
    }
    if (r == c->pid) {
        c->pid = 0;
        c->status = s;
        /* died by a signal we did not send (end() uses SIGTERM, then SIGKILL): a crash (SIGSEGV, SIGABRT, ...) */
        if (WIFSIGNALED(s) && WTERMSIG(s) != SIGTERM && WTERMSIG(s) != SIGKILL) {
            char d[96];
            snprintf(d, sizeof(d), "%s died by signal %d", c->name ? c->name : "?", WTERMSIG(s));
            plog_line("CRASH %s", d, g_incident_session);
            incident("crash", d, 10000);
        }
        return 0;
    }
    return r == 0;
}

/* stop a child and its whole process group: SIGTERM, up to 5 s grace, then SIGKILL */
static void end(Child* c)
{
    long long until;
    if (!alive(c))
        return;
    kill(-c->pid, SIGTERM);
    until = ms() + 5000;
    while (alive(c) && ms() < until)
        usleep(20000);
    if (alive(c)) {
        char d[96];
        snprintf(d, sizeof(d), "%s pid=%ld did not stop after SIGTERM; sending SIGKILL", c->name, (long)c->pid);
        plog_line("child shutdown: %s", d, g_incident_session);
        kill(-c->pid, SIGKILL);
        while (waitpid(c->pid, &c->status, 0) < 0 && errno == EINTR) {
        }
        c->pid = 0;
    }
}

/* start args[0] in its own process group, stdin from input (or /dev/null), stdout+stderr to log (or /dev/null) */
static Child spawn(char* const args[], const char* input, const char* log)
{
    Child c = {0, -1, 0, 0};
    c.name = args[0];
    c.pid = fork();
    if (!c.pid) {
        int fd;
        if (setpgid(0, 0))
            _exit(125);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        fd = open(input ? input : "/dev/null", O_RDONLY);
        if (fd < 0)
            _exit(126);
        dup2(fd, 0);
        if (fd > 2)
            close(fd);
        fd = open(log ? log : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            _exit(126);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
        execv(args[0], args);
        _exit(127);
    }
    if (c.pid < 0)
        c.pid = 0;
    else {
        c.started_pid = c.pid;
        setpgid(c.pid, c.pid);
    }
    return c;
}

/* ---- status for the HMI page ------------------------------------------------------------------------------------ */

/* The private status.txt plus a world-readable copy /fs/ram3/nbt-aa-state ("state=..", "phone=0|1") for the HMI plugin
 * (aa_entry_plugin). Rewritten at least every 2 s while the service runs, so a stale file means "service down".
 * States: waiting_for_phone, several_devices, waiting_for_hmi, connecting, unsupported_device, replug_required,
 *         starting_decoder, running, draining, stopped. */
static int g_phone_present;
static long long g_last_report;
static char g_last_state[32];

static void report(const char* state, unsigned session)
{
    FILE* f = fopen("status.tmp", "w");
    if (f) {
        fprintf(f, "pid=%d\nstate=%s\nsession=%u\nphone=%d\n", (int)getpid(), state, session, g_phone_present);
        fclose(f);
        rename("status.tmp", "status.txt");
    }
    f = fopen(HMI_STATE_FILE ".tmp", "w");
    if (f) {
        fprintf(f, "state=%s\nphone=%d\nsession=%u\n", state, g_phone_present, session);
        fclose(f);
        chmod(HMI_STATE_FILE ".tmp", 0644);
        rename(HMI_STATE_FILE ".tmp", HMI_STATE_FILE);
    }
    snprintf(g_last_state, sizeof(g_last_state), "%s", state);
    g_last_report = ms();
}

static void keep_reported(unsigned session)
{
    if (ms() - g_last_report >= 2000)
        report(g_last_state, session);
}

/* ---- phone detection --------------------------------------------------------------------------------------------- */

/* Generic phone detection (any Android phone, no list): every USB device except
 *   - the head unit's own devices: PEIKER telematics 2239:f004 (vendor 0x2239) is always on bus 0 address 1
 *   - Apple (0x05ac, no Android Auto), hubs (class 0x09), communication/HID/audio/mass-storage/video device classes
 *   - devices that already answered the AOA probe with "not supported" (ignored until they are unplugged)
 * is a candidate. Accessory mode is 18d1:2d00, or 18d1:2d01 with USB debugging on. Exactly one candidate is required. */
#define USB_VENDOR_PEIKER 0x2239
#define USB_VENDOR_APPLE 0x05ac
#define USB_VENDOR_GOOGLE 0x18d1
#define AOA_PRODUCT_ACCESSORY 0x2d00
#define AOA_PRODUCT_ACCESSORY_ADB 0x2d01
#define PHONE_NONE 0
#define PHONE_NORMAL 1    /* a candidate phone in its normal USB mode: has to be switched to accessory mode */
#define PHONE_ACCESSORY 2 /* already in Android Open Accessory mode: ready for the session */

typedef struct {
    int bus, addr;
    unsigned v, p; /* USB vendor and product id */
} UsbDev;

static UsbDev g_ignored[8];
static int g_ignored_n;

static int ignored(const UsbDev* u)
{
    int i;
    for (i = 0; i < g_ignored_n; ++i)
        if (g_ignored[i].bus == u->bus && g_ignored[i].addr == u->addr && g_ignored[i].v == u->v &&
            g_ignored[i].p == u->p)
            return 1;
    return 0;
}

static void ignore_dev(int bus, int addr, unsigned v, unsigned p)
{
    if (g_ignored_n < 8) {
        UsbDev u = {bus, addr, v, p};
        g_ignored[g_ignored_n++] = u;
    }
}

static unsigned found_v, found_p; /* vendor/product of the last normal-mode candidate (for NBT_PHONE_ID) */
static unsigned found_aoa;        /* product id of the last accessory-mode device (0x2d00 or 0x2d01) */

/* Parses the output of the QNX "usb" tool. Returns PHONE_NORMAL / PHONE_ACCESSORY with *bus / *device set when exactly
 * one candidate is present, else PHONE_NONE. Also updates g_phone_present (any candidate seen). */
static int find_phone(int* bus, int* device)
{
    FILE* f = popen("LD_LIBRARY_PATH=/lib:/usr/lib:/opt/sys/lib usb", "r");
    char line[256];
    int b = -1, result = 0, matches = 0, n = 0, i, j;
    UsbDev dev[16];
    unsigned cls[16];
    int have_cls[16];
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        int d;
        unsigned x;
        if (sscanf(line, "USB %d", &b) == 1)
            continue;
        if (sscanf(line, "Device Address : %d", &d) == 1) {
            if (n < 16 && b >= 0 && d > 0) {
                dev[n].bus = b;
                dev[n].addr = d;
                dev[n].v = dev[n].p = 0;
                cls[n] = 0;
                have_cls[n] = 0;
                ++n;
            }
            continue;
        }
        if (!n)
            continue;
        if (sscanf(line, "Vendor : 0x%x", &x) == 1)
            dev[n - 1].v = x;
        else if (sscanf(line, "Product : 0x%x", &x) == 1)
            dev[n - 1].p = x;
        else if (!have_cls[n - 1] && sscanf(line, "Class : 0x%x", &x) == 1) {
            cls[n - 1] = x; /* the first Class line is the device class */
            have_cls[n - 1] = 1;
        }
    }
    pclose(f);

    /* forget ignored devices that are gone */
    for (i = 0; i < g_ignored_n;) {
        int seen = 0;
        for (j = 0; j < n; ++j)
            if (dev[j].bus == g_ignored[i].bus && dev[j].addr == g_ignored[i].addr && dev[j].v == g_ignored[i].v &&
                dev[j].p == g_ignored[i].p)
                seen = 1;
        if (seen)
            ++i;
        else
            g_ignored[i] = g_ignored[--g_ignored_n];
    }

    g_phone_present = 0;
    for (i = 0; i < n; ++i) {
        UsbDev* u = &dev[i];
        int type = PHONE_NONE;
        unsigned c = cls[i];
        if (!u->v || !u->p || u->v == USB_VENDOR_PEIKER || u->v == USB_VENDOR_APPLE)
            continue;
        /* audio, communication, HID, mass storage, hub, video */
        if (c == 0x01 || c == 0x02 || c == 0x03 || c == 0x08 || c == 0x09 || c == 0x0e)
            continue;
        if (u->v == USB_VENDOR_GOOGLE && (u->p == AOA_PRODUCT_ACCESSORY || u->p == AOA_PRODUCT_ACCESSORY_ADB))
            type = PHONE_ACCESSORY;
        else if (!ignored(u))
            type = PHONE_NORMAL;
        if (!type)
            continue;
        g_phone_present = 1;
        ++matches;
        *bus = u->bus;
        *device = u->addr;
        result = type;
        if (type == PHONE_NORMAL) {
            found_v = u->v;
            found_p = u->p;
        } else
            found_aoa = u->p;
    }
    return matches == 1 ? result : 0;
}

/* ---- small helpers ------------------------------------------------------------------------------------------------ */

/* 1 if the file contains token on one of its lines */
static int log_has(const char* path, const char* token)
{
    FILE* f = fopen(path, "r");
    char l[256];
    int hit = 0;
    if (!f)
        return 0;
    while (!hit && fgets(l, sizeof(l), f))
        hit = strstr(l, token) != 0;
    fclose(f);
    return hit;
}

/* USB reset of the phone; log of the last one in reset.log. */
static void reset_phone(int bus, int device, int type, int n)
{
    char bus_text[16], device_text[16];
    char* args[] = {"./nbt_android_auto", "--reset", bus_text, device_text, 0};
    Child c;
    long long limit;
    snprintf(bus_text, sizeof(bus_text), "%d", bus);
    snprintf(device_text, sizeof(device_text), "%d", device);
    if (type == PHONE_ACCESSORY) {
        setenv("NBT_RESET_AOA", "1", 1);
        if (found_aoa == AOA_PRODUCT_ACCESSORY_ADB)
            setenv("NBT_AOA_PRODUCT", "2d01", 1);
        else
            unsetenv("NBT_AOA_PRODUCT");
    } else {
        char id[16];
        snprintf(id, sizeof(id), "%04x:%04x", found_v, found_p);
        setenv("NBT_PHONE_ID", id, 1);
        unsetenv("NBT_RESET_AOA");
    }
    printf("native_service: failed attempt, USB reset %d of 3 (bus %d device %d)\n", n, bus, device);
    c = spawn(args, 0, "reset.log");
    limit = ms() + 10000;
    while (alive(&c) && ms() < limit)
        usleep(100000);
    end(&c);
    unsetenv("NBT_RESET_AOA");
}

/* read and discard everything currently in a non-blocking descriptor */
static void drain(int fd)
{
    char b[65536];
    while (read(fd, b, sizeof(b)) > 0) {
    }
}

/* split a line into at most max-1 space separated tokens in place (for the optional wal.args file) */
static int split(char* s, char** v, int max)
{
    int n = 0;
    char* p = s;
    while (*p && n < max - 1) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        v[n++] = p;
        while (*p && *p != ' ')
            p++;
        if (*p)
            *p++ = 0;
    }
    v[n] = 0;
    return n;
}

/* center800: nbt_hwvideo hides the iDrive HMI window while Android Auto is in front; make sure it is visible again after
 * nbt_hwvideo ended (also after a crash or SIGKILL of it) */
static void hmi_visible(void)
{
    int r = system("LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_hwvideo --hmi-visible >/dev/null 2>&1");
    (void)r;
}

/* nbt_hwvideo for one session; viewport x,y,w,h on the 1280x480 display: centred (240,0) or left edge (0,0, iDrive
 * stays visible). restart > 0: a later attempt in the same session (own log file). */
static Child start_hwvideo(int center, int right, unsigned session, unsigned restart)
{
    char* args[] = {"./nbt_hwvideo", "--rows", "480", center ? "--viewport" : 0,
                    right ? "0,0,800,480" : "240,0,800,480", right ? "--keep-hmi" : 0, 0};
    char hwname[48];
    Child c;
    if (restart)
        snprintf(hwname, sizeof(hwname), "hwvideo-%u.%u.log", session, restart);
    else
        snprintf(hwname, sizeof(hwname), "hwvideo-%u.log", session);
    setenv("LD_LIBRARY_PATH", SYSTEM_LIBRARY_PATH, 1);
    c = spawn(args, 0, hwname);
    setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
    return c;
}

/* ---- main ------------------------------------------------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    Child lease = {0, -1, 0, 0};   /* accessory_window: keeps the stock USB stack from claiming the phone */
    Child http = {0, -1, 0, 0};    /* fifo_http (or nbt_live_file in the old shadow mode) */
    Child decoder = {0, -1, 0, 0}; /* decode_runner -> NmeCmdLine */
    Child frames = {0, -1, 0, 0};  /* old modes only: native_frames (PNG) or nbt_wal_present */
    Child capture = {0, -1, 0, 0}; /* nbt_android_auto --capture-video: the Android Auto session */
    Child knob = {0, -1, 0, 0};    /* nbt_knob: iDrive controller -> input.fifo */
    Child hwv = {0, -1, 0, 0};     /* nbt_hwvideo: shows the video window in front of iDrive */
    (void)argv;
    int guard = -1, lock = -1, bus, device, type, attempts = 0, resets = 0;
    int recovered = 0; /* pid of a native_service whose stale lock we took over */
    long long absent_since = 0;
    int live = 0, wal = 0, gate = 0, hw = 0, res720 = 0, center = 0, ana = 0, right = 0; /* mode switches */
    unsigned session = 0;
    char bus_text[16], device_text[16], shadow[200], walargs[160], *wargv[16];

    if (argc != 1 || geteuid() != 0 || chdir(RUNTIME_DIR))
        return 2;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGPIPE, SIG_IGN);
    lock = acquire_service_lock(&recovered);
    if (lock < 0) {
        perror("native_service: cannot acquire service lock");
        return 2;
    }
    /* stale FIFOs of a killed instance would make mkfifo fail below */
    unlink("annexb.fifo");
    unlink("raw.fifo");
    plog_line("service start%s", 0, 0);
    if (recovered) {
        char d[64];
        snprintf(d, sizeof(d), "the previous native_service (pid %d) had died", recovered);
        incident("service-recovered", d, 0);
    }
    if (mkfifo("annexb.fifo", 0600) || mkfifo("raw.fifo", 0600))
        goto done;
    /* keep a reader on raw.fifo open so a writer (shared Cinemo service in the old PNG mode) never gets SIGPIPE */
    guard = open("raw.fifo", O_RDONLY | O_NONBLOCK);
    if (guard < 0)
        goto done;
    fcntl(guard, F_SETFD, FD_CLOEXEC);

    /* ---- environment for nbt_android_auto and the NBT-AAD core (read by the children) ---- */
    setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
    setenv("NBT_AA_TLS_CERT", RUNTIME_DIR "/cert.pem", 1);
    setenv("NBT_AA_TLS_KEY", RUNTIME_DIR "/key.pem", 1);
    setenv("NBT_AA_TLS_CA", RUNTIME_DIR "/ca.pem", 1);
    setenv("NBT_AA_TLS_ALLOW_DATE_ERRORS", access(PKG_DIR "tls-allow-date-errors", F_OK) ? "0" : "1", 1);
    /* Any phone: the phone's certificate must chain to ca.pem = Google Automotive Link root (valid until 2044), the root
     * behind every Android Auto phone; validity and key usage are still checked. No per-app leaf pin any more (the OnePlus
     * leaf sha256:893145f5... would break with another phone or an Android Auto app update). peer.pin restores a pin. */
    setenv("NBT_AA_TLS_PEER", "chain", 1);
    {
        FILE* f = fopen("peer.pin", "r");
        char pin[80];
        if (f) {
            if (fgets(pin, sizeof(pin), f)) {
                size_t n = strlen(pin);
                while (n && (pin[n - 1] == '\n' || pin[n - 1] == '\r'))
                    pin[--n] = 0;
                if (n == 71 && !strncmp(pin, "sha256:", 7)) /* "sha256:" + 64 hex digits */
                    setenv("NBT_AA_TLS_PEER", pin, 1);
            }
            fclose(f);
        }
    }
    setenv("NBT_AA_CAPTURE_PREFIX", RUNTIME_DIR "/session", 1);
    setenv("NBT_AA_LIVE_FIFO", RUNTIME_DIR "/annexb.fifo", 1);
    setenv("NBT_AA_USB_RX_16K", access(PKG_DIR "usb-rx-16k", F_OK) == 0 ? "1" : "0", 1);
    /* identity the head unit reports to the phone in the service discovery response */
    setenv("NBT_SDR_IDENTITY", "1", 1);
    setenv("NBT_CAR_MAKE", "BMW", 1);
    setenv("NBT_CAR_MODEL", "F07", 1);
    /* Phone canvas 800x480 (NBT_VID_RES=1 in capture). The reference default insets 0,240,0,0 are meant for a 1280x720 canvas
     * and left the phone only the top 240 rows (vehicle 2026-11-06: picture in the upper half only). 0,180,0,0 leaves
     * 800x300 = the 8:3 shape of the 1280x480 display, stretched 1.6x both ways. Not overwritten when the caller sets it. */
    setenv("NBT_VID_INSETS", "0,180,0,0", 0);
    /* pad each H.264 access unit to 8 KB with a filler NAL so the decoder's byte buffers fill quickly (input lag) */
    setenv("NBT_AA_PAD", "8192", 0);
    setenv("NBT_CAR_YEAR", "2014", 1);
    setenv("NBT_HU_MODEL", "HU_NBT", 1);
    set_audio_mode();
    setenv("NBT_RX_TIMEOUT_S", "5", 1);
    /* now playing (song / artist / state) for nbt_cluster's track line in the cluster / HUD */
    setenv("NBT_NOWPLAYING_FILE", RUNTIME_DIR "/nowplaying", 1);
    unlink("nowplaying");
    /* the car's master volume steers the Android Auto gain (nbt_knob -> "V" lines); aa-volume-off keeps 0 dB */
    setenv("NBT_AA_VOLUME", access(PKG_DIR "aa-volume-off", F_OK) ? "1" : "0", 1);

    /* ---- mode switches (files in the runtime directory) ---- */
    /* Old experimental mode: if shadow.path names a file under /fs/usb0/, nbt_live_file shadows that file with the live
     * H.264 stream so the STOCK USB video player plays it (experiments/usb-file-overlay). In that mode fifo_http,
     * native_frames and decode_runner are not started. Without shadow.path nothing changes. */
    shadow[0] = 0;
    {
        FILE* f = fopen("shadow.path", "r");
        if (f) {
            if (fgets(shadow, sizeof(shadow), f)) {
                size_t n = strlen(shadow);
                while (n && (shadow[n - 1] == '\n' || shadow[n - 1] == '\r' || shadow[n - 1] == ' '))
                    shadow[--n] = 0;
                if ((strncmp(shadow, "/fs/usb0/", 9) && strncmp(shadow, "/fs/pfs0/", 9)) || n <= 9)
                    shadow[0] = 0;
            }
            fclose(f);
        }
    }
    live = shadow[0] != 0;
    /* Old experimental mode: if wal.args exists, nbt_wal_present (decoded picture as a WAL sprite in front of the HMI,
     * arguments from that file, may be empty) replaces native_frames (experiments/wal-overlay; crashed the car). */
    walargs[0] = 0;
    {
        FILE* f = fopen("wal.args", "r");
        if (f) {
            wal = 1;
            if (fgets(walargs, sizeof(walargs), f)) {
                size_t n = strlen(walargs);
                while (n && (walargs[n - 1] == '\n' || walargs[n - 1] == '\r'))
                    walargs[--n] = 0;
            }
            fclose(f);
        }
    }
    if (wal) {
        wargv[0] = "./nbt_wal_present";
        split(walargs, wargv + 1, 15);
    }
    gate = access("gate.mode", F_OK) == 0;
    /* Hardware video mode (hw.mode, car-proven path 2026-11-07): the decoder renders into the stock video window
     * (decode_runner "screen"), nbt_hwvideo asks VideoApp for the video connection and shows it in front of iDrive.
     * No YUV FIFO, no RGBA conversion, no WAL sprites. Takes precedence over wal.args. */
    hw = access("hw.mode", F_OK) == 0 && access("nbt_hwvideo", X_OK) == 0;
    /* res720 (hardware mode only): the phone sends 1280x720 and draws into rows 0..479 (1280x480, the display shape);
     * the decoder crops these rows. Sharper vertically than 800x300; the capture stays 800x480. */
    res720 = hw && access("res720", F_OK) == 0;
    if (res720) {
        setenv("NBT_VID_RES", "2", 1);
        setenv("NBT_VID_INSETS", "0,240,0,0", 1);
    }
    /* center800 (hardware mode only, wins over res720; user request 2026-11-08): the phone draws the whole 800x480 canvas,
     * shown 1:1 in the middle of the 1280x480 display (black bars of 240 px left and right). No scaling at all: as sharp as
     * the video path allows (the capture is 800x480 anyway), instead of 800x300 stretched 1.6x both ways. */
    center = hw && access("center800", F_OK) == 0;
    /* split800 (user idea 2026-11-08): same 1:1 picture at the LEFT edge (0..799); iDrive stays visible on the right
     * 480 px, exactly where its own split screen (480x480 windows) sits. This is the "16:9" mode of the HMI page. */
    right = hw && !center && access("split800", F_OK) == 0;
    if (right)
        center = 1;
    if (center) {
        res720 = 0;
        setenv("NBT_VID_RES", "1", 1);
        setenv("NBT_VID_INSETS", "0,0,0,0", 1);
    }
    /* anamorph (hardware mode only, user idea 2026-11-08): the phone is told that the pixels are 1.6x wider than tall
     * (VideoConfig pixel_aspect_ratio_e4 = 16000) and draws its whole 800x480 canvas squeezed horizontally; the GPU
     * stretches 800 -> 1280 only horizontally, so the picture fills the display with 480 instead of 300 rows.
     * This is the "wide" mode of the HMI page. */
    ana = hw && !center && access("anamorph", F_OK) == 0;
    if (ana) {
        res720 = 0;
        setenv("NBT_VID_RES", "1", 1);
        setenv("NBT_VID_INSETS", "0,0,0,0", 1);
        setenv("NBT_VID_PAR", "16000", 0);
    }

    /* ---- processes that run for the whole service life ---- */
    if (live) {
        char* args[] = {"./nbt_live_file", "--path", shadow, "--fifo", "annexb.fifo", "--ready", "http.ready",
                        "--seconds", "0", 0};
        unlink("http.ready");
        http = spawn(args, 0, "live_file.log");
        {
            long long limit = ms() + 5000;
            while (!stopping && access("http.ready", F_OK) && alive(&http) && ms() < limit)
                usleep(100000);
        }
        if (access("http.ready", F_OK) || !alive(&http))
            goto done;
    }
    {
        char* args[] = {"./accessory_window", "--owned-window", 0};
        lease = spawn(args, 0, "window.log");
    }
    usleep(500000);
    if (!alive(&lease))
        goto done;
    /* Optional iDrive controller (nbt_knob present): observe-only MOST client, writes key lines into input.fifo, which the
     * live AA session (nbt_android_auto --capture-video) reads. Runs for the whole service life, independent of sessions.
     * nbt_knob needs /opt/sys/lib (libsys_dsi_colibry.so.3 and its dependencies; car 2026-11-07: "Library cannot be found"
     * with the Cinemo library path), so it gets its own library path for the fork. */
    if (!access("nbt_knob", X_OK) && (!mkfifo("input.fifo", 0600) || errno == EEXIST)) {
        char* args[] = {"./nbt_knob", 0};
        setenv("LD_LIBRARY_PATH", SYSTEM_LIBRARY_PATH, 1);
        knob = spawn(args, 0, "knob.log");
        setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
    }

    /* ---- one Android Auto session per loop pass ---- */
    while (!stopping && alive(&lease)) {
        char session_end[160] = "";
        audio_route_cleanup();
        /* Optional HMI gate (file gate.mode in the runtime directory): the phone is only touched after the Android Auto
         * page in the HMI was opened, which creates /fs/ram3/nbt-aa-request (see src/idrive/aa_entry_plugin.cpp). */
        if (gate && access(HMI_REQUEST_FILE, F_OK)) {
            find_phone(&bus, &device);
            report("waiting_for_hmi", session);
            sleep(1);
            continue;
        }
        type = find_phone(&bus, &device);
        if (!type) {
            attempts = 0;
            /* gone for more than 10 s: really unplugged (a USB reset of ours only makes it vanish for a moment) */
            if (!absent_since)
                absent_since = ms();
            if (ms() - absent_since > 10000)
                resets = 0;
            report(g_phone_present ? "several_devices" : "waiting_for_phone", session);
            sleep(1);
            continue;
        }
        absent_since = 0;
        /* The last attempt failed (user 2026-11-12: after "Restart Android Auto" the phone often did not come back until
         * it was replugged): reset its USB port, the software version of pulling the cable, and try again. "Reconnect the
         * phone" only after 3 resets in a row; a real unplug (> 10 s) or a good session starts the count again. */
        if (attempts > 0) {
            if (resets >= 3) {
                report("replug_required", session);
                g_incident_session = session;
                incident("replug", "3 USB resets did not bring the phone back", 600000);
                sleep(2);
                continue;
            }
            ++resets;
            report("connecting", session);
            reset_phone(bus, device, type, resets);
            attempts = 0;
            sleep(3);
            continue;
        }

        /* step 2: switch a normal-mode phone to accessory mode; it then reappears as 18d1:2d00/2d01 */
        if (type == PHONE_NORMAL) {
            Child sw;
            char* args[] = {"./nbt_android_auto", "--switch-aa", bus_text, device_text, 0};
            char id[16];
            report("connecting", session);
            snprintf(id, sizeof(id), "%04x:%04x", found_v, found_p);
            setenv("NBT_PHONE_ID", id, 1);
            snprintf(bus_text, sizeof(bus_text), "%d", bus);
            snprintf(device_text, sizeof(device_text), "%d", device);
            sw = spawn(args, 0, "switch.log");
            while (!stopping && alive(&sw))
                usleep(100000);
            end(&sw);
            if (stopping)
                break;
            /* Only ignore devices that actually refused GET_PROTOCOL (unsupported version, STALL, or short reply).
             * Transient attach/connect/USB errors before a definitive answer should retry instead of blacklisting. */
            if (!log_has("switch.log", "AOA_PROTOCOL_OK")) {
                if (log_has("switch.log", "AOA_PROTOCOL_UNSUPPORTED") ||
                    log_has("switch.log", "AOA_REQUEST id=51 index=0 result=0 status=02000004 ") ||
                    log_has("switch.log", "AOA_REQUEST id=51 index=0 result=0 status=01000000 ")) {
                    ignore_dev(bus, device, found_v, found_p);
                    report("unsupported_device", session);
                } else {
                    ++attempts;
                }
                sleep(1);
                continue;
            }
            sleep(1);
            if (find_phone(&bus, &device) != PHONE_ACCESSORY) {
                ++attempts;
                continue;
            }
        }

        /* step 3: the video chain for this session */
        report("connecting", session);
        ++attempts;
        ++session;
        g_incident_session = session;
        report("starting_decoder", session);
        drain(guard);
        /* no leftover client of the shared io-nme from an earlier, abruptly ended session (only our own NmeCmdLine copy
         * exists: the stock player works through mme/io-media-cinemo, never NmeCmdLine) */
        if (hw) {
            int r = system("slay -f -Q -s TERM NmeCmdLine >/dev/null 2>&1");
            (void)r;
            usleep(500000);
            r = system("slay -f -Q -s KILL NmeCmdLine >/dev/null 2>&1");
            (void)r;
        }
        if (found_aoa == AOA_PRODUCT_ACCESSORY_ADB)
            setenv("NBT_AOA_PRODUCT", "2d01", 1);
        else
            unsetenv("NBT_AOA_PRODUCT");
        if (!live) {
            unlink("http.ready");
            {
                char* args[] = {"./fifo_http", "annexb.fifo", "http.ready", 0};
                http = spawn(args, 0, 0);
            }
            if (hw) {
                /* hardware mode: nothing between decoder and screen */
            } else if (wal)
                frames = spawn(wargv, "raw.fifo", "present.log");
            else {
                char* args[] = {"./native_frames", "--seconds", "0", "--fps", "3", 0};
                frames = spawn(args, "raw.fifo", "frames.log");
            }
            {
                char* args[] = {"./decode_runner", "0", RUNTIME_DIR "/NmeCmdLine", "http://127.0.0.1:18765/live.h264",
                                hw ? ((center || ana) ? "screencenter" : res720 ? "screen720" : "screen")
                                   : RUNTIME_DIR "/raw.fifo",
                                0};
                char decname[40];
                snprintf(decname, sizeof(decname), "decoder-%u.log", session);
                decoder = spawn(args, 0, decname);
            }
            /* wait up to 10 s until the player is connected to fifo_http */
            {
                long long limit = ms() + 10000;
                while (!stopping && access("http.ready", F_OK) && alive(&http) && alive(&decoder) && ms() < limit)
                    usleep(100000);
            }
            /* the decoder already crops to the content rows (-vr), so the window is the plain 800x480 output;
             * viewport x,y,w,h on the 1280x480 display: centred (240,0) or left edge (0,0, iDrive stays visible) */
            if (hw && !stopping)
                hwv = start_hwvideo(center, right, session, 0);
        }

        /* step 4: the Android Auto session itself (old live mode: nbt_live_file, kept in 'http', runs all the time) */
        if (!stopping && (live ? alive(&http) : (!access("http.ready", F_OK) && alive(&decoder) && alive(&http)))) {
            char* args[] = {"./nbt_android_auto", "--capture-video", bus_text, device_text, 0};
            int previous_knob_mode = audio_knob_mode;
            set_audio_mode(); /* again per session: a failed audio route falls back to Bluetooth */
            if (previous_knob_mode != audio_knob_mode && knob.pid) {
                char* knob_args[] = {"./nbt_knob", 0};
                end(&knob);
                audio_route_cleanup_impl(1);
                set_audio_mode();
                setenv("LD_LIBRARY_PATH", SYSTEM_LIBRARY_PATH, 1);
                knob = spawn(knob_args, 0, "knob.log");
                setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
            }
            snprintf(bus_text, sizeof(bus_text), "%d", bus);
            snprintf(device_text, sizeof(device_text), "%d", device);
            {
                char capname[40];
                snprintf(capname, sizeof(capname), "capture-%u.log", session);
                /* /opt/sys/lib last: liba2i_client.so (A2I audio output) and what it links */
                setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH ":/opt/sys/lib", 1);
                unlink("/fs/ram3/nbt-aa-media"); /* the phone's media state, written anew by this session */
                capture = spawn(args, 0, capname);
                setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
            }
            report("running", session);
            {
                long long began = ms(), last_hwv = ms(), last_persist = ms();
                unsigned hwv_restarts = 0;
                const Child* ended = NULL;
                const char* phone = getenv("NBT_PHONE_ID");
                char detail[112];
                snprintf(detail, sizeof(detail), "phone=%s capture_pid=%ld http_pid=%ld decoder_pid=%ld",
                         phone ? phone : "unknown", (long)capture.started_pid, (long)http.started_pid, (long)decoder.pid);
                plog_line("session running (%s)", detail, session);
                while (!stopping) {
                    if (!alive(&capture))
                        ended = &capture;
                    else if (!alive(&http))
                        ended = &http;
                    else if (!live && !alive(&decoder))
                        ended = &decoder;
                    else if (!live && !hw && !alive(&frames))
                        ended = &frames;
                    if (ended)
                        break;
                    usleep(200000);
                    keep_reported(session);
                    /* "connected, but no picture": nbt_hwvideo gives up (exit 5) when VideoApp never acknowledged the
                     * video connection, or it died. The session and the phone are fine, so only the picture side is
                     * started again (at most every 10 s, 6 times per session) instead of leaving a black page that
                     * only a restart fixed. */
                    /* only while the Android Auto page is on screen: every start takes VideoApp's sink (StopVideo, Set
                     * source 0x16), which would interrupt the reverse camera or a DVD that made it give up */
                    if (hw && !live && !alive(&hwv) && hwv_restarts < 6 && ms() - last_hwv >= 10000 &&
                        access("/fs/ram3/nbt-aa-hidden", F_OK)) {
                        char line[64];
                        snprintf(line, sizeof(line), "%d",
                                 hwv.status >= 0 && WIFEXITED(hwv.status) ? WEXITSTATUS(hwv.status) : -1);
                        plog_line("nbt_hwvideo ended (exit %s): starting it again", line, session);
                        incident("no-picture", line, 60000);
                        hwv = start_hwvideo(center, right, session, ++hwv_restarts);
                        last_hwv = ms();
                    }
                    if (!unlink("stall")) { /* session saw no video for 15 s while the page was on screen */
                        plog_line("FREEZE no video for 15 s while Android Auto was on screen%s", 0, session);
                        incident("freeze", "no video frame for 15 s while the Android Auto page was on screen", 300000);
                    }
                    if (ms() - last_persist >= 300000) {
                        last_persist = ms();
                        plog_line("session still running%s", 0, session);
                    }
                }
                if (ended) {
                    const char* outcome = "status unavailable";
                    int code = -1;
                    if (ended->status >= 0 && WIFEXITED(ended->status)) {
                        outcome = "exit";
                        code = WEXITSTATUS(ended->status);
                    } else if (ended->status >= 0 && WIFSIGNALED(ended->status)) {
                        outcome = "signal";
                        code = WTERMSIG(ended->status);
                    }
                    snprintf(detail, sizeof(detail), "%s ended after %llds, pid=%ld %s=%d",
                             ended->name ? ended->name : "unknown child", (ms() - began) / 1000,
                             (long)ended->started_pid, outcome, code);
                } else {
                    snprintf(detail, sizeof(detail), "service stopping after %llds, signal=%d", (ms() - began) / 1000,
                             (int)stopping);
                }
                snprintf(session_end, sizeof(session_end), "%s, picture restarts %u", detail, hwv_restarts);
                plog_line("session end: %s", session_end, session);
                plog_tail("/fs/ram3/nbt-aa-plugin.log", "HMI plugin log");
                /* a session that ran for a minute was a good one: its end is not a failed attempt (review 2026-11-09:
                 * the counter was only reset by unplugging, so the third normal session end of a drive showed
                 * "replug the phone") */
                if (ms() - began >= 60000)
                    attempts = resets = 0;
            }
        }

        /* step 5: tear the chain down (video window first, so iDrive is visible again) */
        report("draining", session);
        end(&hwv);
        if (center)
            hmi_visible();
        end(&capture);
        if (*session_end && !stopping)
            incident("session-end", session_end, 30000);
        audio_route_cleanup();
        if (!live)
            end(&http);
        {
            long long limit = ms() + 3000;
            while (alive(&decoder) && ms() < limit)
                usleep(20000);
        }
        end(&decoder);
        {
            long long limit = ms() + 2000;
            while (alive(&frames) && ms() < limit)
                usleep(20000);
        }
        end(&frames);
        drain(guard);
        if (!live)
            unlink("http.ready");
        /* Clear only the FIFO bytes left by this completed session (live mode: nbt_live_file owns the FIFO reader). */
        if (!live) {
            int fd = open("annexb.fifo", O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                drain(fd);
                close(fd);
            }
        }
        if (!stopping)
            sleep(2);
    }

done:
    end(&hwv);
    if (center)
        hmi_visible();
    end(&capture);
    audio_route_cleanup();
    end(&http);
    end(&decoder);
    end(&frames);
    end(&knob);
    audio_route_cleanup_impl(1);
    end(&lease);
    unlink("input.fifo");
    if (guard >= 0) {
        drain(guard);
        close(guard);
    }
    unlink("annexb.fifo");
    unlink("raw.fifo");
    unlink("http.ready");
    unlink("service.lock");
    close(lock);
    plog_line("service stop%s", 0, session);
    report("stopped", session);
    unlink(HMI_STATE_FILE);
    return 0;
}
