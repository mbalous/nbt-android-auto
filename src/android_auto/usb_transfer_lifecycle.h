#ifndef NBT_USB_TRANSFER_LIFECYCLE_H
#define NBT_USB_TRANSFER_LIFECYCLE_H

#include <pthread.h>
#include <string.h>

#define USB_TRANSFER_STOPPED -1

struct usb_transfer_lifecycle {
    pthread_mutex_t mutex;
    int stopping;
    int active[2];
};

typedef int (*usb_transfer_submit)(void*);

static inline int usb_transfer_lifecycle_init(struct usb_transfer_lifecycle* lifecycle)
{
    memset(lifecycle, 0, sizeof(*lifecycle));
    return pthread_mutex_init(&lifecycle->mutex, 0);
}

static inline void usb_transfer_lifecycle_destroy(struct usb_transfer_lifecycle* lifecycle)
{
    pthread_mutex_destroy(&lifecycle->mutex);
}

static inline int usb_transfer_submit_guarded(struct usb_transfer_lifecycle* lifecycle,
                                              int direction, usb_transfer_submit submit, void* context)
{
    int rc;
    if (pthread_mutex_lock(&lifecycle->mutex))
        return USB_TRANSFER_STOPPED;
    if (lifecycle->stopping) {
        pthread_mutex_unlock(&lifecycle->mutex);
        return USB_TRANSFER_STOPPED;
    }
    rc = submit(context);
    if (rc == 0)
        lifecycle->active[direction] = 1;
    pthread_mutex_unlock(&lifecycle->mutex);
    return rc;
}

static inline void usb_transfer_finished(struct usb_transfer_lifecycle* lifecycle, int direction)
{
    if (pthread_mutex_lock(&lifecycle->mutex))
        return;
    lifecycle->active[direction] = 0;
    pthread_mutex_unlock(&lifecycle->mutex);
}

static inline unsigned usb_transfer_cancel(struct usb_transfer_lifecycle* lifecycle)
{
    unsigned active = 0;
    if (pthread_mutex_lock(&lifecycle->mutex))
        return 0;
    lifecycle->stopping = 1;
    if (lifecycle->active[0])
        active |= 1;
    if (lifecycle->active[1])
        active |= 2;
    pthread_mutex_unlock(&lifecycle->mutex);
    return active;
}

static inline int usb_transfer_is_stopping(struct usb_transfer_lifecycle* lifecycle)
{
    int stopping = 0;
    if (pthread_mutex_lock(&lifecycle->mutex))
        return 1;
    stopping = lifecycle->stopping;
    pthread_mutex_unlock(&lifecycle->mutex);
    return stopping;
}

#endif
