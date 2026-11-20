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
            c.queue.bytes == tail + 24 && c.queue.items.back()[4] == 0x65,
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
    alarm(15);
    require(setenv("NBT_AA_PAD", "24", 1) == 0, "set test padding");
    test_diagnostic_capture();
    test_live_stream();
    test_live_recovery();
    test_padded_queue_budget();
    test_bulk_reader(false);
    test_bulk_reader(true);
    test_bulk_reader(false, true);
    test_focus_handoff();
    puts("Capture output OK: diagnostic index, live bytes, backpressure, USB reader, and focus handoff");
    return 0;
}
