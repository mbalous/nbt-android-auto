#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define NBT_AA_CAPTURE
#define main nbt_android_auto_main
#include "../nbt_android_auto.c"
#undef main

struct usbd_urb { int unused; };
struct usbd_pipe { int unused; };

static pthread_mutex_t fake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_condition = PTHREAD_COND_INITIALIZER;
static int fake_submit_rc, fake_completion_rc, fake_pending, fake_submitted, fake_io_calls, fake_aborts;
static int fake_zlp_failure, fake_abort_no_callback, fake_keep_status_on_abort;
static _Uint32t fake_status, fake_actual;
static void (*fake_callback)(struct usbd_urb*, struct usbd_pipe*, void*);
static struct usbd_urb* fake_urb;
static void* fake_context;

int usbd_setup_bulk(struct usbd_urb* urb, uint32_t flags, void* buffer, uint32_t size)
{
    (void)urb; (void)flags; (void)buffer; (void)size;
    return EOK;
}

int usbd_io(struct usbd_urb* urb, struct usbd_pipe* pipe,
            void (*callback)(struct usbd_urb*, struct usbd_pipe*, void*), void* context, uint32_t timeout)
{
    int rc, pending;
    (void)timeout;
    pthread_mutex_lock(&fake_mutex);
    ++fake_io_calls;
    rc = fake_submit_rc;
    pending = fake_pending;
    if (!rc) {
        fake_urb = urb;
        fake_callback = callback;
        fake_context = context;
        fake_submitted = 1;
        pthread_cond_signal(&fake_condition);
    }
    pthread_mutex_unlock(&fake_mutex);
    if (!rc && !pending)
        callback(urb, pipe, context);
    return rc;
}

int usbd_urb_status(struct usbd_urb* urb, uint32_t* status, uint32_t* actual)
{
    int rc;
    (void)urb;
    pthread_mutex_lock(&fake_mutex);
    *status = fake_status;
    *actual = fake_actual;
    rc = fake_completion_rc;
    if (fake_zlp_failure && fake_io_calls == 2) {
        *status = 0x02000005U;
        *actual = 0;
        rc = EIO;
    }
    pthread_mutex_unlock(&fake_mutex);
    return rc;
}

int usbd_abort_pipe(struct usbd_pipe* pipe)
{
    void (*callback)(struct usbd_urb*, struct usbd_pipe*, void*);
    struct usbd_urb* urb;
    void* context;
    pthread_mutex_lock(&fake_mutex);
    ++fake_aborts;
    if (!fake_keep_status_on_abort)
        fake_completion_rc = EINTR;
    callback = fake_callback;
    urb = fake_urb;
    context = fake_context;
    pthread_mutex_unlock(&fake_mutex);
    if (!fake_abort_no_callback)
        callback(urb, pipe, context);
    return EOK;
}

static void require_test(int ok, const char* message)
{
    if (!ok) {
        fprintf(stderr, "USB capture: %s\n", message);
        exit(1);
    }
}

static void reset_fake(void)
{
    fake_submit_rc = fake_completion_rc = fake_pending = fake_submitted = fake_io_calls = fake_aborts = 0;
    fake_zlp_failure = fake_abort_no_callback = fake_keep_status_on_abort = 0;
    fake_status = USBD_STATUS_CMP;
    fake_actual = 0;
}

struct transfer_args { struct capture_usb* capture; unsigned char data[8192]; unsigned size; int result; };
static void* read_transfer(void* context)
{
    struct transfer_args* args = (struct transfer_args*)context;
    args->result = capture_transfer(args->capture, 1, args->data, &args->size);
    return NULL;
}

