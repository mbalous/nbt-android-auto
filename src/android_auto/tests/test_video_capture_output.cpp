// Exercise the real media callbacks and writer without the QNX session or USB.
#include "../aa_video_capture.cpp"

namespace aa_audio {
bool setup(nbt::AASession&) { abort(); }
void shutdown() { abort(); }
void setVolume(int) { abort(); }
}
namespace nbt { namespace mediaout {
void setConsumedCallback(ConsumedFn, void*) { abort(); }
bool start(const char*) { abort(); }
void stop() { abort(); }
void onAudio(uint8_t, const uint8_t*, size_t) { abort(); }
void onState(uint8_t, bool, uint32_t, uint32_t) { abort(); }
void setVolume(int) { abort(); }
} }

namespace {

const uint8_t kConfig[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0x1f, 0, 0, 0, 1, 0x68, 0xab};
const uint8_t kFrame[] = {0, 0, 0, 1, 0x41, 0xaa};

void feed_video(const uint8_t* p, size_t n, uint64_t pts, Capture* c)
{
    std::vector<uint8_t> buf(p, p + n);
    video(0, buf, 0, n, pts, c);
}

void require(bool ok, const char* message)
{
    if (!ok) {
        fprintf(stderr, "Capture output: %s\n", message);
        exit(1);
    }
}

void expect_bytes(int fd, const void* data, size_t size)
{
    const uint8_t* expected = static_cast<const uint8_t*>(data);
    uint8_t buf[4096];
    while (size) {
        ssize_t got = read(fd, buf, size < sizeof(buf) ? size : sizeof(buf));
        require(got > 0, "missing output bytes");
        require(memcmp(buf, expected, got) == 0, "unexpected output bytes");
        expected += got;
        size -= got;
    }
}

void expect_end(int fd)
{
    uint8_t byte;
    require(read(fd, &byte, 1) == 0, "unexpected trailing output");
    close(fd);
}

void test_diagnostic_capture()
{
    int data[2], index[2];
    require(pipe(data) == 0 && pipe(index) == 0, "open capture pipes");
    Capture c;
    c.data = data[1];
    c.index = index[1];
    config(kConfig, sizeof(kConfig), &c);
    feed_video(kFrame, sizeof(kFrame), 42, &c);
    require(!c.error, "capture rejected valid media");
    const uint8_t invalid[] = {0xff, 0, 0, 1, 0x41, 0xaa};
    feed_video(invalid, sizeof(invalid), 43, &c);
    require(c.error, "capture accepted non-Annex-B media");

    close(data[1]);
    close(index[1]);
    expect_bytes(data[0], kConfig, sizeof(kConfig));
    expect_bytes(data[0], kFrame, sizeof(kFrame));
    const char csv[] = "config,0,0,0,14\nframe,0,42,14,6\n";
    expect_bytes(index[0], csv, sizeof(csv) - 1);
    expect_end(data[0]);
    expect_end(index[0]);
}

void expect_live_output(Capture& c, const std::vector<uint8_t>& expected)
{
    int data[2];
    require(pipe(data) == 0 && fcntl(data[1], F_SETFL, O_NONBLOCK) == 0, "open nonblocking writer pipe");
    c.data = data[1];
    pthread_t writer;
    require(pthread_create(&writer, 0, live_writer, &c) == 0, "start live writer");
    expect_bytes(data[0], expected.data(), expected.size());

    pthread_mutex_lock(&c.queue.m);
    c.queue.stop = true;
    pthread_cond_signal(&c.queue.cv);
    pthread_mutex_unlock(&c.queue.m);
    require(pthread_join(writer, 0) == 0, "stop live writer");
    require(!c.error && c.queue.items.empty(), "live writer did not drain expected media");
    close(data[1]);
    expect_end(data[0]);
}

void test_padded_queue_budget()
{
    Capture c;
    c.live = true;
    const size_t large = 2 * 1024 * 1024;
    const size_t tail = kLiveBudget - large - 12;
    std::vector<uint8_t> frame(large, 0xaa);
    memcpy(frame.data(), kFrame, sizeof(kFrame));
    feed_video(frame.data(), large, 1, &c);
    feed_video(frame.data(), tail, 2, &c);

    // Twelve bytes remain: a six-byte frame fits only without its 24-byte padded size.
    feed_video(kFrame, sizeof(kFrame), 3, &c);
    require(c.queue.bytes == large + tail && c.queue.dropping && c.queue.dropped == 1,
            "drop dependent frame that only fits before padding");

    const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xaa};
    feed_video(idr, sizeof(idr), 4, &c);
    require(!c.queue.dropping && c.queue.dropped == 2 && c.queue.items.size() == 2 &&
            c.queue.bytes == tail + 24 && c.queue.items.back().data[4] == 0x65,
            "evict enough backlog to queue the padded keyframe within budget");
}

