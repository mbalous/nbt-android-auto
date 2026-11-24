// aa_video_capture: runs one Android Auto session (the NBT-AAD core, nbt::AASession) over the phone's USB accessory link
// and takes care of what the head unit needs from it. Part of the nbt_android_auto program (--capture-video).
//
// Two modes:
//   capture mode (diagnostics)  H.264 frames go to <prefix>.h264 with an index <prefix>.csv; bounded by time, frame
//                               count and 8 MB.
//   live mode (the product)     NBT_AA_LIVE_FIFO is set by native_service: H.264 goes into annexb.fifo for the decoder
//                               (via a queue and a writer thread), iDrive controller keys come in from input.fifo,
//                               navigation guidance goes out to nav.state for nbt_cluster. Runs until SIGTERM.
//
// Data flow:
//   USB (transfer callback) --reader thread--> socket pair --> AASession (TLS, protocol)
//   AASession video callback --> save() --> live queue --> live_writer thread --> annexb.fifo --> fifo_http --> decoder
//   input.fifo (nbt_knob) --> handle_input() --> AASession key / rotary events
//   AASession navigation callback --> nav_status() --> /fs/ram3/nbt-aa-native/nav.state
#include "aa_video_capture.h"
#include "aa_audio.hpp"
#include "aa_session.hpp"
#include "aa_media_out.hpp"
#include "video_recovery.h"
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <deque>
#include <vector>

