#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../usb_transfer_lifecycle.h"

static void require(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

struct Submit {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool entered, release;
    unsigned calls;
    Submit() : entered(false), release(false), calls(0)
    {
        pthread_mutex_init(&mutex, 0);
        pthread_cond_init(&condition, 0);
    }
    ~Submit()
    {
        pthread_cond_destroy(&condition);
        pthread_mutex_destroy(&mutex);
    }
};

static int blocked_submit(void* context)
{
    Submit* s = static_cast<Submit*>(context);
    pthread_mutex_lock(&s->mutex);
    ++s->calls;
    s->entered = true;
    pthread_cond_broadcast(&s->condition);
    while (!s->release)
        pthread_cond_wait(&s->condition, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

static int immediate_submit(void* context)
{
    Submit* s = static_cast<Submit*>(context);
    pthread_mutex_lock(&s->mutex);
    ++s->calls;
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

struct SubmitThread {
    usb_transfer_lifecycle* lifecycle;
    Submit* submit;
    int result;
};

static void* run_submit(void* context)
{
    SubmitThread* t = static_cast<SubmitThread*>(context);
    t->result = usb_transfer_submit_guarded(t->lifecycle, 1, blocked_submit, t->submit);
    return 0;
}

static void* release_submit(void* context)
{
    Submit* s = static_cast<Submit*>(context);
    usleep(10000);
    pthread_mutex_lock(&s->mutex);
    s->release = true;
    pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

struct CancelThread {
    usb_transfer_lifecycle* lifecycle;
    unsigned active;
};

static void* run_cancel(void* context)
{
    CancelThread* t = static_cast<CancelThread*>(context);
    t->active = usb_transfer_cancel(t->lifecycle);
    return 0;
}

int main()
{
    usb_transfer_lifecycle lifecycle;
    Submit submit;
    require(usb_transfer_lifecycle_init(&lifecycle) == 0, "initialize lifecycle");
    require(usb_transfer_cancel(&lifecycle) == 0, "cancel before submit");
    require(usb_transfer_is_stopping(&lifecycle), "cancel flag is observable by waiters");
    require(usb_transfer_submit_guarded(&lifecycle, 0, immediate_submit, &submit) == USB_TRANSFER_STOPPED,
            "cancelled lifecycle rejects submit");
    require(submit.calls == 0, "rejected submit never calls USB");
    usb_transfer_lifecycle_destroy(&lifecycle);

    Submit gate;
    SubmitThread submission = {0, 0, 0};
    CancelThread cancellation = {0, 0};
    pthread_t submitter, releaser, canceller;
    require(usb_transfer_lifecycle_init(&lifecycle) == 0, "initialize race lifecycle");
    submission.lifecycle = &lifecycle;
    submission.submit = &gate;
    require(pthread_create(&submitter, 0, run_submit, &submission) == 0, "start submit");
    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered)
        pthread_cond_wait(&gate.condition, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);
    cancellation.lifecycle = &lifecycle;
    require(pthread_create(&releaser, 0, release_submit, &gate) == 0, "start release");
    require(pthread_create(&canceller, 0, run_cancel, &cancellation) == 0, "start cancel");
    require(pthread_join(submitter, 0) == 0 && pthread_join(canceller, 0) == 0 &&
            pthread_join(releaser, 0) == 0, "join submit/cancel race");
    require(submission.result == 0, "in-progress submit completes");
    require(cancellation.active == 2, "cancel observes the submitted direction");
    require(usb_transfer_is_stopping(&lifecycle), "waiting transfer observes cancellation");
    usb_transfer_finished(&lifecycle, 1);
    require(usb_transfer_cancel(&lifecycle) == 0, "finished transfer is no longer active");
    usb_transfer_lifecycle_destroy(&lifecycle);

    Submit active;
    require(usb_transfer_lifecycle_init(&lifecycle) == 0, "initialize multi-direction lifecycle");
    require(usb_transfer_submit_guarded(&lifecycle, 0, immediate_submit, &active) == 0,
            "submit OUT");
    require(usb_transfer_submit_guarded(&lifecycle, 1, immediate_submit, &active) == 0,
            "submit IN");
    require(usb_transfer_cancel(&lifecycle) == 3, "cancel reports both active directions");
    require(!usb_transfer_claim_abort(&lifecycle, 0) && !usb_transfer_claim_abort(&lifecycle, 1),
            "waiter cannot abort after cancellation claimed the pipes");
    usb_transfer_finished(&lifecycle, 0);
    usb_transfer_finished(&lifecycle, 1);
    usb_transfer_lifecycle_destroy(&lifecycle);

    require(usb_transfer_lifecycle_init(&lifecycle) == 0, "initialize abort ownership lifecycle");
    require(usb_transfer_submit_guarded(&lifecycle, 1, immediate_submit, &active) == 0, "submit read");
    require(usb_transfer_claim_abort(&lifecycle, 1), "timeout claims abort");
    require(usb_transfer_cancel(&lifecycle) == 0, "cancellation does not abort an already claimed pipe");
    usb_transfer_finished(&lifecycle, 1);
    usb_transfer_lifecycle_destroy(&lifecycle);
    puts("USB_TRANSFER_LIFECYCLE_OK");
    return 0;
}
