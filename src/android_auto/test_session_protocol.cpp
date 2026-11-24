// Offline phone simulator: real AASession, AA frames and TLS, lab identities only.
// No USB/display access and no listening network socket.
#define NBT_TLS_PEER_ONLY
#include "test_tls_memory.cpp"
#include "aa_session.hpp"
#include <sys/socket.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <vector>

typedef std::vector<uint8_t> Bytes;
struct Frame { unsigned channel, flags; Bytes data; };

static bool read_exact(int fd, uint8_t* p, size_t size) {
    while (size) {
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        struct timeval timeout = { 2, 0 };
        int ready = select(fd + 1, &fds, NULL, NULL, &timeout);
        if (ready < 0 && errno == EINTR) continue;
        require(ready > 0, "bounded peer read (not receiver idle timeout)");
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == ECONNRESET) return false;
        if (n == 0) return false;
        require(n > 0, "peer socket read");
        p += n; size -= n;
    }
    return true;
}
static Frame receive_frame(int fd) {
    uint8_t h[4];
    require(read_exact(fd, h, sizeof(h)), "frame header present");
    Frame f; f.channel = h[0]; f.flags = h[1];
    require((f.flags & 3) == 3, "receiver sends bulk frame");
    f.data.resize((h[2] << 8) | h[3]);
    require(!f.data.empty() && read_exact(fd, &f.data[0], f.data.size()), "frame body present");
    return f;
}
static unsigned message(const Bytes& bytes) {
    require(bytes.size() >= 2, "message id present");
    return (bytes[0] << 8) | bytes[1];
}
static void send_bytes(int fd, const Bytes& bytes) {
    for (size_t offset = 0; offset < bytes.size();) {
        size_t part = bytes.size() - offset;
        if (part > 3) part = 3; // Force partial stream writes, including frame headers.
        ssize_t n = write(fd, &bytes[offset], part);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "peer fragmented socket write");
        offset += n;
    }
}
static void send_frame(int fd, unsigned channel, unsigned flags, const Bytes& body) {
    require(body.size() <= 65535, "test frame bound");
    Bytes wire;
    wire.push_back(channel); wire.push_back(flags);
    wire.push_back(body.size() >> 8); wire.push_back(body.size());
    wire.insert(wire.end(), body.begin(), body.end());
    send_bytes(fd, wire);
}
static Bytes msg(unsigned id) {
    Bytes b; b.push_back(id >> 8); b.push_back(id); return b;
}
static Bytes version(unsigned major = 1, unsigned status = 0) {
    Bytes b = msg(2);
    b.push_back(major >> 8); b.push_back(major);
    b.push_back(0); b.push_back(7);
    b.push_back(status >> 8); b.push_back(status);
    return b;
}
static int begin(nbt::AASession& session) {
    int sv[2];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    require(session.start(sv[0]), "session start");
    Frame f = receive_frame(sv[1]);
    require(f.channel == 0 && f.flags == 3 && message(f.data) == 1, "version request");
    return sv[1];
}
static void expect_closed(nbt::AASession& session, int fd) {
    uint8_t byte;
    require(!read_exact(fd, &byte, 1), "session closes immediately without extra frames");
    session.stop(); close(fd);
}
static void feed_server(peer* server, const Bytes& bytes, size_t skip = 0) {
    require(skip <= bytes.size() && bytes.size() - skip <= sizeof(server->input) - server->used,
            "server input bound");
    memcpy(server->input + server->used, &bytes[skip], bytes.size() - skip);
    server->used += bytes.size() - skip;
}
static void negotiate(int fd, peer* server, peer* carrier) {
    send_frame(fd, 0, 3, version());
    bool complete = false;
    for (unsigned step = 0; step < 50; ++step) {
        Frame f = receive_frame(fd);
        require(f.channel == 0 && f.flags == 3, "plain control handshake");
        if (message(f.data) == 4) {
            require(complete && f.data.size() == 4 && f.data[2] == 8 && f.data[3] == 0,
                    "one successful AuthComplete after TLS");
            require(mbedtls_ssl_get_verify_result(&server->ssl) == 0, "receiver certificate verified");
            return;
        }
        require(message(f.data) == 3, "TLS handshake envelope");
        feed_server(server, f.data, 2);
        int rc = mbedtls_ssl_handshake(&server->ssl);
        require(rc == 0 || rc == MBEDTLS_ERR_SSL_WANT_READ, "simulated phone handshake");
        complete = rc == 0;
        for (size_t offset = 0; offset < carrier->used;) {
            size_t n = carrier->used - offset;
            if (n > 251) n = 251; // TLS record split across AA handshake messages.
            Bytes b = msg(3);
            b.insert(b.end(), carrier->input + offset, carrier->input + offset + n);
            send_frame(fd, 0, 3, b); offset += n;
        }
        carrier->used = 0;
    }
    require(0, "bounded handshake steps");
}
static void send_encrypted(int fd, peer* server, peer* carrier, unsigned channel,
                           const Bytes& body, bool tamper = false) {
    require(carrier->used == 0, "outgoing TLS queue drained");
    require(mbedtls_ssl_write(&server->ssl, &body[0], body.size()) == (int)body.size(), "phone TLS write");
    Bytes encrypted(carrier->input, carrier->input + carrier->used);
    carrier->used = 0;
    if (tamper) encrypted.back() ^= 1;
    send_frame(fd, channel, 0x0b, encrypted);
}
static Bytes receive_encrypted(int fd, peer* server, unsigned channel) {
    Frame f = receive_frame(fd);
    require(f.channel == channel && f.flags == 0x0b, "encrypted response channel");
    feed_server(server, f.data);
    uint8_t plain[16384];
    int n = mbedtls_ssl_read(&server->ssl, plain, sizeof(plain));
    require(n > 0, "phone TLS read");
    return Bytes(plain, plain + n);
}
static void on_audio(uint8_t channel, const uint8_t* pcm, size_t size, uint64_t pts, void* ctx) {
    require(channel == 9 && size == 4 && pts == 42 && pcm[0] == 0x12 && pcm[3] == 0x78,
            "PCM callback preserves channel/payload/timestamp");
    ++*(unsigned*)ctx;
}
struct PhoneCalls {
    pthread_mutex_t mutex;
    unsigned count;
    unsigned active;
    bool in_call;
    PhoneCalls() : count(0), active(0), in_call(false) { pthread_mutex_init(&mutex, NULL); }
    ~PhoneCalls() { pthread_mutex_destroy(&mutex); }
};
static void on_phone(bool in_call, void* ctx) {
    PhoneCalls* calls = static_cast<PhoneCalls*>(ctx);
    pthread_mutex_lock(&calls->mutex);
    ++calls->count;
    if (in_call) ++calls->active;
    calls->in_call = in_call;
    pthread_mutex_unlock(&calls->mutex);
}
static void configure_identity() {
    setenv("NBT_AA_TLS_CERT", "lab-client.pem", 1);
    setenv("NBT_AA_TLS_KEY", "lab-client-key.pem", 1);
    setenv("NBT_AA_TLS_CA", "lab-ca.pem", 1);
    setenv("NBT_AA_TLS_PEER", "qnx-aa-lab.local", 1);
}