namespace {

// ===== Android Auto navigation status (channel 10) -> nav.state for nbt_cluster (2026-11-08) ==========================
//
// Message ids and protobuf field numbers (aasdk navigationstatus):
//   0x8003 NavigationStatus          {1 status: 1 active, 2 inactive, 3 rerouting}
//   0x8006 NavigationState           {1 steps[] {1 maneuver {1 type, 2 exit_number, 3 exit_angle}, 2 road {1 name},
//                                                3 lanes[] {1 directions[] {1 shape, 2 highlighted}}}}
//   0x8007 NavigationCurrentPosition {1 step_distance {1 distance {1 meters, 2 display_value, 3 display_units}}}
// older phones:
//   0x8004 NextTurnEvent             {1 road, 2 turn_side, 3 event, 6 exit_angle}
//   0x8005 NextTurnDistanceEvent     {1 distance_m, 3 display_distance_e3, 4 unit}
// Maneuver type = androidx.car.app.navigation.model.Maneuver TYPE_*.
// nav.state is written atomically (tmp + rename) whenever something changes; the file is small and in RAM.
enum NavMessage {
    kNavStatus = 0x8003,
    kNextTurnEvent = 0x8004,         // old phones
    kNextTurnDistanceEvent = 0x8005, // old phones
    kNavState = 0x8006,
    kNavCurrentPosition = 0x8007
};

struct NavInfo {
    int status;         // 1 active, 2 inactive, 3 rerouting (0 = nothing received yet)
    int type;           // maneuver type of the next step
    int unit;           // display unit of dist_text (1 m, 2/3 km, 4/5 mi, 6 ft, 7 yd)
    int type2;          // maneuver type of the step after (-1 = none)
    long long dist_m;   // distance to the next maneuver in metres (-1 = unknown)
    long long exit_angle;
    bool has_angle;
    std::string road, dist_text, road2, lanes; // lanes: "4h,1o,1o" (see nav_status)
    unsigned seq;       // incremented on every write
    NavInfo() : status(0), type(0), unit(0), type2(-1), dist_m(-1), exit_angle(0), has_angle(false), seq(0) {}
};
NavInfo g_nav;

// ---- minimal protobuf reader (no generated code on the head unit) ----

// one varint at p (advances p); false if it runs past e
bool pb_varint(const uint8_t*& p, const uint8_t* e, unsigned long long& v)
{
    v = 0;
    for (int sh = 0; p < e && sh < 64; sh += 7) {
        uint8_t b = *p++;
        v |= (unsigned long long)(b & 0x7f) << sh;
        if (!(b & 0x80))
            return true;
    }
    return false;
}

// First occurrence of field num in a message: varint -> *v, length-delimited -> *d / *n.
// Returns the wire type (0 or 2) or -1 if the field is missing or the message is malformed.
int pb_field(const uint8_t* p, size_t len, unsigned num, unsigned long long* v, const uint8_t** d, size_t* n)
{
    const uint8_t* e = p + len;
    while (p < e) {
        unsigned long long tag, x;
        if (!pb_varint(p, e, tag))
            return -1;
        unsigned w = (unsigned)(tag & 7), f = (unsigned)(tag >> 3); // wire type, field number
        if (w == 0) { // varint
            if (!pb_varint(p, e, x))
                return -1;
            if (f == num) {
                if (v)
                    *v = x;
                return 0;
            }
        } else if (w == 2) { // length-delimited
            if (!pb_varint(p, e, x) || x > (unsigned long long)(e - p))
                return -1;
            if (f == num) {
                if (d)
                    *d = p;
                if (n)
                    *n = (size_t)x;
                return 2;
            }
            p += x;
        } else if (w == 1) { // 64 bit
            if (e - p < 8)
                return -1;
            p += 8;
        } else if (w == 5) { // 32 bit
            if (e - p < 4)
                return -1;
            p += 4;
        } else
            return -1;
    }
    return -1;
}

// sub-message (or bytes) field num
bool pb_sub(const uint8_t* p, size_t len, unsigned num, const uint8_t** d, size_t* n)
{
    return pb_field(p, len, num, 0, d, n) == 2;
}

// k-th (0-based) length-delimited occurrence of field num (repeated sub-messages such as NavigationState.steps)
bool pb_sub_nth(const uint8_t* p, size_t len, unsigned num, unsigned k, const uint8_t** d, size_t* n)
{
    const uint8_t* e = p + len;
    while (p < e) {
        unsigned long long tag, x;
        if (!pb_varint(p, e, tag))
            return false;
        unsigned w = (unsigned)(tag & 7), f = (unsigned)(tag >> 3);
        if (w == 0) {
            if (!pb_varint(p, e, x))
                return false;
        } else if (w == 2) {
            if (!pb_varint(p, e, x) || x > (unsigned long long)(e - p))
                return false;
            if (f == num) {
                if (!k) {
                    *d = p;
                    *n = (size_t)x;
                    return true;
                }
                --k;
            }
            p += x;
        } else if (w == 1) {
            if (e - p < 8)
                return false;
            p += 8;
        } else if (w == 5) {
            if (e - p < 4)
                return false;
            p += 4;
        } else
            return false;
    }
    return false;
}

// integer field num
bool pb_int(const uint8_t* p, size_t len, unsigned num, long long* v)
{
    unsigned long long x;
    if (pb_field(p, len, num, &x, 0, 0) != 0)
        return false;
    *v = (long long)x;
    return true;
}

// string field num, control characters replaced by spaces ("" if missing)
std::string pb_str(const uint8_t* p, size_t len, unsigned num)
{
    const uint8_t* d;
    size_t n;
    if (pb_field(p, len, num, 0, &d, &n) != 2)
        return std::string();
    std::string s((const char*)d, n);
    for (size_t i = 0; i < s.size(); ++i)
        if ((unsigned char)s[i] < 0x20)
            s[i] = ' ';
    return s;
}

// ---- nav.state ----

void nav_write()
{
    char buf[512];
    ++g_nav.seq;
    int n = snprintf(buf, sizeof(buf),
                     "seq=%u\nstatus=%d\ntype=%d\nangle=%lld\nhas_angle=%d\ndist_m=%lld\ndist_text=%s\nunit=%d\nroad=%s\n"
                     "type2=%d\nroad2=%s\nlanes=%s\n",
                     g_nav.seq, g_nav.status, g_nav.type, g_nav.exit_angle, g_nav.has_angle ? 1 : 0, g_nav.dist_m,
                     g_nav.dist_text.c_str(), g_nav.unit, g_nav.road.substr(0, 120).c_str(), g_nav.type2,
                     g_nav.road2.substr(0, 120).c_str(), g_nav.lanes.c_str());
    if (n <= 0)
        return;
    int fd = open("/fs/ram3/nbt-aa-native/nav.state.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    if (w == n)
        rename("/fs/ram3/nbt-aa-native/nav.state.tmp", "/fs/ram3/nbt-aa-native/nav.state");
}

// AASession navigation callback: one message of the navigation status channel
void nav_status(uint16_t id, const uint8_t* b, size_t len, void*)
{
    long long x;
    const uint8_t* d;
    size_t n;
    if (id == kNavStatus) {
        if (pb_int(b, len, 1, &x))
            g_nav.status = (int)x;
    } else if (id == kNavState) {
        // first step = the next maneuver
        const uint8_t* st;
        size_t sn;
        if (!pb_sub(b, len, 1, &st, &sn))
            return;
        if (pb_sub(st, sn, 1, &d, &n)) { // maneuver
            g_nav.type = 0;
            g_nav.has_angle = false;
            if (pb_int(d, n, 1, &x))
                g_nav.type = (int)x;
            if (pb_int(d, n, 3, &x)) {
                g_nav.exit_angle = x;
                g_nav.has_angle = true;
            }
        }
        if (pb_sub(st, sn, 2, &d, &n)) // road
            g_nav.road = pb_str(d, n, 1);
        // lanes of this step: field 3 repeated {1 repeated LaneDirection {1 shape, 2 highlighted}} -> "4h,1o,1o" (left to
        // right; per lane the highlighted direction, else the first; shape 1 straight, 2/3 slight, 4/5 normal, 6/7 sharp,
        // 8/9 u-turn left/right; h = highlighted, o = other)
        g_nav.lanes.clear();
        {
            const uint8_t* ln;
            size_t lnn;
            for (unsigned li = 0; li < 10 && pb_sub_nth(st, sn, 3, li, &ln, &lnn); ++li) {
                int shape = -1;
                bool hl = false;
                const uint8_t* dd;
                size_t dn;
                for (unsigned di = 0; di < 6 && pb_sub_nth(ln, lnn, 1, di, &dd, &dn); ++di) {
                    long long sh = 0, h = 0;
                    pb_int(dd, dn, 1, &sh);
                    pb_int(dd, dn, 2, &h);
                    if (h) {
                        shape = (int)sh;
                        hl = true;
                        break;
                    }
                    if (shape < 0)
                        shape = (int)sh;
                }
                char t[8];
                snprintf(t, sizeof(t), "%s%d%c", g_nav.lanes.empty() ? "" : ",", shape < 0 ? 0 : shape, hl ? 'h' : 'o');
                g_nav.lanes += t;
            }
        }
        // second step = the maneuver after the next one
        g_nav.type2 = -1;
        g_nav.road2.clear();
        const uint8_t* st2;
        size_t sn2;
        if (pb_sub_nth(b, len, 1, 1, &st2, &sn2)) {
            g_nav.type2 = 0;
            if (pb_sub(st2, sn2, 1, &d, &n) && pb_int(d, n, 1, &x))
                g_nav.type2 = (int)x;
            if (pb_sub(st2, sn2, 2, &d, &n))
                g_nav.road2 = pb_str(d, n, 1);
        }
        if (!g_nav.status)
            g_nav.status = 1;
    } else if (id == kNavCurrentPosition) {
        const uint8_t* sd;
        size_t sdn;
        if (pb_sub(b, len, 1, &sd, &sdn) && pb_sub(sd, sdn, 1, &d, &n)) { // step_distance.distance
            if (pb_int(d, n, 1, &x))
                g_nav.dist_m = x;
            g_nav.dist_text = pb_str(d, n, 2);
            if (pb_int(d, n, 3, &x))
                g_nav.unit = (int)x;
        }
    } else if (id == kNextTurnEvent) {
        // old event numbers -> maneuver type for a left / right turn: {event, type if left, type if right}
        static const int legacy[][3] = {{1, 1, 1},    {2, 2, 2},    {3, 5, 6},    {4, 7, 8},    {5, 9, 10},
                                        {6, 11, 12},  {7, 15, 16},  {8, 23, 24},  {9, 25, 26},  {10, 27, 28},
                                        {11, 34, 32}, {12, 34, 32}, {13, 34, 32}, {14, 36, 36}, {16, 37, 37},
                                        {17, 38, 38}, {19, 39, 39}};
        long long side = 0, ev = 0;
        bool hs = pb_int(b, len, 2, &side); // turn_side: 1 left, 2 right
        int pl = 0, pr = 0;
        if (pb_int(b, len, 3, &ev))
            for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); ++i)
                if (legacy[i][0] == ev) {
                    pl = legacy[i][1];
                    pr = legacy[i][2];
                }
        g_nav.type = hs && side == 1 ? pl : hs && side == 2 ? pr : (pl == pr ? pl : 0);
        g_nav.road = pb_str(b, len, 1);
        g_nav.has_angle = pb_int(b, len, 6, &x);
        if (g_nav.has_angle)
            g_nav.exit_angle = x;
        if (!g_nav.status)
            g_nav.status = 1;
    } else if (id == kNextTurnDistanceEvent) {
        if (pb_int(b, len, 1, &x))
            g_nav.dist_m = x;
        if (pb_int(b, len, 4, &x))
            g_nav.unit = (int)x;
        if (pb_int(b, len, 3, &x)) { // display distance in 1/1000 of the unit
            char t[32];
            if (g_nav.unit == 3 || g_nav.unit == 5) // km / mi with one decimal
                snprintf(t, sizeof(t), "%lld.%lld", x / 1000, (x % 1000) / 100);
            else
                snprintf(t, sizeof(t), "%lld", (x + 500) / 1000);
            g_nav.dist_text = t;
        }
    } else
        return;
    printf("[NAV] msg 0x%04X status=%d type=%d angle=%lld dist=%lldm '%s' road='%s'\n", id, g_nav.status, g_nav.type,
           g_nav.has_angle ? g_nav.exit_angle : -1LL, g_nav.dist_m, g_nav.dist_text.c_str(), g_nav.road.c_str());
    nav_write();
}

// Media playback channel (ch 12): write playback state to /fs/ram3/nbt-aa-media (for nbt_knob) and now-playing
// metadata atomically to NBT_NOWPLAYING_FILE (for nbt_cluster / HUD).
void write_now_playing(const std::string* song, const std::string* artist, uint32_t state)
{
    static std::string last_song, last_artist;
    const char* path = getenv("NBT_NOWPLAYING_FILE");
    if (!path || strncmp(path, "/fs/ram3/", 9))
        return;
    if (song)
        last_song = song->substr(0, 120);
    if (artist)
        last_artist = artist->substr(0, 120);
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f)
        return;
    fprintf(f, "state=%u\nsong=%s\nartist=%s\n", state == 0xFFFFFFFFu ? 0u : state, last_song.c_str(),
            last_artist.c_str());
    fclose(f);
    rename(tmp.c_str(), path);
}

