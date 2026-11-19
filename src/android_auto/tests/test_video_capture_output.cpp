// Exercise the real media callbacks and writer without the QNX session or USB.
#include "../aa_video_capture.cpp"

namespace aa_audio {
bool setup(nbt::AASession&) { abort(); }
void shutdown() { abort(); }
}

namespace {

const uint8_t kConfig[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0x1f, 0, 0, 0, 1, 0x68, 0xab};
const uint8_t kFrame[] = {0, 0, 0, 1, 0x41, 0xaa};

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
    video(0, kFrame, sizeof(kFrame), 42, &c);
    require(!c.error, "capture rejected valid media");
    const uint8_t invalid[] = {0xff, 0, 0, 1, 0x41, 0xaa};
    video(0, invalid, sizeof(invalid), 43, &c);
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

void test_live_stream()
{
    Capture c;
    c.live = true;
    config(kConfig, sizeof(kConfig), &c);
    video(0, kFrame, sizeof(kFrame), 42, &c);

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
    video(0, frame.data(), frame.size(), 1, &c);
    video(0, frame.data(), frame.size(), 2, &c);
    video(0, kFrame, sizeof(kFrame), 3, &c);
    frame[4] = 0x65;
    video(0, frame.data(), frame.size(), 4, &c);
    expect_live_output(c, frame);
}

} // namespace

int main()
{
    alarm(15);
    require(setenv("NBT_AA_PAD", "24", 1) == 0, "set test padding");
    test_diagnostic_capture();
    test_live_stream();
    test_live_recovery();
    puts("Capture output OK: diagnostic index, live bytes, and recovery after backpressure");
    return 0;
}
