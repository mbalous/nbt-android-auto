#include <limits.h>
#include <pthread.h>

static char test_control[512], test_status[512];
#define NBT_VIDEO_CONTROL test_control
#define NBT_VIDEO_STATUS test_status
#define main native_service_main
#include "../nbt_native_service.c"
#undef main

static void require_test(int ok, const char* message)
{
    if (!ok) {
        fprintf(stderr, "Video supervisor: %s\n", message);
        exit(1);
    }
}

struct writer_args { unsigned generation; const char* outcome; pid_t kill_pid; };

static void* fake_writer(void* context)
{
    const struct writer_args* args = (const struct writer_args*)context;
    unsigned generation;
    char state[32];
    int paused = 0;
    unsigned i;
    for (i = 0; i < 1000; ++i) {
        if (video_recovery_read(test_control, "session", &generation, state, sizeof(state)) &&
            generation == args->generation) {
            if (!paused && !strcmp(state, "pause")) {
                if (args->kill_pid) {
                    kill(args->kill_pid, SIGTERM);
                    return NULL;
                }
                require_test(video_recovery_publish(test_status, "session", generation, "paused"), "publish paused");
                paused = 1;
            } else if (paused && !strcmp(state, "ready")) {
                require_test(video_recovery_publish(test_status, "session", generation, args->outcome),
                             "publish recovery outcome");
                return NULL;
            }
        }
        usleep(10000);
    }
    return (void*)1;
}

int main(int argc, char** argv)
{
    char directory[] = "/tmp/nbt-supervisor-XXXXXX";
    char executable[4096];
    const char* names[] = {"fifo_http", "decode_runner", "native_frames", "capture"};
    Child http = {0, -1, 0, 0}, decoder = {0, -1, 0, 0}, frames = {0, -1, 0, 0};
    Child hwv = {0, -1, 0, 0}, capture = {0, -1, 0, 0};
    char *http_args[] = {"./fifo_http", 0}, *decoder_args[] = {"./decode_runner", 0};
    char *frame_args[] = {"./native_frames", 0}, *capture_args[] = {"./capture", 0};
    char* unused_args[] = {0};
    pthread_t writer;
    int guard[2], ok, passed, i, raw;
    pid_t capture_pid, old_http;
    ssize_t length;
    void* thread_result;
    struct writer_args writer_args = {1, "running", 0};
    (void)argc;
    if (strstr(argv[0], "fifo_http")) {
        int ready = open("http.ready", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (ready < 0)
            return 2;
        close(ready);
        for (;;)
            pause();
    }
    if (strstr(argv[0], "decode_runner") || strstr(argv[0], "native_frames") || strstr(argv[0], "/capture"))
        for (;;)
            pause();

    alarm(20);
    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    require_test(length > 0, "resolve test executable");
    executable[length] = 0;
    require_test(mkdtemp(directory) && chdir(directory) == 0, "create private test directory");
    snprintf(test_control, sizeof(test_control), "%s/control", directory);
    snprintf(test_status, sizeof(test_status), "%s/status", directory);
    for (i = 0; i < 4; ++i)
        require_test(symlink(executable, names[i]) == 0, "create fake video child");
    require_test(mkfifo("annexb.fifo", 0600) == 0 && pipe(guard) == 0, "create video and guard pipes");
    raw = open("raw.fifo", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    require_test(raw >= 0, "create fake decoder output");
    close(raw);
    require_test(fcntl(guard[0], F_SETFL, O_NONBLOCK) == 0, "make guard nonblocking");
    http = spawn(http_args, 0, "old-http.log");
    decoder = spawn(decoder_args, 0, "old-decoder.log");
    frames = spawn(frame_args, 0, "old-frames.log");
    capture = spawn(capture_args, 0, "capture.log");
    capture_pid = capture.pid;
    old_http = http.pid;
    require_test(capture_pid > 0 && old_http > 0, "spawn original session");
    g_last_report = LLONG_MAX / 2;
    require_test(pthread_create(&writer, NULL, fake_writer, &writer_args) == 0, "start fake video writer");
    ok = recover_video_chain(&http, &decoder, &frames, &hwv, &capture, 0, 0, 0, 0, 0, 0,
                             unused_args, 1, 1, "session", guard[0]);
    require_test(pthread_join(writer, &thread_result) == 0, "join fake writer");
    passed = ok && !thread_result && alive(&capture) && capture.pid == capture_pid &&
             alive(&http) && http.pid != old_http && alive(&decoder) && alive(&frames);
    writer_args.generation = 2;
    writer_args.outcome = "failed";
    require_test(pthread_create(&writer, NULL, fake_writer, &writer_args) == 0, "start failed recovery writer");
    ok = recover_video_chain(&http, &decoder, &frames, &hwv, &capture, 0, 0, 0, 0, 0, 0,
                             unused_args, 1, 2, "session", guard[0]);
    require_test(pthread_join(writer, &thread_result) == 0, "join failed recovery writer");
    passed = passed && !ok && !thread_result && alive(&capture) && capture.pid == capture_pid;
    writer_args.generation = 3;
    writer_args.kill_pid = capture_pid;
    require_test(pthread_create(&writer, NULL, fake_writer, &writer_args) == 0, "start USB loss simulation");
    ok = recover_video_chain(&http, &decoder, &frames, &hwv, &capture, 0, 0, 0, 0, 0, 0,
                             unused_args, 1, 3, "session", guard[0]);
    require_test(pthread_join(writer, &thread_result) == 0, "join USB loss simulation");
    passed = passed && !ok && !thread_result && !alive(&capture);
    end(&http);
    end(&decoder);
    end(&frames);
    end(&capture);
    close(guard[0]);
    close(guard[1]);
    unlink("annexb.fifo");
    unlink("raw.fifo");
    unlink("http.ready");
    unlink(test_control);
    unlink(test_status);
    unlink("old-http.log");
    unlink("old-decoder.log");
    unlink("old-frames.log");
    unlink("capture.log");
    unlink("fifo-http-1.1.log");
    unlink("decoder-1.1.log");
    unlink("frames-1.1.log");
    unlink("fifo-http-1.2.log");
    unlink("decoder-1.2.log");
    unlink("frames-1.2.log");
    for (i = 0; i < 4; ++i)
        unlink(names[i]);
    require_test(chdir("/") == 0 && rmdir(directory) == 0, "remove private test directory");
    require_test(passed, "restart video children without replacing capture");
    puts("VIDEO_SUPERVISOR_RECOVERY_OK");
    return 0;
}