void media_status(uint16_t id, const uint8_t* b, size_t len, void*)
{
    static uint32_t last_state = 0xFFFFFFFFu;
    if (id == 0x8001) { // MediaPlaybackStatus {state=1}
        long long x;
        if (pb_int(b, len, 1, &x)) {
            const uint32_t st = (uint32_t)x;
            if (st != last_state) {
                last_state = st;
                if (FILE* mf = fopen("/fs/ram3/nbt-aa-media", "w")) {
                    fprintf(mf, "%u\n", st);
                    fclose(mf);
                }
                write_now_playing(0, 0, st);
            }
        }
    } else if (id == 0x8003 && len > 0) { // MediaPlaybackMetadata {song=1, artist=2}
        const std::string song = pb_str(b, len, 1);
        const std::string artist = pb_str(b, len, 2);
        if (!song.empty() || !artist.empty()) {
            printf("[AASession] now playing: '%s' - '%s'\n", song.c_str(), artist.c_str());
            write_now_playing(&song, &artist, last_state);
        }
    }
}

} // namespace

namespace {

// ===== video: session thread -> queue -> writer thread -> decoder FIFO =================================================
//
// Live mode (car 2026-11-07): writing H.264 into the decoder FIFO directly from the AA session thread blocked that thread
// whenever the decoder fell behind (up to 2 s per frame). Media acks and controller keys then queued behind it (3-4 s input
// lag), the phone throttled the video and finally ended the session. Now the session thread only queues the access unit;
// a writer thread feeds the FIFO. When the queue exceeds its budget the frames are dropped up to the next IDR picture, so
// the decoder always resumes on a clean picture and the session thread never waits.
struct LiveItem {
    std::vector<uint8_t> data;
    uint64_t token;
    explicit LiveItem(uint64_t t) : token(t) {}
};

struct LiveQueue {
    pthread_mutex_t m;
    pthread_cond_t cv;
    std::deque<LiveItem> items;             // H.264 access units
    size_t bytes;                            // total size of items
    bool stop, dropping, paused, need_idr, condition_ready;
    std::vector<uint8_t> recovery_config;
    unsigned dropped, drops;                 // dropped frames, number of drop runs
    LiveQueue() : bytes(0), stop(false), dropping(false), paused(false), need_idr(false), condition_ready(false),
                  dropped(0), drops(0)
    {
        pthread_mutex_init(&m, 0);
        pthread_condattr_t attr;
        int rc = pthread_condattr_init(&attr);
        if (!rc) {
            rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
            if (!rc)
                rc = pthread_cond_init(&cv, &attr);
            pthread_condattr_destroy(&attr);
        }
        condition_ready = rc == 0;
    }
    ~LiveQueue()
    {
        if (condition_ready)
            pthread_cond_destroy(&cv);
        pthread_mutex_destroy(&m);
    }
};
const size_t kLiveBudget = 3 * 1024 * 1024; // queue limit in bytes

struct Capture {
    pthread_mutex_t mutex;
    nbt_capture_transfer transfer; // USB transfer callback from nbt_android_auto.c
    void* transport;
    int socket;      // our end of the socket pair to the session
    int data;        // capture file or live FIFO
    int index;       // capture mode: .csv index
    int error;
    bool stop_reader;
    const char* reader_error;
    unsigned read_size;
    uint64_t bytes;  // unpadded input bytes in live mode, bytes written in diagnostic mode
    unsigned frames, configs, frame_limit;
    unsigned received; // every video frame from the phone, both modes (freeze watch; frames counts capture mode only)
    long long max_usb_in_ms, max_usb_to_socket_ms, max_usb_out_ms;
    long long usb_in_started, usb_to_socket_started, usb_out_started;
    bool live, native_focus_pending;
    std::string video_token;
    const char* video_control;
    const char* video_status;
    std::string live_fifo;
    nbt::AASession* session;
    LiveQueue queue;
    Capture() : socket(-1), data(-1), index(-1), error(0), stop_reader(false), reader_error(0),
                read_size(NBT_CAPTURE_MAX_TRANSFER), bytes(0), frames(0), configs(0), frame_limit(300), received(0),
                max_usb_in_ms(0), max_usb_to_socket_ms(0), max_usb_out_ms(0),
                usb_in_started(-1), usb_to_socket_started(-1), usb_out_started(-1),
                live(false), native_focus_pending(false), video_control(NBT_VIDEO_CONTROL),
                video_status(NBT_VIDEO_STATUS), session(0)
    {
        pthread_mutex_init(&mutex, 0);
    }
    ~Capture() { pthread_mutex_destroy(&mutex); }
};

// Annex-B access unit contains an IDR slice (NAL type 5)?
bool has_nal(const uint8_t* p, size_t n, unsigned type)
{
    for (size_t i = 0; i + 3 < n; ++i)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == type)
            return true;
    return false;
}

bool has_idr(const uint8_t* p, size_t n) { return has_nal(p, n, 5); }

void video_consumed(Capture* c, uint64_t token)
{
    if (c->session && token)
        c->session->postVideoConsumed(token);
}

// Caller holds the live queue mutex.
void clear_live_queue(Capture* c)
{
    LiveQueue& q = c->queue;
    while (!q.items.empty()) {
        video_consumed(c, q.items.front().token);
        q.items.pop_front();
    }
    q.bytes = 0;
}

// session thread: never blocks on the decoder
void live_enqueue(Capture* c, const uint8_t* p, size_t n, bool config, uint64_t token)
{
    LiveQueue& q = c->queue;
    pthread_mutex_lock(&q.m);
    if (config) {
        q.recovery_config.clear();
        if (n <= 65536 && has_nal(p, n, 7) && has_nal(p, n, 8))
            q.recovery_config.assign(p, p + n);
    }
    if (q.paused || (q.need_idr && (config || !has_idr(p, n) || q.recovery_config.empty()))) {
        ++q.dropped;
        pthread_mutex_unlock(&q.m);
        video_consumed(c, token);
        return;
    }
    const bool recovering = q.need_idr;
    if (recovering) {
        clear_live_queue(c);
        q.need_idr = false;
        q.dropping = false;
    }
    // NBT_AA_PAD (bytes, car 2026-11-07): a static Android Auto screen is only ~600 bytes per frame (~16 KB/s), so any
    // byte-sized input buffer of the decoder chain (HTTP client, demuxer) delays the picture by seconds. Each access
    // unit is padded with one H.264 filler NAL (type 12, ignored by decoders) up to this size.
    static long pad = -1;
    if (pad < 0) {
        const char* e = getenv("NBT_AA_PAD");
        pad = e ? strtol(e, 0, 10) : 0;
        if (pad < 0 || pad > 65536)
            pad = 0;
    }
    const size_t padded_size = !config && pad > 0 && n + 6 < (size_t)pad ? (size_t)pad : n;
    const bool over_budget = q.bytes + padded_size > kLiveBudget;
    const bool restart = config || ((q.dropping || over_budget) && has_idr(p, n));
    if (q.dropping && !restart) {
        ++q.dropped;
        pthread_mutex_unlock(&q.m);
        video_consumed(c, token);
        return;
    }
    if (over_budget) {
        if (!restart) {
            q.dropping = true;
            ++q.drops;
            ++q.dropped;
            pthread_mutex_unlock(&q.m);
            video_consumed(c, token);
            return;
        }
        // a fresh IDR/config does not fit: throw away the old backlog, the decoder restarts on this picture
        while (!q.items.empty() && q.bytes + padded_size > kLiveBudget) {
            q.bytes -= q.items.front().data.size();
            video_consumed(c, q.items.front().token);
            q.items.pop_front();
            ++q.dropped;
        }
    }
    q.dropping = false;
    q.items.push_back(LiveItem(token));
    std::vector<uint8_t>& v = q.items.back().data;
    v.reserve(padded_size + (recovering ? q.recovery_config.size() : 0));
    if (recovering)
        v.insert(v.end(), q.recovery_config.begin(), q.recovery_config.end());
    v.insert(v.end(), p, p + n);
    if (padded_size > n) {
        const size_t fill = padded_size - n - 6;
        static const uint8_t head[5] = {0, 0, 0, 1, 0x0c}; // start code + NAL header type 12 (filler data)
        v.insert(v.end(), head, head + 5);
        v.insert(v.end(), fill, (uint8_t)0xff);
        v.push_back(0x80); // rbsp trailing bits
    }
    q.bytes += v.size();
    pthread_cond_signal(&q.cv);
    pthread_mutex_unlock(&q.m);
}