struct VideoFrames {
    pthread_mutex_t mutex;
    nbt::AASession* session;
    std::vector<uint64_t> tokens;
    explicit VideoFrames(nbt::AASession* s) : session(s) { pthread_mutex_init(&mutex, NULL); }
    ~VideoFrames() { pthread_mutex_destroy(&mutex); }
};
static void on_video(uint32_t, Bytes& data, size_t offset, size_t length, uint64_t, void* ctx) {
    VideoFrames* frames = static_cast<VideoFrames*>(ctx);
    require(offset <= data.size() && length <= data.size() - offset, "valid video slice");
    const uint64_t token = frames->session->currentVideoToken();
    pthread_mutex_lock(&frames->mutex);
    frames->tokens.push_back(token);
    pthread_mutex_unlock(&frames->mutex);
}
static uint64_t wait_video(VideoFrames& frames, unsigned count) {
    for (unsigned i = 0; i < 200; ++i) {
        pthread_mutex_lock(&frames.mutex);
        const uint64_t token = frames.tokens.size() >= count ? frames.tokens[count - 1] : 0;
        pthread_mutex_unlock(&frames.mutex);
        if (token) return token;
        usleep(10000);
    }
    require(false, "video callback deadline");
    return 0;
}
static void no_video_ack(int fd, unsigned ms) {
    fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
    struct timeval timeout = { (long)(ms / 1000), (long)((ms % 1000) * 1000) };
    require(select(fd + 1, &fds, NULL, NULL, &timeout) == 0, "video ack remains deferred");
}
static void test_video_flow() {
    setenv("NBT_AA_VIDEO_FPS", "2", 1);
    setenv("NBT_AA_VIDEO_WINDOW", "3", 1);
    nbt::AASession session;
    VideoFrames frames(&session);
    unsigned audio_calls = 0;
    session.setVideoFrameCallback(on_video, &frames);
    session.setAudioFrameCallback(on_audio, &audio_calls);
    session.setVideoAckGate(true);
    peer* server = (peer*)calloc(1, sizeof(peer));
    peer* carrier = (peer*)calloc(1, sizeof(peer));
    require(server && carrier, "allocate video phone");
    server->other = carrier; initialize(server, 1, NULL);
    int fd = begin(session); negotiate(fd, server, carrier);
    send_encrypted(fd, server, carrier, 6, msg(0x8000));
    Bytes setup = receive_encrypted(fd, server, 6);
    require(message(setup) == 0x8003 && setup.size() >= 6 && setup[4] == 0x10 && setup[5] == 3,
            "video setup advertises configured window");
    require(message(receive_encrypted(fd, server, 6)) == 0x8008, "video focus response");
    Bytes start = msg(0x8001); start.push_back(8); start.push_back(7); start.push_back(16); start.push_back(0);
    send_encrypted(fd, server, carrier, 6, start);
    Bytes frame = msg(0); frame.resize(10, 0);
    frame.push_back(0); frame.push_back(0); frame.push_back(0); frame.push_back(1); frame.push_back(0x65);
    send_encrypted(fd, server, carrier, 6, frame);
    const uint64_t first = wait_video(frames, 1);
    send_encrypted(fd, server, carrier, 6, frame);
    const uint64_t second = wait_video(frames, 2);
    session.postVideoConsumed(second);
    no_video_ack(fd, 50);
    Bytes pcm = msg(0); pcm.resize(10, 0); pcm[9] = 42;
    pcm.push_back(0x12); pcm.push_back(0x34); pcm.push_back(0x56); pcm.push_back(0x78);
    send_encrypted(fd, server, carrier, 9, pcm);
    require(message(receive_encrypted(fd, server, 9)) == 0x8004,
            "audio remains responsive while video is gated");
    session.postVideoConsumed(first);
    require(message(receive_encrypted(fd, server, 6)) == 0x8004, "handoff releases first ack");
    no_video_ack(fd, 150);
    require(message(receive_encrypted(fd, server, 6)) == 0x8004, "second ack obeys rate limit");
    send_encrypted(fd, server, carrier, 6, frame);
    const uint64_t old = wait_video(frames, 3);
    send_encrypted(fd, server, carrier, 6, start);
    require(message(receive_encrypted(fd, server, 6)) == 0x8004, "new stream flushes old ack");
    send_encrypted(fd, server, carrier, 6, frame);
    const uint64_t fresh = wait_video(frames, 4);
    session.postVideoConsumed(old);
    no_video_ack(fd, 50);
    session.postVideoConsumed(fresh);
    require(message(receive_encrypted(fd, server, 6)) == 0x8004, "old handoff cannot release new stream ack");
    session.stop(); close(fd);
    require(audio_calls == 1, "gated video preserves audio callback");
    release(server); free(carrier);
    setenv("NBT_AA_VIDEO_FPS", "0", 1);
    nbt::AASession ungated;
    VideoFrames ungated_frames(&ungated);
    ungated.setVideoFrameCallback(on_video, &ungated_frames);
    ungated.setVideoAckGate(false);
    server = (peer*)calloc(1, sizeof(peer));
    carrier = (peer*)calloc(1, sizeof(peer));
    require(server && carrier, "allocate ungated phone");
    server->other = carrier; initialize(server, 1, NULL);
    fd = begin(ungated); negotiate(fd, server, carrier);
    send_encrypted(fd, server, carrier, 6, msg(0x8000));
    setup = receive_encrypted(fd, server, 6);
    require(message(setup) == 0x8003 && setup.size() >= 6 && setup[5] == 10, "ungated video window");
    require(message(receive_encrypted(fd, server, 6)) == 0x8008, "ungated video focus response");
    send_encrypted(fd, server, carrier, 6, start);
    send_encrypted(fd, server, carrier, 6, frame);
    require(message(receive_encrypted(fd, server, 6)) == 0x8004, "disabled flow control acks on arrival");
    ungated.stop(); close(fd);
    release(server); free(carrier);
    unsetenv("NBT_AA_VIDEO_FPS");
    unsetenv("NBT_AA_VIDEO_WINDOW");
    puts("AA_VIDEO_FLOW_OK");
}