void test_live_stream()
{
    Capture c;
    c.live = true;
    config(kConfig, sizeof(kConfig), &c);
    feed_video(kFrame, sizeof(kFrame), 42, &c);
    require(c.frames == 1 && c.received == 1 && c.configs == 1 && c.bytes == sizeof(kFrame) + sizeof(kConfig),
            "live summary counts source frames, configs and unpadded bytes");

    const uint8_t padded_frame[] = {
        0, 0, 0, 1, 0x41, 0xaa, 0, 0, 0, 1, 0x0c,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80
    };
    std::vector<uint8_t> expected(kConfig, kConfig + sizeof(kConfig));
    expected.insert(expected.end(), padded_frame, padded_frame + sizeof(padded_frame));
    expect_live_output(c, expected);
}

void test_live_recovery()
{
    Capture c;
    c.live = true;
    // Hold off the writer so the second large dependent frame overflows the queue.
    std::vector<uint8_t> frame(2 * 1024 * 1024, 0xaa);
    memcpy(frame.data(), kFrame, sizeof(kFrame));
    feed_video(frame.data(), frame.size(), 1, &c);
    feed_video(frame.data(), frame.size(), 2, &c);
    feed_video(kFrame, sizeof(kFrame), 3, &c);
    frame[4] = 0x65;
    feed_video(frame.data(), frame.size(), 4, &c);
    require(c.frames == 4 && c.bytes == 3 * frame.size() + sizeof(kFrame),
            "live source count includes frames dropped by the queue");
    expect_live_output(c, frame);
}

void test_video_ack_queue()
{
    nbt::AASession session;
    Capture c;
    c.live = true;
    c.session = &session;
    config(kConfig, sizeof(kConfig), &c);
    session.token = 1;
    feed_video(kFrame, sizeof(kFrame), 1, &c);
    require(session.consumed.empty(), "queued frame is not acknowledged before handoff");
    session.token = 2;
    c.queue.paused = true;
    feed_video(kFrame, sizeof(kFrame), 2, &c);
    c.queue.paused = false;
    require(session.consumed.size() == 1 && session.consumed[0] == 2, "dropped frame reports its own token");

    std::vector<uint8_t> expected(kConfig, kConfig + sizeof(kConfig));
    const uint8_t padded_frame[] = {
        0, 0, 0, 1, 0x41, 0xaa, 0, 0, 0, 1, 0x0c,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80
    };
    expected.insert(expected.end(), padded_frame, padded_frame + sizeof(padded_frame));
    expect_live_output(c, expected);
    require(session.consumed.size() == 2 && session.consumed[1] == 1, "writer reports handed-off frame, not config");

    Capture recovery;
    recovery.live = true;
    recovery.session = &session;
    session.token = 3;
    feed_video(kFrame, sizeof(kFrame), 3, &recovery);
    session.token = 4;
    feed_video(kFrame, sizeof(kFrame), 4, &recovery);
    pause_live_writer(&recovery);
    require(session.consumed.size() == 4 && session.consumed[2] == 3 && session.consumed[3] == 4,
            "recovery pause reports all discarded frames");
}