long long now_ms();

// write everything; a non-blocking descriptor may stay full for at most 2 s
bool write_all(int fd, const unsigned char* p, size_t n)
{
    long long deadline = 0;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && errno == EAGAIN) {
            if (!deadline)
                deadline = now_ms() + 2000;
            if (now_ms() >= deadline)
                return false;
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            timeval t = {0, 100000};
            if (select(fd + 1, 0, &set, 0, &t) < 0 && errno != EINTR)
                return false;
            continue;
        }
        if (k <= 0)
            return false;
        p += k;
        n -= k;
    }
    return true;
}

// one H.264 access unit (or codec config) from the session
void save(Capture* c, const uint8_t* p, size_t n, uint64_t pts, bool config, uint64_t token = 0)
{
    bool queued = false;
    pthread_mutex_lock(&c->mutex);
    if (!c->error && c->frames < c->frame_limit) {
        const bool annexb = n >= 4 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1));
        if (!annexb || n > 2 * 1024 * 1024 || (!c->live && n > 8 * 1024 * 1024 - c->bytes))
            c->error = 1;
        else {
            bool accepted = true;
            if (!config)
                ++c->received;
            if (c->live) {
                // Count valid input even when backpressure drops it; queue drops are logged separately.
                live_enqueue(c, p, n, config, token);
                queued = true;
            } else {
                accepted = write_all(c->data, p, n);
                if (accepted && c->index >= 0) {
                    char row[160];
                    int k = snprintf(row, sizeof(row), "%s,%u,%llu,%u,%u\n", config ? "config" : "frame", c->frames,
                                     (unsigned long long)pts, (unsigned)c->bytes, (unsigned)n);
                    accepted = k > 0 && (size_t)k < sizeof(row) && write_all(c->index, (const unsigned char*)row, k);
                }
            }
            if (!accepted)
                c->error = 1;
            else {
                c->bytes += n;
                if (config)
                    ++c->configs;
                else
                    ++c->frames;
            }
        }
    }
    pthread_mutex_unlock(&c->mutex);
    if (!queued)
        video_consumed(c, token);
}

// AASession video frame callback
void video(uint32_t, std::vector<uint8_t>& frame_data, size_t data_offset, size_t data_len, uint64_t pts, void* v)
{
    Capture* c = (Capture*)v;
    const uint64_t token = c->session ? c->session->currentVideoToken() : 0;
    if (data_offset <= frame_data.size() && data_len <= frame_data.size() - data_offset)
        save(c, data_len ? &frame_data[data_offset] : 0, data_len, pts, false, token);
    else
        video_consumed(c, token);
}

// Android Auto sound into the car (2026-11-18, aa_media_out.hpp): the service sets NBT_AA_MEDIA_OUT to the io-amp input
// of the Android Auto audio source (input_stereo_1 = connection f6) and offers the media channel to the phone.
void media_audio(uint8_t channel, const uint8_t* pcm, size_t len, uint64_t, void*)
{
    nbt::mediaout::onAudio(channel, pcm, len);
}

void media_state(uint8_t channel, bool started, uint32_t rate, uint32_t channels, void*)
{
    nbt::mediaout::onState(channel, started, rate, channels);
}

void media_consumed(uint8_t channel, size_t bytes, void* session)
{
    static_cast<nbt::AASession*>(session)->postAudioConsumed(channel, bytes);
}

// The session callback runs on the session thread; the capture pump owns the handoff and focus indications.
void focus_request(bool projected, uint32_t reason, void* user)
{
    if (projected)
        return;
    Capture* c = static_cast<Capture*>(user);
    pthread_mutex_lock(&c->mutex);
    c->native_focus_pending = true;
    pthread_mutex_unlock(&c->mutex);
    printf("AA_FOCUS phone requested native screen (reason %u)\n", (unsigned)reason);
}

const char* const kExitRequest = "/fs/ram3/nbt-aa-exit-to-idrive";

struct FocusHandoff {
    bool pending;
    long long deadline, hidden_since;
    FocusHandoff() : pending(false), deadline(0), hidden_since(0) {}

    // Returns true if the caller should restore projected focus immediately.
    bool request(long long now, bool visible, const char* path) {
        if (pending)
            return false;
        if (now < 0) {
            printf("AA_FOCUS cannot request iDrive handoff: monotonic clock failed\n");
            return true;
        }
        if (visible) {
            const std::string tmp = std::string(path) + ".tmp";
            int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) {
                printf("AA_FOCUS cannot request iDrive handoff: errno=%d\n", errno);
                return true;
            }
            char msg[40];
            const int len = snprintf(msg, sizeof(msg), "%lld\n", now + 4000);
            const bool written = len > 0 && len < (int)sizeof(msg) && write(fd, msg, len) == len;
            const int saved_errno = errno;
            const bool closed = close(fd) == 0;
            if (!written || !closed || rename(tmp.c_str(), path) != 0) {
                printf("AA_FOCUS cannot publish iDrive handoff: errno=%d\n", written ? errno : saved_errno);
                unlink(tmp.c_str());
                return true;
            }
        }
        pending = true;
        deadline = now + 4000;
        hidden_since = visible ? 0 : now;
        return false;
    }

    // Keep the decoder fed once the overlay has actually hidden. With the opt-in background-focus mode, leave native
    // focus in place until the page is opened again. The timeout avoids stranding the decoder if the HMI ignores us.
    bool tick(long long now, bool visible, bool bg_focus, int& shown_state, const char* path) {
        if (!pending)
            return false;
        if (now < 0) {
            pending = false;
            unlink(path);
            printf("AA_FOCUS handoff interrupted: monotonic clock failed\n");
            return true;
        }
        if (!visible && !hidden_since)
            hidden_since = now;
        if (visible && hidden_since) {
            pending = false;
            unlink(path);
            shown_state = 1;
            printf("AA_FOCUS Android Auto reopened during handoff\n");
            return true;
        }
        if (!visible) {
            if (now - hidden_since >= 500) {
                pending = false;
                unlink(path);
                shown_state = 0;
                printf("AA_FOCUS iDrive visible; %s\n", bg_focus ? "keeping native focus" : "resuming background video");
                return !bg_focus;
            }
            return false;
        }
        if (now >= deadline) {
            pending = false;
            unlink(path);
            printf("AA_FOCUS iDrive handoff timed out; restoring projection\n");
            return true;
        }
        return false;
    }
};

// AASession codec config callback (SPS/PPS)
void config(const uint8_t* p, size_t n, void* v)
{
    save((Capture*)v, p, n, 0, true);
}

// reader thread: USB -> socket pair (the session reads the other end)
bool reader_stopping(Capture* c, const char* failure = 0)
{
    pthread_mutex_lock(&c->mutex);
    const bool stop = c->stop_reader;
    if (!stop && failure)
        c->reader_error = failure;
    pthread_mutex_unlock(&c->mutex);
    return stop;
}

long long now_ms();
void bridge_begin(Capture* c, long long& started)
{
    const long long now = now_ms();
    pthread_mutex_lock(&c->mutex);
    started = now;
    pthread_mutex_unlock(&c->mutex);
}

