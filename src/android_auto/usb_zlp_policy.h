#ifndef NBT_USB_ZLP_POLICY_H
#define NBT_USB_ZLP_POLICY_H

static inline unsigned usb_packet_size(unsigned descriptor_value)
{
    return descriptor_value & 0x07ffU;
}

static inline int usb_out_needs_zlp(unsigned transferred, unsigned max_packet)
{
    return transferred != 0 && max_packet != 0 && transferred % max_packet == 0;
}

static inline int usb_zlp_succeeded(int io_rc, int completion_rc, unsigned status,
                                    unsigned status_mask, unsigned success_status, unsigned actual)
{
    return io_rc == 0 && completion_rc == 0 &&
           (status & status_mask) == success_status && actual == 0;
}

#endif
