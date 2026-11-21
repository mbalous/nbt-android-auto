#ifndef NBT_USB_WAIT_H
#define NBT_USB_WAIT_H

#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <string.h>

struct usb_wait {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int done;
};

static int usb_wait_init(struct usb_wait* w)
{
    pthread_condattr_t attr;
    int attr_ready = 0;
    int rc = pthread_mutex_init(&w->mutex, 0);
    if (rc)
        return rc;
    memset(&attr, 0, sizeof(attr));
    rc = pthread_condattr_init(&attr);
    attr_ready = rc == 0;
    if (!rc)
        rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc)
        rc = pthread_cond_init(&w->condition, &attr);
    if (attr_ready)
        (void)pthread_condattr_destroy(&attr);
    if (rc) {
        pthread_mutex_destroy(&w->mutex);
        return rc;
    }
    w->done = 0;
    return 0;
}

static void usb_wait_destroy(struct usb_wait* w)
{
    pthread_cond_destroy(&w->condition);
    pthread_mutex_destroy(&w->mutex);
}

static void usb_wait_done_locked(struct usb_wait* w)
{
    w->done = 1;
    pthread_cond_signal(&w->condition);
}

static void usb_wait_signal(struct usb_wait* w)
{
    int rc = pthread_mutex_lock(&w->mutex);
    if (!rc) {
        usb_wait_done_locked(w);
        pthread_mutex_unlock(&w->mutex);
    }
}

static int usb_wait_deadline(struct timespec* deadline, unsigned milliseconds)
{
    int rc = clock_gettime(CLOCK_MONOTONIC, deadline);
    if (rc)
        return -(errno ? errno : EIO);
    deadline->tv_sec += (time_t)(milliseconds / 1000);
    deadline->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int usb_wait_await_until(struct usb_wait* w, const struct timespec* deadline)
{
    int rc = pthread_mutex_lock(&w->mutex);
    if (rc)
        return -rc;
    rc = 0;
    while (!w->done && (!rc || rc == EINTR))
        rc = pthread_cond_timedwait(&w->condition, &w->mutex, deadline);
    int done = w->done;
    pthread_mutex_unlock(&w->mutex);
    if (done)
        return 1;
    return rc == ETIMEDOUT ? 0 : -rc;
}

static int usb_wait_await(struct usb_wait* w, unsigned milliseconds)
{
    struct timespec deadline;
    int rc = usb_wait_deadline(&deadline, milliseconds);
    return rc ? rc : usb_wait_await_until(w, &deadline);
}

static int usb_wait_await_cancellable(struct usb_wait* w, unsigned milliseconds,
                                      int (*stopping)(void*), void* context)
{
    struct timespec deadline;
    int rc = usb_wait_deadline(&deadline, milliseconds);
    if (rc)
        return rc;
    for (;;) {
        struct timespec slice;
        int expired, result;
        rc = usb_wait_deadline(&slice, 100);
        if (rc)
            return rc;
        expired = slice.tv_sec > deadline.tv_sec ||
                  (slice.tv_sec == deadline.tv_sec && slice.tv_nsec >= deadline.tv_nsec);
        if (expired)
            slice = deadline;
        result = usb_wait_await_until(w, &slice);
        if (result != 0)
            return result;
        if (stopping(context))
            return -ECANCELED;
        if (expired)
            return 0;
    }
}

#endif