void bridge_end(Capture* c, long long& maximum, long long& started)
{
    const long long now = now_ms();
    pthread_mutex_lock(&c->mutex);
    if (started >= 0 && now >= started && now - started > maximum)
        maximum = now - started;
    started = -1;
    pthread_mutex_unlock(&c->mutex);
}

long long in_flight_ms(long long started, long long now)
{
    return started >= 0 && now >= started ? now - started : -1;
}

void bridge_snapshot(Capture* c)
{
    long long max_in, max_handoff, max_out, in, handoff, out;
    const long long now = now_ms();
    pthread_mutex_lock(&c->mutex);
    max_in = c->max_usb_in_ms;
    max_handoff = c->max_usb_to_socket_ms;
    max_out = c->max_usb_out_ms;
    in = in_flight_ms(c->usb_in_started, now);
    handoff = in_flight_ms(c->usb_to_socket_started, now);
    out = in_flight_ms(c->usb_out_started, now);
    pthread_mutex_unlock(&c->mutex);
    printf("AA_CAPTURE_BRIDGE_SNAPSHOT usb_in_max_ms=%lld usb_to_session_max_ms=%lld usb_out_max_ms=%lld "
           "usb_in_inflight_ms=%lld usb_to_session_inflight_ms=%lld usb_out_inflight_ms=%lld\n",
           max_in, max_handoff, max_out, in, handoff, out);
}

void report_stall(Capture* c, nbt_capture_snapshot snapshot, void* context, const char* path)
{
    bridge_snapshot(c);
    if (snapshot)
        snapshot(context);
    if (fflush(stdout))
        perror("AA_STALL snapshot flush");
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
    else
        perror("AA_STALL marker");
}

bool write_session_socket(Capture* c, const unsigned char* p, size_t n)
{
    const long long start = now_ms();
    if (start < 0)
        return false;
    const long long deadline = start + 2000;
    while (n && !reader_stopping(c)) {
        const long long now = now_ms();
        if (now < 0 || now >= deadline) {
            printf("AA_CAPTURE_PUMP_ERROR operation=usb_to_session_socket cause=%s uptime_ms=%lld\n",
                   now < 0 ? "clock" : "backpressure_timeout", now);
            return false;
        }
        ssize_t written = send(c->socket, p, n, 0);
        if (written > 0) {
            p += written;
            n -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(c->socket, &set);
            timeval wait = {0, 100000};
            if (select(c->socket + 1, 0, &set, 0, &wait) < 0 && errno != EINTR) {
                printf("AA_CAPTURE_PUMP_ERROR operation=usb_to_session_socket cause=select errno=%d uptime_ms=%lld\n",
                       errno, now_ms());
                return false;
            }
            continue;
        }
        printf("AA_CAPTURE_PUMP_ERROR operation=usb_to_session_socket cause=send errno=%d uptime_ms=%lld\n",
               written < 0 ? errno : 0, now_ms());
        return false;
    }
    return n == 0;
}

void* reader(void* v)
{
    Capture* c = (Capture*)v;
    unsigned char b[NBT_CAPTURE_MAX_TRANSFER];
    while (!reader_stopping(c)) {
        unsigned n = c->read_size;
        bridge_begin(c, c->usb_in_started);
        const bool ok = c->transfer(c->transport, 1, b, &n);
        bridge_end(c, c->max_usb_in_ms, c->usb_in_started);
        const char* failure = !ok ? "usb_in" : n > c->read_size ? "usb_in_invalid_size" : 0;
        if (reader_stopping(c, failure) || failure)
            break;
        // A successful zero-length USB packet contributes no bytes to the AA stream; it is not socket EOF.
        if (!n)
            usleep(1000);
        else {
            bridge_begin(c, c->usb_to_socket_started);
            const bool written = write_session_socket(c, b, n);
            bridge_end(c, c->max_usb_to_socket_ms, c->usb_to_socket_started);
            if (!written) {
                reader_stopping(c, "usb_to_session_socket");
                break;
            }
        }
    }
    shutdown(c->socket, SHUT_RDWR);
    return 0;
}

long long now_ms()
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void pause_live_writer(Capture* c)
{
    if (c->data >= 0) {
        close(c->data);
        c->data = -1;
    }
    pthread_mutex_lock(&c->queue.m);
    c->queue.paused = true;
    c->queue.need_idr = false;
    clear_live_queue(c);
    pthread_mutex_unlock(&c->queue.m);
}

bool video_writer_status(Capture* c, unsigned generation, const char* state)
{
    printf("AA_VIDEO_RECOVERY session=%s generation=%u state=%s uptime_ms=%lld\n",
           c->video_token.c_str(), generation, state, now_ms());
    if (video_recovery_publish(c->video_status, c->video_token.c_str(), generation, state))
        return true;
    printf("AA_VIDEO_RECOVERY_ERROR operation=publish errno=%d uptime_ms=%lld\n", errno, now_ms());
    pthread_mutex_lock(&c->mutex);
    c->error = 1;
    pthread_mutex_unlock(&c->mutex);
    return false;
}

// phase: 0 streaming, 1 paused, 2 waiting for the first fresh IDR, 3 peer lost.
bool video_writer_control(Capture* c, unsigned& generation, int& phase)
{
    unsigned requested;
    char state[32];
    if (c->video_token.empty() ||
        !video_recovery_read(c->video_control, c->video_token.c_str(), &requested, state, sizeof(state)))
        return true;
    if (!strcmp(state, "pause") && requested > generation) {
        pause_live_writer(c);
        generation = requested;
        phase = 1;
        return video_writer_status(c, generation, "paused");
    }
    if (phase == 1 && requested == generation && !strcmp(state, "ready")) {
        struct stat st;
        int error = 0;
        int fd = open(c->live_fifo.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd < 0 && (errno == ENXIO || errno == EINTR))
            return true;
        if (fd < 0)
            error = errno;
        else if (fstat(fd, &st))
            error = errno;
        else if (!S_ISFIFO(st.st_mode))
            error = EINVAL;
        if (error) {
            if (fd >= 0)
                close(fd);
            printf("AA_VIDEO_RECOVERY_ERROR session=%s generation=%u operation=reopen errno=%d uptime_ms=%lld\n",
                   c->video_token.c_str(), generation, error, now_ms());
            phase = 3;
            return video_writer_status(c, generation, "failed");
        }
        c->data = fd;
        pthread_mutex_lock(&c->queue.m);
        c->queue.paused = false;
        c->queue.need_idr = true;
        pthread_mutex_unlock(&c->queue.m);
        phase = 2;
        return video_writer_status(c, generation, "opened");
    }
    return true;
}

