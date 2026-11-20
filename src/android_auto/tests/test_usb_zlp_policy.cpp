#include <stdio.h>
#include <stdlib.h>
#include "../usb_zlp_policy.h"

static void require(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main()
{
    const unsigned mask = 0xff000000U;
    const unsigned complete = 0x01000000U;
    require(usb_packet_size(0x0200) == 0x0200, "packet size");
    require(usb_packet_size(0x1e00) == 0x0600, "descriptor transaction bits are excluded");
    require(!usb_out_needs_zlp(0, 512), "empty transfer does not need another ZLP");
    require(!usb_out_needs_zlp(49, 512), "short transfer is self-delimiting");
    require(!usb_out_needs_zlp(513, 512), "trailing short packet is self-delimiting");
    require(usb_out_needs_zlp(512, 512), "one-packet transfer needs ZLP");
    require(usb_out_needs_zlp(16384, 512), "multi-packet boundary needs ZLP");
    require(!usb_out_needs_zlp(512, 0), "invalid packet size cannot create a boundary decision");
    require(usb_zlp_succeeded(0, 0, complete, mask, complete, 0),
            "confirmed zero-byte completion succeeds");
    require(!usb_zlp_succeeded(-1, -1, 0, mask, complete, 0), "setup failure is fatal");
    require(!usb_zlp_succeeded(0, 260, 0, mask, complete, 0), "completion timeout is fatal");
    require(!usb_zlp_succeeded(0, 19, 0x06000000U, mask, complete, 0), "abort result is fatal");
    require(!usb_zlp_succeeded(0, 0, 0x02000005U, mask, complete, 0), "USB status failure is fatal");
    require(!usb_zlp_succeeded(0, 0, complete, mask, complete, 1), "nonzero ZLP length is fatal");
    puts("USB_ZLP_POLICY_OK");
    return 0;
}
