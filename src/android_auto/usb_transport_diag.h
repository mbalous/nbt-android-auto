#ifndef NBT_USB_TRANSPORT_DIAG_H
#define NBT_USB_TRANSPORT_DIAG_H

enum usb_transfer_phase {
    USB_TRANSFER_PHASE_NONE = 0,
    USB_TRANSFER_PHASE_INIT = 1,
    USB_TRANSFER_PHASE_SETUP = 2,
    USB_TRANSFER_PHASE_SUBMIT = 3,
    USB_TRANSFER_PHASE_CALLBACK = 4,
    USB_TRANSFER_PHASE_COMPLETE = 5
};

struct usb_transfer_diag {
    unsigned long long attempts, bytes_requested, bytes_actual;
    unsigned failures, zlp_attempts, zlp_failures;
    unsigned max_request, max_actual, max_elapsed_ms;
    unsigned first_failure;
    int first_io_rc, first_completion_rc;
    unsigned first_status, first_request, first_actual, first_elapsed_ms;
};

static inline unsigned usb_diag_saturate_add(unsigned value)
{
    return value == ~0U ? value : value + 1;
}

static inline unsigned long long usb_diag_saturate_bytes(unsigned long long value, unsigned amount)
{
    const unsigned long long candidate = value + amount;
    return candidate < value ? ~(unsigned long long)0 : candidate;
}

static inline void usb_transfer_diag_record(struct usb_transfer_diag* diag, int success,
                                            unsigned phase, unsigned request, unsigned actual,
                                            unsigned elapsed_ms, int io_rc, int completion_rc,
                                            unsigned status)
{
    diag->attempts = usb_diag_saturate_bytes(diag->attempts, 1);
    diag->bytes_requested = usb_diag_saturate_bytes(diag->bytes_requested, request);
    diag->bytes_actual = usb_diag_saturate_bytes(diag->bytes_actual, actual);
    if (request > diag->max_request)
        diag->max_request = request;
    if (actual > diag->max_actual)
        diag->max_actual = actual;
    if (elapsed_ms > diag->max_elapsed_ms)
        diag->max_elapsed_ms = elapsed_ms;
    if (!success) {
        diag->failures = usb_diag_saturate_add(diag->failures);
        if (!diag->first_failure) {
            diag->first_failure = phase;
            diag->first_io_rc = io_rc;
            diag->first_completion_rc = completion_rc;
            diag->first_status = status;
            diag->first_request = request;
            diag->first_actual = actual;
            diag->first_elapsed_ms = elapsed_ms;
        }
    }
}

static inline void usb_transfer_diag_zlp(struct usb_transfer_diag* diag, int success)
{
    diag->zlp_attempts = usb_diag_saturate_add(diag->zlp_attempts);
    if (!success)
        diag->zlp_failures = usb_diag_saturate_add(diag->zlp_failures);
}

#endif