void* live_writer(void* v)
{
    Capture* c = (Capture*)v;
    LiveQueue& q = c->queue;
    unsigned generation = 0;
    int phase = 0;
    for (;;) {
        if (!video_writer_control(c, generation, phase))
            return 0;
        pthread_mutex_lock(&q.m);
        if (q.stop) {
            pthread_mutex_unlock(&q.m);
            return 0;
        }
        if (q.items.empty() || q.paused) {
            struct timespec deadline;
            if (clock_gettime(CLOCK_MONOTONIC, &deadline)) {
                pthread_mutex_unlock(&q.m);
                pthread_mutex_lock(&c->mutex);
                c->error = 1;
                pthread_mutex_unlock(&c->mutex);
                printf("AA_LIVE_WRITER_FAILED operation=clock errno=%d\n", errno);
                return 0;
            }
            deadline.tv_nsec += 100000000;
            if (deadline.tv_nsec >= 1000000000) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&q.cv, &q.m, &deadline);
            pthread_mutex_unlock(&q.m);
            continue;
        }
        LiveItem item(0);
        item.data.swap(q.items.front().data);
        item.token = q.items.front().token;
        q.items.pop_front();
        q.bytes -= item.data.size();
        pthread_mutex_unlock(&q.m);
        size_t off = 0;
        while (off < item.data.size()) {
            const unsigned previous_generation = generation;
            if (!video_writer_control(c, generation, phase))
                return 0;
            if (generation != previous_generation || phase == 1 || phase == 3)
                break;
            ssize_t k = write(c->data, &item.data[off], item.data.size() - off);
            if (k > 0) {
                off += (size_t)k;
                continue;
            }
            if (k < 0 && errno == EINTR)
                continue;
            if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pthread_mutex_lock(&q.m);
                bool stop = q.stop;
                pthread_mutex_unlock(&q.m);
                if (stop) {
                    video_consumed(c, item.token);
                    return 0;
                }
                fd_set set;
                FD_ZERO(&set);
                FD_SET(c->data, &set);
                timeval t = {0, 100000};
                select(c->data + 1, 0, &set, 0, &t);
                continue;
            }
            const int error = k < 0 ? errno : 0;
            printf("AA_LIVE_WRITER_FAILED errno=%d uptime_ms=%lld\n", error, now_ms());
            if (error == EPIPE && !c->video_token.empty()) {
                pause_live_writer(c);
                phase = 3;
                if (!video_writer_status(c, generation, "fault"))
                    return 0;
                break;
            }
            pthread_mutex_lock(&c->mutex);
            c->error = 1;
            pthread_mutex_unlock(&c->mutex);
            return 0;
        }
        video_consumed(c, item.token);
        if (off == item.data.size() && phase == 2) {
            phase = 0;
            if (!video_writer_status(c, generation, "running"))
                return 0;
        }
    }
}

volatile sig_atomic_t live_stop = 0;

void stop_live(int signo)
{
    live_stop = signo;
}

// ===== iDrive controller input (live mode) =============================================================================
//
// nbt_knob writes "R <key> <delta>" (rotary) / "K <key> <0|1>" (key up/down) lines into this FIFO.
// Opened O_RDWR so the FIFO never reports EOF when nbt_knob is not running. Only the keys declared on input channel 7
// are accepted: HOME 3, BACK 4, CALL 5, DPAD 19..23, SEARCH 84, ROTARY 65536, MEDIA 65537, NAVIGATION 65538, and the
// media keys PLAY_PAUSE 85, NEXT 87, PREVIOUS 88 (steering wheel, 2026-11-12; all in the DHU template's keycodes_supported).
const char* const kInputFifo = "/fs/ram3/nbt-aa-native/input.fifo";

// Car day/night for Android Auto's night mode (dark maps at night). This NBT has no day/night car data property, so the
// headlights stand in: the HMI plugin writes CDS controls.headlights to this file as '{"headlights":N}'; 0 = day,
// anything else = night. Returns 1 night, 0 day, -1 unknown (no file, or an error answer: keep what the phone has).
const char* const kHeadlightsFile = "/fs/ram3/nbt-aa-headlights";
int car_night()
{
    char b[96];
    const int fd = open(kHeadlightsFile, O_RDONLY);
    if (fd < 0)
        return -1;
    const ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    b[n] = 0;
    const char* p = strstr(b, "\"headlights\":");
    if (!p)
        return -1;
    p += 13;
    if (*p < '0' || *p > '9')
        return -1;
    return atoi(p) != 0 ? 1 : 0;
}

bool allowed_key(unsigned k)
{
    return k == 3 || k == 4 || k == 5 || k == 6 || (k >= 19 && k <= 23) || k == 84 || k == 85 || k == 87 || k == 88 ||
           k == 65536 || k == 65537 || k == 65538;
}

struct InputLines {
    char b[256]; // partial line buffer
    size_t n;
    InputLines() : n(0) {}
};

void handle_input(int fd, InputLines& in, nbt::AASession& session)
{
    for (;;) {
        if (in.n >= sizeof(in.b))
            in.n = 0; // overlong garbage: drop it
        ssize_t r = read(fd, in.b + in.n, sizeof(in.b) - in.n);
        if (r <= 0)
            return;
        in.n += (size_t)r;
        char* start = in.b;
        char* nl;
        while ((nl = (char*)memchr(start, '\n', in.n - (size_t)(start - in.b)))) {
            *nl = 0;
            char kind = 0;
            unsigned key = 0;
            int value = 0;
            if (sscanf(start, "%c %u %d", &kind, &key, &value) == 3 && kind == 'V' && key <= 100) {
                aa_audio::setVolume((int)key);
                nbt::mediaout::setVolume((int)key);
            }
            else if (sscanf(start, "%c %u %d", &kind, &key, &value) == 3 && allowed_key(key)) {
                if (kind == 'R' && key == 65536 && value >= -20 && value <= 20 && value)
                    session.postRelativeEvent(key, value);
                else if (kind == 'K' && (value == 0 || value == 1))
                    session.postKeyEvent(key, value == 1);
            }
            start = nl + 1;
        }
        size_t rest = in.n - (size_t)(start - in.b);
        memmove(in.b, start, rest);
        in.n = rest;
    }
}

} // namespace

