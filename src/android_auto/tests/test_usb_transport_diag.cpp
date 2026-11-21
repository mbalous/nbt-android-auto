#include <stdio.h>
#include <stdlib.h>
#include "../usb_transport_diag.h"

static void require(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main()
{
    usb_transfer_diag diag = {};
    usb_transfer_diag_record(&diag, 1, USB_TRANSFER_PHASE_COMPLETE, 512, 512, 2, 0, 0, 0x01000000U);
    require(diag.attempts == 1 && diag.failures == 0, "successful observation");
    usb_transfer_diag_record(&diag, 0, USB_TRANSFER_PHASE_CALLBACK, 65536, 4608, 8123, 260, 19, 0x02000005U);
    usb_transfer_diag_record(&diag, 0, USB_TRANSFER_PHASE_COMPLETE, 1024, 0, 10, 0, 19, 0x02000005U);
    require(diag.attempts == 3 && diag.failures == 2, "bounded counters retain failures");
    require(diag.max_request == 65536 && diag.max_actual == 4608 && diag.max_elapsed_ms == 8123,
            "maximum lengths and timing retained");
    require(diag.first_failure == USB_TRANSFER_PHASE_CALLBACK && diag.first_io_rc == 260 &&
            diag.first_completion_rc == 19 && diag.first_actual == 4608,
            "first failure retains phase and completion outcome");
    usb_transfer_diag_zlp(&diag, 0);
    usb_transfer_diag_zlp(&diag, 1);
    require(diag.zlp_attempts == 2 && diag.zlp_failures == 1, "ZLP outcome retained");
    diag.attempts = ~(unsigned long long)0;
    usb_transfer_diag_record(&diag, 1, USB_TRANSFER_PHASE_COMPLETE, 1, 1, 1, 0, 0, 0x01000000U);
    require(diag.attempts == ~(unsigned long long)0, "counter saturation remains bounded");
    puts("USB_TRANSPORT_DIAGNOSTICS_OK");
    return 0;
}