int main() {
    alarm(90); signal(SIGPIPE, SIG_IGN); setvbuf(stdout, NULL, _IOLBF, 0);
    check((int)psa_crypto_init(), "PSA init");
    configure_identity();
    // Longer than our two-second negative-test deadline: EOF must come from rejection.
    setenv("NBT_RX_TIMEOUT_S", "10", 1);
    nbt::AASession session;
    unsigned audio_calls = 0;
    PhoneCalls phone_calls;
    session.setAudioFrameCallback(on_audio, &audio_calls);
    session.setPhoneCallCallback(on_phone, &phone_calls);
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        int fd = begin(session);
        switch (scenario) {
        case 0: send_frame(fd, 0, 3, version(1, 0xffff)); break;
        case 1: send_frame(fd, 0, 3, version(2)); break;
        case 2: send_frame(fd, 0, 3, msg(2)); break;
        case 3: send_frame(fd, 0, 3, msg(3)); break;
        case 4: send_frame(fd, 0, 3, msg(5)); break;
        case 5: send_frame(fd, 9, 3, msg(0)); break;
        case 6: send_frame(fd, 32, 3, msg(0x8001)); break;
        case 7: send_frame(fd, 0, 0x0b, msg(5)); break;
        }
        expect_closed(session, fd);
    }
    int fd = begin(session);
    unsetenv("NBT_AA_TLS_CERT");
    send_frame(fd, 0, 3, version()); expect_closed(session, fd);
    configure_identity();
    fd = begin(session);
    send_frame(fd, 0, 3, version());
    require(message(receive_frame(fd).data) == 3, "initial ClientHello");
    send_frame(fd, 0, 3, version()); expect_closed(session, fd);
    for (unsigned malformed = 0; malformed < 2; ++malformed) {
        fd = begin(session);
        send_frame(fd, 0, 3, version());
        require(message(receive_frame(fd).data) == 3, "ClientHello before bad handshake");
        Bytes bad = msg(3);
        if (malformed) bad.resize(12, 0xff); // Impossible TLS record type/version.
        send_frame(fd, 0, 3, bad);
        expect_closed(session, fd);
    }
    puts("AA_PROTOCOL_NEGATIVE_OK cases=12");

    // Reuse the SAME session object after both failed and fully authenticated sessions.
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        peer* server = (peer*)calloc(1, sizeof(peer));
        peer* carrier = (peer*)calloc(1, sizeof(peer));
        require(server && carrier, "allocate phone");
        server->other = carrier; initialize(server, 1, NULL);
        fd = begin(session); negotiate(fd, server, carrier);
        send_encrypted(fd, server, carrier, 0, msg(5));
        Bytes discovery = receive_encrypted(fd, server, 0);
        require(message(discovery) == 6 && discovery.size() > 100, "encrypted service discovery body");
        if (scenario == 0) {
            Bytes pcm = msg(0); pcm.resize(10, 0); pcm[9] = 42;
            pcm.push_back(0x12); pcm.push_back(0x34); pcm.push_back(0x56); pcm.push_back(0x78);
            send_encrypted(fd, server, carrier, 9, pcm);
            require(message(receive_encrypted(fd, server, 9)) == 0x8004, "media acknowledgment");
            Bytes phone = msg(0x8001);
            phone.push_back(0x0a); phone.push_back(2); phone.push_back(8); phone.push_back(4);
            send_encrypted(fd, server, carrier, 11, phone);
            phone.back() = 3;
            send_encrypted(fd, server, carrier, 11, phone);
            Bytes ping = msg(11); ping.push_back(8); ping.push_back(123);
            send_frame(fd, 0, 3, ping);
            Frame pong = receive_frame(fd); ping[1] = 12;
            require(pong.channel == 0 && pong.flags == 3 && pong.data == ping, "ping timestamp echo");
            pthread_mutex_lock(&phone_calls.mutex);
            require(phone_calls.count == 2 && phone_calls.active == 1 && !phone_calls.in_call,
                    "phone status dispatched without AV response");
            pthread_mutex_unlock(&phone_calls.mutex);
            send_encrypted(fd, server, carrier, 0, msg(15));
            require(message(receive_encrypted(fd, server, 0)) == 16, "clean ByeBye response");
        } else if (scenario == 1) {
            send_encrypted(fd, server, carrier, 0, msg(5), true);
        } else if (scenario == 2) {
            send_frame(fd, 0, 3, msg(3)); // No second authentication notification.
        } else {
            send_frame(fd, 0, 3, msg(5)); // Applications require authenticated encryption.
        }
        expect_closed(session, fd);
        release(server); free(carrier);
    }
    require(audio_calls == 1, "only authenticated PCM delivered");
    puts("AA_PROTOCOL_SIMULATOR_OK handshakes=4 discovery=4 PCM=1 phone-status ping bye tamper repeated-auth plaintext-rejection reconnect");
    test_video_flow();
    return 0;
}