// ===== entry point (called by nbt_android_auto.c once the phone is in accessory mode) =======================================
//
// transfer: USB bulk transfer callback (direction 0 = to the phone, 1 = from the phone); context: its transport.
// prefix:   capture mode file prefix (must be under /fs/ram3/nbt-aa- when running as root).
// Returns 0 = session received valid config and video without a capture or pump error, 2 = bad arguments / files,
// 3 = failure.
extern "C" int nbt_video_capture_run(nbt_capture_transfer transfer, nbt_capture_cancel cancel,
                                     nbt_capture_stop_requested stop_requested, nbt_capture_snapshot snapshot,
                                     void* context, const char* prefix, unsigned read_size)
{
    if (!transfer || !prefix || !*prefix || strlen(prefix) > 200 || !read_size || read_size > NBT_CAPTURE_MAX_TRANSFER)
        return 2;
    if (geteuid() == 0 && strncmp(prefix, "/fs/ram3/nbt-aa-", sizeof("/fs/ram3/nbt-aa-") - 1))
        return 2;
    signal(SIGPIPE, SIG_IGN);
    Capture c;
    if (!c.queue.condition_ready) {
        puts("AA_VIDEO_CAPTURE_START_FAILED cannot initialize video queue condition");
        return 3;
    }
    c.transfer = transfer;
    c.transport = context;
    c.read_size = read_size;
    unsigned seconds = 15;
    const char* live_fifo = getenv("NBT_AA_LIVE_FIFO");
    const char* requested = getenv("NBT_AA_CAPTURE_SECONDS");
    if (requested) {
        char* end = 0;
        long n = strtol(requested, &end, 10);
        if (!*requested || *end || n < 1 || n > 60)
            return 2;
        seconds = (unsigned)n;
        c.frame_limit = seconds * 30;
    }
    const std::string name(prefix);

    // ---- output: live FIFO (only the one native_service creates) or capture files ----
    if (live_fifo) {
        if (strcmp(live_fifo, "/fs/ram3/nbt-aa-native/annexb.fifo"))
            return 2;
        struct stat st;
        if (lstat(live_fifo, &st) || !S_ISFIFO(st.st_mode))
            return 2;
        c.data = open(live_fifo, O_WRONLY | O_NONBLOCK);
        if (c.data >= 0) {
            struct stat opened;
            if (fstat(c.data, &opened) || !S_ISFIFO(opened.st_mode)) {
                close(c.data);
                return 2;
            }
        }
        c.live = true;
        c.live_fifo = live_fifo;
        const char* video_token = getenv("NBT_AA_VIDEO_SESSION");
        if (video_token)
            c.video_token = video_token;
        c.frame_limit = ~0u;
        seconds = 0;
        live_stop = 0;
        signal(SIGTERM, stop_live);
        signal(SIGINT, stop_live);
    } else
        c.data = open((name + ".h264").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (c.data < 0)
        return 2;
    if (!c.live)
        c.index = open((name + ".csv").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (!c.live && c.index < 0) {
        close(c.data);
        return 2;
    }

    // ---- session over a socket pair: pair[0] for the session, pair[1] for our USB pump ----
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
        close(c.data);
        close(c.index);
        return 2;
    }
    c.socket = pair[1];
    const int socket_flags = fcntl(c.socket, F_GETFL);
    if (socket_flags < 0 || fcntl(c.socket, F_SETFL, socket_flags | O_NONBLOCK) < 0) {
        printf("AA_CAPTURE_PUMP_ERROR operation=socket_nonblocking errno=%d\n", errno);
        close(pair[0]);
        close(pair[1]);
        close(c.data);
        close(c.index);
        return 3;
    }
    nbt::AASession session;
    session.setVideoFrameCallback(video, &c);
    session.setCodecConfigCallback(config, &c);
    bool media_audio_started = false;
    if (c.live) {
        c.session = &session;
        const char* video_gate = getenv("NBT_AA_VIDEO_GATE");
        session.setVideoAckGate(!(video_gate && !strcmp(video_gate, "0")));
        session.setNavStatusCallback(nav_status, 0);
        session.setMediaStatusCallback(media_status, 0);
        unlink("/fs/ram3/nbt-aa-native/nav.state");
        unlink(kExitRequest);
        session.setVideoFocusCallback(focus_request, &c);
        const char* media_out = getenv("NBT_AA_MEDIA_OUT");
        if (media_out && *media_out) {
            nbt::mediaout::setConsumedCallback(media_consumed, &session);
            if (nbt::mediaout::start(media_out)) {
                media_audio_started = true;
                FILE* v = fopen("/fs/ram3/nbt-aa-native/volume", "r");
                if (v) {
                    int pct;
                    if (fscanf(v, "%d", &pct) == 1) nbt::mediaout::setVolume(pct);
                    fclose(v);
                }
                // acks once played: the phone sends at the speed the car plays (no queue growing, no cuts)
                session.setDeferredAudioAcks(true);
                session.setAudioFrameCallback(media_audio, 0);
                session.setAudioStateCallback(media_state, 0);
                printf("AA_MEDIA_OUT Android Auto sound -> %s\n", media_out);
            } else {
                printf("AA_MEDIA_OUT not available: no Android Auto sound in the car this session\n");
                FILE* f = fopen("/fs/ram3/nbt-aa-native/audio-output.failed", "w");
                if (f) { fprintf(f, "A2I media output not loaded\n"); fclose(f); }
                else perror("AA_MEDIA_OUT cannot record output failure");
            }
        }
    }
    const char* configured_media_out = getenv("NBT_AA_MEDIA_OUT");
    const bool audio = media_audio_started ||
                       (c.live && (!configured_media_out || !*configured_media_out) && aa_audio::setup(session));
    setenv("NBT_VID_RES", "1", 0); // 800x480 unless the service asks for 1280x720 (res720)
    // Day/night before the session starts: the phone asks for the NIGHT_MODE sensor early and gets this value
    int night_posted = c.live ? car_night() : -1;
    if (night_posted >= 0) {
        session.postNightMode(night_posted != 0);
        printf("AA_NIGHT car is in %s mode at session start\n", night_posted ? "night" : "day");
    }
    long long next_night_check = 0;
    printf("AA_VIDEO_CAPTURE_BEGIN max_seconds=%u max_bytes=8388608 max_frames=%u no_display=1 no_audio_playback=%d\n",
           seconds, c.frame_limit, audio ? 0 : 1);
    if (!session.start(pair[0])) {
        nbt::mediaout::stop();
        aa_audio::shutdown();
        close(pair[1]);
        close(c.data);
        close(c.index);
        return 3;
    }
    pthread_t thread, writer;
    int thread_error;
    bool have_writer = false;
    if (c.live) {
        if ((thread_error = pthread_create(&writer, 0, live_writer, &c)) != 0) {
            printf("AA_VIDEO_CAPTURE_START_FAILED worker=live_writer rc=%d\n", thread_error);
            nbt::mediaout::stop();
            session.stop();
            aa_audio::shutdown();
            close(pair[1]);
            close(c.data);
            return 3;
        }
        have_writer = true;
    }
    if ((thread_error = pthread_create(&thread, 0, reader, &c)) != 0) {
        printf("AA_VIDEO_CAPTURE_START_FAILED worker=usb_reader rc=%d\n", thread_error);
        if (have_writer) {
            pthread_mutex_lock(&c.queue.m);
            c.queue.stop = true;
            pthread_cond_signal(&c.queue.cv);
            pthread_mutex_unlock(&c.queue.m);
            pthread_join(writer, 0);
        }
        nbt::mediaout::stop();
        session.stop();
        aa_audio::shutdown();
        close(pair[1]);
        close(c.data);
        close(c.index);
        return 3;
    }
    int input = -1;
    InputLines lines;
    if (c.live) {
        struct stat st;
        if (lstat(kInputFifo, &st) == 0 && S_ISFIFO(st.st_mode))
            input = open(kInputFifo, O_RDWR | O_NONBLOCK);
        printf("AA_INPUT %s\n", input >= 0 ? "controller FIFO open" : "no controller FIFO (video only)");
    }
    // Optional (NBT_AA_BG_FOCUS=1, off until car-tested): while the Android Auto page is not on screen
    // (/fs/ram3/nbt-aa-hidden) tell the phone it lost the screen (video focus NATIVE): it pauses the video and sends a
    // fresh key frame when the page comes back. Off: the phone keeps streaming in the background (proven path).
    const char* bg = getenv("NBT_AA_BG_FOCUS");
    const bool bg_focus = c.live && bg && !strcmp(bg, "1");
    int shown_state = 1;
    unsigned refreshed_generation = 0;
    long long next_focus_check = 0;
    FocusHandoff handoff;
    const long long start = now_ms();
    // Clean end (car 2026-11-10): an abrupt end (service restart, decoder gone) left Android Auto on the phone stuck until
    // it was force-stopped ("Invalid channel ... premature encryption" right after the next VersionRequest). Before
    // stopping, a ByeByeRequest goes out and the session keeps pumping both directions for up to 1 s until the phone's
    // ByeByeResponse ends it.
    long long bye_until = 0;
    const char* bye_reason = 0;
    const char* stop_reason = start < 0 ? "clock_error" : "session_inactive";
    bool pump_error = start < 0;
    // Freeze watch (user report: "the Android Auto screen freezes sometimes"): no new video frame for 15 s while the
    // page is on screen -> AA_STALL in this log and the flag file below; native_service saves an incident snapshot.
    // (The phone may also send nothing for a still screen, so this is evidence for the log, not an action.)
    unsigned stall_frames = 0;
    long long stall_since = now_ms(), next_stall_check = 0;
    bool stall_reported = false;

    // ---- main loop: socket pair -> USB, controller FIFO -> session, focus and shutdown handling ----
    while (start >= 0 && session.isActive()) {
        pthread_mutex_lock(&c.mutex);
        const bool output_error = c.error;
        const bool frame_limit = c.frames >= c.frame_limit;
        pthread_mutex_unlock(&c.mutex);
        if (stop_requested && stop_requested(context)) {
            stop_reason = "usb_removed";
            pump_error = true;
            break;
        }
        if (!bye_until) {
            const int signo = live_stop;
            if (signo)
                bye_reason = "signal";
            else if (output_error)
                bye_reason = "capture_error";
            else if (frame_limit)
                bye_reason = "frame_limit";
            else if (!c.live && now_ms() - start >= seconds * 1000LL)
                bye_reason = "time_limit";
            if (bye_reason) {
                printf("AA_BYEBYE requested reason=%s signal=%d uptime_ms=%lld\n", bye_reason, signo, now_ms());
                session.byeAndWait(0);
                bye_until = now_ms() + 1000;
            }
        }
        if (bye_until && now_ms() >= bye_until) {
            printf("AA_BYEBYE no answer within 1 s\n");
            stop_reason = "byebye_timeout";
            break;
        }
        if (c.live && !bye_until && now_ms() >= next_stall_check) {
            next_stall_check = now_ms() + 1000;
            pthread_mutex_lock(&c.mutex);
            // Count received frames even when backpressure drops them.
            const unsigned f = c.received;
            pthread_mutex_unlock(&c.mutex);
            const bool visible = access("/fs/ram3/nbt-aa-hidden", F_OK) != 0;
            if (f != stall_frames || !visible) {
                if (stall_reported && f != stall_frames)
                    printf("AA_STALL ended: video again after %lld s\n", (now_ms() - stall_since) / 1000);
                stall_frames = f;
                stall_since = now_ms();
                stall_reported = false;
            } else if (!stall_reported && now_ms() - stall_since >= 15000) {
                stall_reported = true;
                printf("AA_STALL no video frame for 15 s while the page is on screen (frames %u)\n", f);
                report_stall(&c, snapshot, context, "/fs/ram3/nbt-aa-native/stall");
            }
        }
        if (c.live && !bye_until && now_ms() >= next_night_check) {
            next_night_check = now_ms() + 1000;
            const int n = car_night();
            if (n >= 0 && n != night_posted) {
                night_posted = n;
                session.postNightMode(n != 0);
                printf("AA_NIGHT car headlights %s: Android Auto %s\n", n ? "on" : "off", n ? "night (dark)" : "day");
            }
        }
        if (c.live && !bye_until) {
            pthread_mutex_lock(&c.mutex);
            const bool native_requested = c.native_focus_pending;
            c.native_focus_pending = false;
            pthread_mutex_unlock(&c.mutex);
            const long long tm = now_ms();
            const bool visible = access("/fs/ram3/nbt-aa-hidden", F_OK) != 0;
            if (native_requested && handoff.request(tm, visible, kExitRequest))
                session.postVideoFocus(true);
            if (handoff.tick(tm, visible, bg_focus, shown_state, kExitRequest))
                session.postVideoFocus(true);
            if (!c.video_token.empty() && visible && !handoff.pending && !native_requested) {
                unsigned generation;
                char state[32];
                if (video_recovery_read(c.video_status, c.video_token.c_str(), &generation, state, sizeof(state)) &&
                    generation > refreshed_generation && !strcmp(state, "opened")) {
                    refreshed_generation = generation;
                    session.postVideoFocus(true);
                    printf("AA_VIDEO_RECOVERY session=%s generation=%u state=focus_refresh uptime_ms=%lld\n",
                           c.video_token.c_str(), generation, tm);
                }
            }
        }
        if (!bye_until && bg_focus && !handoff.pending && now_ms() >= next_focus_check && now_ms() - start >= 2000) {
            next_focus_check = now_ms() + 250;
            const int visible = access("/fs/ram3/nbt-aa-hidden", F_OK) != 0;
            if (visible != shown_state) {
                shown_state = visible;
                session.postVideoFocus(visible != 0);
                printf("AA_VIDEO_FOCUS %s\n", visible ? "PROJECTED (page on screen)" : "NATIVE (iDrive on screen)");
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(c.socket, &fds);
        if (input >= 0 && !bye_until)
            FD_SET(input, &fds);
        struct timeval timeout = {0, 100000};
        int ready = select((input > c.socket ? input : c.socket) + 1, &fds, 0, 0, &timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            printf("AA_CAPTURE_PUMP_ERROR operation=select errno=%d\n", errno);
            stop_reason = "socket_select";
            pump_error = true;
            break;
        }
        if (ready && input >= 0 && !bye_until && FD_ISSET(input, &fds))
            handle_input(input, lines, session);
        if (ready && FD_ISSET(c.socket, &fds)) { // session -> phone
            unsigned char b[NBT_CAPTURE_MAX_TRANSFER];
            ssize_t n = recv(c.socket, b, sizeof(b), 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (n <= 0) {
                if (n < 0) {
                    printf("AA_CAPTURE_PUMP_ERROR operation=recv errno=%d\n", errno);
                    pump_error = true;
                }
                stop_reason = n < 0 ? "socket_recv" : "session_socket_eof";
                break;
            }
            unsigned size = n;
            bridge_begin(&c, c.usb_out_started);
            const bool sent = transfer(context, 0, b, &size);
            bridge_end(&c, c.max_usb_out_ms, c.usb_out_started);
            if (!sent || size != (unsigned)n) {
                printf("AA_CAPTURE_PUMP_ERROR operation=usb_out requested=%u\n", (unsigned)n);
                stop_reason = "usb_out";
                pump_error = true;
                break;
            }
        }
    }

    // ---- shutdown ----
    if (input >= 0)
        close(input);
    pthread_mutex_lock(&c.mutex);
    c.stop_reader = true;
    if (c.reader_error) {
        if (!pump_error)
            stop_reason = c.reader_error;
        pump_error = true;
    }
    const bool clean_shutdown = !pump_error && !c.error;
    pthread_mutex_unlock(&c.mutex);
    printf("AA_VIDEO_CAPTURE_STOP reason=%s local_shutdown=%s uptime_ms=%lld\n",
           stop_reason, bye_reason ? bye_reason : "none", now_ms());
    shutdown(c.socket, SHUT_RDWR);
    if (cancel)
        cancel(context, clean_shutdown);
    nbt::mediaout::stop();   // before the session: its writer reports consumed audio to the session
    session.stop();
    aa_audio::shutdown(); // after the session thread ended: no more frames come in
    if (c.live) {
        unlink("/fs/ram3/nbt-aa-native/nav.state");
        unlink(kExitRequest);
    }
    // cancel() aborts the reader URB before joining; transfer callbacks must not own Capture memory.
    pthread_join(thread, 0);
    if (have_writer) {
        pthread_mutex_lock(&c.queue.m);
        c.queue.stop = true;
        pthread_cond_signal(&c.queue.cv);
        printf("AA_LIVE_QUEUE dropped=%u drop_runs=%u left=%u bytes\n", c.queue.dropped, c.queue.drops,
               (unsigned)c.queue.bytes);
        pthread_mutex_unlock(&c.queue.m);
        pthread_join(writer, 0);
    }
    close(c.socket);
    close(c.data);
    close(c.index);
    const int result = !pump_error && !c.error && c.frames && c.configs ? 0 : 3;
    printf("AA_CAPTURE_BRIDGE_SUMMARY usb_in_max_ms=%lld usb_to_session_max_ms=%lld usb_out_max_ms=%lld\n",
           c.max_usb_in_ms, c.max_usb_to_socket_ms, c.max_usb_out_ms);
    printf("AA_VIDEO_CAPTURE_END frames=%u configs=%u bytes=%llu invalid=%d no_display=1 result=%d\n", c.frames,
           c.configs, (unsigned long long)c.bytes, c.error, result);
    return result;
}