void wait_video_status(const char* path, const char* token, unsigned expected, const char* wanted)
{
    for (unsigned i = 0; i < 500; ++i) {
        unsigned generation;
        char state[32];
        if (video_recovery_read(path, token, &generation, state, sizeof(state)) &&
            generation == expected && !strcmp(state, wanted))
            return;
        usleep(10000);
    }
    require(false, "video recovery status timed out");
}

void test_fifo_reconnect()
{
    char directory[] = "/tmp/nbt-video-recovery-XXXXXX";
    require(mkdtemp(directory) != 0, "create video recovery directory");
    const std::string base(directory);
    const std::string fifo = base + "/video.fifo";
    const std::string control = base + "/control";
    const std::string status = base + "/status";
    require(mkfifo(fifo.c_str(), 0600) == 0, "create video FIFO");
    int reader_fd = open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
    require(reader_fd >= 0, "open first FIFO reader");
    Capture c;
    c.live = true;
    c.live_fifo = fifo.c_str();
    c.video_control = control.c_str();
    c.video_status = status.c_str();
    c.video_token = "test-session";
    c.data = open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
    require(c.data >= 0, "open FIFO writer");
    pthread_t writer;
    require(pthread_create(&writer, 0, live_writer, &c) == 0, "start recovery writer");
    config(kConfig, sizeof(kConfig), &c);
    close(reader_fd);
    feed_video(kFrame, sizeof(kFrame), 1, &c);
    wait_video_status(status.c_str(), c.video_token.c_str(), 0, "fault");
    require(!c.error, "FIFO reader loss must not end the session");
    require(video_recovery_publish(control.c_str(), "old-session", 1, "pause"), "publish stale request");
    usleep(150000);
    unsigned generation;
    char state[32];
    require(video_recovery_read(status.c_str(), c.video_token.c_str(), &generation, state, sizeof(state)) &&
            generation == 0 && !strcmp(state, "fault"), "ignore stale session");
    require(video_recovery_publish(control.c_str(), c.video_token.c_str(), 1, "pause"), "request pause");
    wait_video_status(status.c_str(), c.video_token.c_str(), 1, "paused");
    reader_fd = open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
    require(reader_fd >= 0, "open replacement FIFO reader");
    require(video_recovery_publish(control.c_str(), c.video_token.c_str(), 1, "ready"), "mark decoder ready");
    wait_video_status(status.c_str(), c.video_token.c_str(), 1, "opened");
    feed_video(kFrame, sizeof(kFrame), 2, &c);
    usleep(100000);
    uint8_t byte;
    require(read(reader_fd, &byte, 1) < 0 && errno == EAGAIN, "dependent frame must not restart decoder");
    const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xaa};
    feed_video(idr, sizeof(idr), 3, &c);
    wait_video_status(status.c_str(), c.video_token.c_str(), 1, "running");
    expect_bytes(reader_fd, kConfig, sizeof(kConfig));
    const uint8_t padded_idr[] = {
        0, 0, 0, 1, 0x65, 0xaa, 0, 0, 0, 1, 0x0c,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80
    };
    expect_bytes(reader_fd, padded_idr, sizeof(padded_idr));
    pthread_mutex_lock(&c.queue.m);
    c.queue.stop = true;
    pthread_cond_signal(&c.queue.cv);
    pthread_mutex_unlock(&c.queue.m);
    require(pthread_join(writer, 0) == 0, "join recovery writer");
    close(c.data);
    close(reader_fd);
    unlink(fifo.c_str());
    unlink(control.c_str());
    unlink(status.c_str());
    require(rmdir(directory) == 0, "remove recovery directory");
}

void test_socket_backpressure()
{
    int pair[2], size = 4096;
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "open handoff pair");
    require(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0, "limit handoff buffer");
    require(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0, "set handoff nonblocking");
    Capture c;
    c.socket = pair[0];
    std::vector<uint8_t> data(65536, 0xa5);
    long long start = now_ms();
    require(!write_session_socket(&c, &data[0], data.size()), "stalled handoff must fail");
    require(now_ms() - start >= 1800 && now_ms() - start < 4000, "handoff is bounded");
    close(pair[0]);
    close(pair[1]);
}

