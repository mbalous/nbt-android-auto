#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../usb_wait.h"

static long long elapsed_ms(const timespec& since)
{
    timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        exit(2);
    return (now.tv_sec - since.tv_sec) * 1000LL + (now.tv_nsec - since.tv_nsec) / 1000000;
}

static void require(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void mark_later(usb_wait* wait)
{
    timespec delay = {0, 10000000};
    nanosleep(&delay, 0);
    usb_wait_signal(wait);
}

static void* complete_thread(void* context)
{
    mark_later(static_cast<usb_wait*>(context));
    return 0;
}

static int delayed_stop_check(void* context)
{
    int* calls = static_cast<int*>(context);
    if (++*calls == 1) {
        timespec delay = {0, 300000000};
        while (nanosleep(&delay, &delay) == -1)
            require(errno == EINTR, "delay cancellation check");
    }
    return 0;
}

static int already_stopped(void*) { return 1; }

struct Wakeup {
    usb_wait* wait;
    bool stopped;
    pthread_mutex_t mutex;
};

static void* wake_thread(void* context)
{
    Wakeup* wakeup = static_cast<Wakeup*>(context);
    timespec delay = {0, 2000000};
    for (;;) {
        nanosleep(&delay, 0);
        pthread_mutex_lock(&wakeup->mutex);
        bool stopped = wakeup->stopped;
        pthread_mutex_unlock(&wakeup->mutex);
        if (stopped)
            return 0;
        pthread_mutex_lock(&wakeup->wait->mutex);
        pthread_cond_signal(&wakeup->wait->condition);
        pthread_mutex_unlock(&wakeup->wait->mutex);
    }
}

int main()
{
    usb_wait wait = {};
    timespec started;
    require(usb_wait_init(&wait) == 0, "initialize monotonic completion");
    require(clock_gettime(CLOCK_MONOTONIC, &started) == 0, "initial clock");
    require(usb_wait_await(&wait, 20) == 0, "empty wait times out");
    long long waited = elapsed_ms(started);
    require(waited >= 15 && waited < 500, "timeout uses one bounded deadline");
    usb_wait_destroy(&wait);

    wait = {};
    int checks = 0;
    require(usb_wait_init(&wait) == 0, "initialize sliced deadline");
    require(usb_wait_await_cancellable(&wait, 250, delayed_stop_check, &checks) == 0,
            "delayed cancellation checks still expire the overall deadline");
    require(checks >= 1 && checks <= 2, "expired overall deadline does not add more wait slices");
    require(usb_wait_await_cancellable(&wait, 500, already_stopped, 0) == -ECANCELED,
            "cancellation is observed during sliced waits");
    usb_wait_destroy(&wait);

    wait = {};
    timespec deadline;
    require(usb_wait_init(&wait) == 0, "initialize absolute wait");
    require(usb_wait_deadline(&deadline, 0) == 0, "create expired deadline");
    require(usb_wait_await_until(&wait, &deadline) == 0, "expired deadline times out");
    usb_wait_signal(&wait);
    require(usb_wait_await_until(&wait, &deadline) == 1, "completed callback wins over expired deadline");
    usb_wait_destroy(&wait);

    wait = {};
    require(usb_wait_init(&wait) == 0, "initialize pre-completed wait");
    usb_wait_signal(&wait);
    require(clock_gettime(CLOCK_MONOTONIC, &started) == 0, "pre-completed clock");
    require(usb_wait_await(&wait, 1000) == 1, "completion before wait is retained");
    require(elapsed_ms(started) < 100, "pre-completed wait does not sleep");
    usb_wait_destroy(&wait);

    wait = {};
    pthread_t completer;
    require(usb_wait_init(&wait) == 0, "initialize concurrent completion");
    require(pthread_create(&completer, 0, complete_thread, &wait) == 0, "start completer");
    require(clock_gettime(CLOCK_MONOTONIC, &started) == 0, "completion clock");
    require(usb_wait_await(&wait, 500) == 1, "completion wakes waiter");
    require(elapsed_ms(started) >= 5 && elapsed_ms(started) < 500, "completion remains bounded");
    require(pthread_join(completer, 0) == 0, "join completer");
    usb_wait_destroy(&wait);

    wait = {};
    Wakeup wakeup = {&wait, false, PTHREAD_MUTEX_INITIALIZER};
    pthread_t waker;
    require(usb_wait_init(&wait) == 0, "initialize spurious-wakeup wait");
    require(pthread_create(&waker, 0, wake_thread, &wakeup) == 0, "start waker");
    require(clock_gettime(CLOCK_MONOTONIC, &started) == 0, "spurious-wakeup clock");
    require(usb_wait_await(&wait, 40) == 0, "spurious wakeups do not extend deadline");
    waited = elapsed_ms(started);
    pthread_mutex_lock(&wakeup.mutex);
    wakeup.stopped = true;
    pthread_mutex_unlock(&wakeup.mutex);
    require(pthread_join(waker, 0) == 0, "join waker");
    require(waited >= 25 && waited < 250, "spurious wakeups remain bounded");
    pthread_mutex_destroy(&wakeup.mutex);
    usb_wait_destroy(&wait);
    puts("USB_WAIT_MONOTONIC_OK");
    return 0;
}