int main(void)
{
    struct capture_usb c;
    struct usbd_urb urb;
    struct usbd_pipe pipe;
    unsigned char dma[8192], out_dma[8192], out[512];
    struct transfer_args args;
    pthread_t thread;
    unsigned size;
    pid_t child;
    int status;
    alarm(15);
    memset(&c, 0, sizeof(c));
    c.urb[1] = &urb;
    c.pipe[1] = &pipe;
    c.dma[1] = dma;
    c.capacity[1] = sizeof(dma);
    c.urb[0] = &urb;
    c.pipe[0] = &pipe;
    c.dma[0] = out_dma;
    c.capacity[0] = sizeof(out_dma);
    c.out_max_packet = 512;
    memset(out, 0, sizeof(out));
    require_test(!usb_transfer_lifecycle_init(&c.lifecycle), "initialize lifecycle");
    require_test(!pthread_mutex_init(&c.diagnostics_mutex, NULL), "initialize diagnostics");
    reset_fake();
    fake_status = 0x02000005U;
    fake_actual = 5632;
    memset(&args, 0, sizeof(args));
    args.capture = &c;
    args.size = sizeof(args.data);
    read_transfer(&args);
    require_test(!args.result && fake_io_calls == 1 && c.diagnostics[1].failures == 1 &&
                 c.diagnostics[1].first_actual == 5632,
                 "partial no-answer fails without replay");

    reset_fake();
    fake_submit_rc = ENOSPC;
    args.size = sizeof(args.data);
    read_transfer(&args);
    require_test(!args.result && fake_io_calls == 1, "undocumented submission error is not retried");

    reset_fake();
    fake_actual = 1;
    size = sizeof(out);
    require_test(!capture_transfer(&c, 0, out, &size) && fake_io_calls == 1,
                 "short OUT is not replayed");
    reset_fake();
    fake_actual = sizeof(out);
    fake_zlp_failure = 1;
    size = sizeof(out);
    require_test(!capture_transfer(&c, 0, out, &size) && fake_io_calls == 2 &&
                 c.diagnostics[0].zlp_failures == 1, "failed OUT terminator is not replayed");

    memset(&c.diagnostics[1], 0, sizeof(c.diagnostics[1]));
    reset_fake();
    fake_pending = 1;
    fake_keep_status_on_abort = 1;
    fake_status = 0x02000005U;
    fake_actual = 1024;
    args.size = sizeof(args.data);
    require_test(pthread_create(&thread, NULL, read_transfer, &args) == 0, "start read racing cancellation");
    pthread_mutex_lock(&fake_mutex);
    while (!fake_submitted)
        pthread_cond_wait(&fake_condition, &fake_mutex);
    pthread_mutex_unlock(&fake_mutex);
    capture_cancel(&c, 1);
    require_test(pthread_join(thread, NULL) == 0 && !args.result && c.diagnostics[1].failures == 0 &&
                 c.diagnostics[1].interruptions == 1 && c.diagnostics[1].bytes_actual == 1024,
                 "failure racing clean shutdown is outcome-unknown and retains byte count");
    usb_transfer_lifecycle_destroy(&c.lifecycle);
    require_test(!usb_transfer_lifecycle_init(&c.lifecycle), "reinitialize lifecycle");

    memset(&c.diagnostics[1], 0, sizeof(c.diagnostics[1]));
    reset_fake();
    fake_pending = 1;
    args.size = sizeof(args.data);
    require_test(pthread_create(&thread, NULL, read_transfer, &args) == 0, "start pending read");
    pthread_mutex_lock(&fake_mutex);
    while (!fake_submitted)
        pthread_cond_wait(&fake_condition, &fake_mutex);
    pthread_mutex_unlock(&fake_mutex);
    capture_cancel(&c, 1);
    require_test(pthread_join(thread, NULL) == 0, "join cancelled read");
    require_test(!args.result && fake_aborts == 1 && c.diagnostics[1].failures == 0 &&
                 c.diagnostics[1].interruptions == 1,
                 "shutdown abort is not counted as a transport failure or repeated");
    usb_transfer_lifecycle_destroy(&c.lifecycle);

    memset(&c.diagnostics[1], 0, sizeof(c.diagnostics[1]));
    require_test(!usb_transfer_lifecycle_init(&c.lifecycle), "reinitialize lifecycle for removal");
    reset_fake();
    fake_pending = 1;
    args.size = sizeof(args.data);
    require_test(pthread_create(&thread, NULL, read_transfer, &args) == 0, "start read before removal");
    pthread_mutex_lock(&fake_mutex);
    while (!fake_submitted)
        pthread_cond_wait(&fake_condition, &fake_mutex);
    pthread_mutex_unlock(&fake_mutex);
    capture_cancel(&c, 0);
    require_test(pthread_join(thread, NULL) == 0 && !args.result && c.diagnostics[1].failures == 1 &&
                 c.diagnostics[1].interruptions == 0, "removal cancellation remains a failure");
    usb_transfer_lifecycle_destroy(&c.lifecycle);
    pthread_mutex_destroy(&c.diagnostics_mutex);

    child = fork();
    require_test(child >= 0, "fork callback-loss test");
    if (!child) {
        _Uint32t transfer_status, actual;
        int completion_rc;
        reset_fake();
        fake_pending = 1;
        fake_abort_no_callback = 1;
        bulk_transfer_t(&urb, &pipe, dma, sizeof(dma), 1, &transfer_status, &actual, &completion_rc, 1, 0, 0, 0, 0);
        _exit(1);
    }
    require_test(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 6,
                 "unconfirmed abort exits without releasing the callback context");
    puts("USB_CAPTURE_FAILURES_OK");
    return 0;
}