struct FakeBulkIn {
    Capture* capture;
    unsigned calls;
    bool stop_during_transfer;
    bool oversize;
};

int fake_bulk_in(void* context, int input, unsigned char* bytes, unsigned* size)
{
    FakeBulkIn* usb = static_cast<FakeBulkIn*>(context);
    require(input == 1, "reader requested bulk IN");
    require(*size == 16384, "reader requested the selected bulk IN size");
    if (usb->oversize) {
        ++usb->calls;
        ++*size;
        return 1;
    }
    if (usb->stop_during_transfer) {
        pthread_mutex_lock(&usb->capture->mutex);
        usb->capture->stop_reader = true;
        pthread_mutex_unlock(&usb->capture->mutex);
        ++usb->calls;
        return 0;
    }
    switch (++usb->calls) {
    case 1:
    case 2:
        *size = 0;
        return 1;
    case 3:
        bytes[0] = 0xa5;
        *size = 1;
        return 1;
    default:
        bytes[0] = 0xb6;
        *size = 1; // A failure is fatal even if it reports partial data.
        return 0;
    }
}

void test_bulk_reader(bool stop_during_transfer, bool oversize = false)
{
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "open capture socket pair");
    Capture c;
    c.read_size = 16384;
    FakeBulkIn usb = {&c, 0, stop_during_transfer, oversize};
    c.transport = &usb;
    c.transfer = fake_bulk_in;
    c.socket = pair[0];
    reader(&c);
    if (stop_during_transfer) {
        require(usb.calls == 1 && !c.reader_error, "shutdown starts no new reads and does not report a pump failure");
    } else if (oversize) {
        require(usb.calls == 1 && c.reader_error && strcmp(c.reader_error, "usb_in_invalid_size") == 0,
                "completion exceeding the submitted read size is rejected");
    } else {
        require(usb.calls == 4 && c.reader_error && strcmp(c.reader_error, "usb_in") == 0,
                "read through empty packets, then stop without replaying a failed transfer");
        const uint8_t expected[] = {0xa5};
        expect_bytes(pair[1], expected, sizeof(expected));
    }
    expect_end(pair[1]);
    close(pair[0]);
}

void fake_snapshot(void* context)
{
    const char* path = static_cast<const char*>(context);
    require(access(path, F_OK) != 0, "transport snapshot precedes stall marker");
    puts("TEST_TRANSPORT_SNAPSHOT");
}

struct SnapshotWorker {
    Capture* capture;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool started, release;
};

void* snapshot_worker(void* context)
{
    SnapshotWorker* worker = static_cast<SnapshotWorker*>(context);
    bridge_begin(worker->capture, worker->capture->usb_in_started);
    pthread_mutex_lock(&worker->mutex);
    worker->started = true;
    pthread_cond_signal(&worker->condition);
    while (!worker->release)
        pthread_cond_wait(&worker->condition, &worker->mutex);
    pthread_mutex_unlock(&worker->mutex);
    bridge_end(worker->capture, worker->capture->max_usb_in_ms, worker->capture->usb_in_started);
    return 0;
}

void test_stall_snapshot()
{
    char path[] = "/tmp/nbt-stall-test-XXXXXX";
    int marker = mkstemp(path);
    require(marker >= 0, "create stall test path");
    close(marker);
    unlink(path);
    int output[2];
    require(pipe(output) == 0, "open snapshot output");
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    require(saved >= 0 && dup2(output[1], STDOUT_FILENO) >= 0, "redirect snapshot output");
    close(output[1]);
    Capture c;
    SnapshotWorker worker = {&c, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false};
    pthread_t thread;
    require(pthread_create(&thread, 0, snapshot_worker, &worker) == 0, "start snapshot worker");
    pthread_mutex_lock(&worker.mutex);
    while (!worker.started)
        pthread_cond_wait(&worker.condition, &worker.mutex);
    pthread_mutex_unlock(&worker.mutex);
    report_stall(&c, fake_snapshot, path, path);
    pthread_mutex_lock(&worker.mutex);
    worker.release = true;
    pthread_cond_signal(&worker.condition);
    pthread_mutex_unlock(&worker.mutex);
    require(pthread_join(thread, 0) == 0, "join snapshot worker");
    pthread_cond_destroy(&worker.condition);
    pthread_mutex_destroy(&worker.mutex);
    require(access(path, F_OK) == 0, "stall marker created after snapshot");
    unlink(path);
    require(dup2(saved, STDOUT_FILENO) >= 0, "restore snapshot output");
    close(saved);
    char text[1024];
    ssize_t n = read(output[0], text, sizeof(text) - 1);
    require(n > 0, "snapshot output exists");
    text[n] = 0;
    close(output[0]);
    require(strstr(text, "AA_CAPTURE_BRIDGE_SNAPSHOT") && strstr(text, "TEST_TRANSPORT_SNAPSHOT"),
            "both bridge and transport snapshots are emitted");
    require(!strstr(text, "usb_in_inflight_ms=-1"), "snapshot includes the active read duration");
}

void test_focus_handoff()
{
    char path[] = "/tmp/nbt-focus-test-XXXXXX";
    int fd = mkstemp(path);
    require(fd >= 0, "create focus test path");
    close(fd);
    unlink(path);

    Capture c;
    focus_request(true, 1, &c);
    require(!c.native_focus_pending, "projected focus must not request iDrive");
    focus_request(false, 1, &c);
    require(c.native_focus_pending, "native focus callback must queue a handoff");

    FocusHandoff h;
    int shown = 1;
    require(!h.request(1000, true, path) && access(path, F_OK) == 0, "request iDrive");
    require(!h.request(1100, true, path), "repeated request while pending");
    require(!h.tick(1500, true, false, shown, path), "do not refocus before iDrive is visible");
    require(!h.tick(1600, false, false, shown, path), "wait for overlay to hide");
    require(!h.tick(2099, false, false, shown, path), "honor overlay hide delay");
    require(h.tick(2100, false, false, shown, path) && !h.pending && shown == 0 &&
            access(path, F_OK) != 0, "resume background video only after hiding");

    require(!h.request(3000, true, path), "request second handoff");
    require(!h.tick(6999, true, false, shown, path), "wait for HMI within deadline");
    require(h.tick(7000, true, false, shown, path) && !h.pending && access(path, F_OK) != 0,
            "restore projection after failed handoff");
    require(!h.request(7100, true, path), "request slow handoff");
    require(!h.tick(11000, false, false, shown, path), "late HMI transition still waits for overlay");
    require(h.tick(11500, false, false, shown, path), "resume only after delayed overlay hide");
    shown = 1;
    require(!h.request(8000, true, path), "request with background focus");
    require(!h.tick(8100, false, true, shown, path), "wait while overlay hides with background focus");
    require(!h.tick(8600, false, true, shown, path) && !h.pending && shown == 0,
            "preserve native background focus");
    require(!h.request(9000, true, path), "request before quick reopen");
    require(!h.tick(9100, false, false, shown, path), "observe hidden page");
    require(h.tick(9200, true, true, shown, path) && shown == 1, "restore focus on quick reopen");
    require(h.request(10000, true, "/nonexistent/nbt-focus-test/request"), "restore focus on signal failure");
    require(!h.pending, "signal failure must not leave a pending handoff");
    require(h.request(-1, true, path), "restore focus if the monotonic clock fails");
}

} // namespace

int main()
{
    alarm(25);
    signal(SIGPIPE, SIG_IGN);
    require(setenv("NBT_AA_PAD", "24", 1) == 0, "set test padding");
    test_diagnostic_capture();
    test_live_stream();
    test_live_recovery();
    test_video_ack_queue();
    test_fifo_reconnect();
    test_socket_backpressure();
    test_padded_queue_budget();
    test_bulk_reader(false);
    test_bulk_reader(true);
    test_bulk_reader(false, true);
    test_stall_snapshot();
    test_focus_handoff();
    puts("Capture output OK: diagnostic index, live bytes, backpressure, USB reader, and focus handoff");
    return 0;
}
