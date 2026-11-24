#define main native_service_main
#include "../nbt_native_service.c"
#undef main

static void check_switch(const char* path, const char* log, int expected)
{
    FILE* file = fopen(path, "w");
    if (!file || fputs(log, file) == EOF || fclose(file) || switch_log_action(path) != expected) {
        fprintf(stderr, "switch decision failed for: %s\n", log);
        exit(1);
    }
}

static void check_reset(UsbRetry failed, UsbRetry current, int expected, const char* scenario)
{
    if (usb_retry_matches(failed, current) != expected) {
        fprintf(stderr, "unexpected USB reset decision: %s\n", scenario);
        exit(1);
    }
}

static void test_reset_target(void)
{
    UsbRetry accessory, normal;
    found_aoa = AOA_PRODUCT_ACCESSORY;
    accessory = usb_retry_target(PHONE_ACCESSORY, 0, 3);
    found_v = 0x18d1;
    found_p = 0x4ee1;
    normal = usb_retry_target(PHONE_NORMAL, 0, 3);

    check_reset(accessory, accessory, 1, "stuck accessory");
    check_reset(accessory, usb_retry_target(PHONE_ACCESSORY, 0, 4), 0, "accessory on new address");
    check_reset(accessory, normal, 0, "back in normal mode at same address");
    check_reset(accessory, usb_retry_target(PHONE_NONE, 0, 0), 0, "absent or ambiguous");
    check_reset(normal, normal, 1, "failed AOA switch with same normal-mode target");
    check_reset(normal, accessory, 0, "late AOA enumeration");
    found_p = 0x4ee2;
    check_reset(normal, usb_retry_target(PHONE_NORMAL, 0, 3), 0, "different normal-mode product at same address");
    found_aoa = AOA_PRODUCT_ACCESSORY_ADB;
    check_reset(accessory, usb_retry_target(PHONE_ACCESSORY, 0, 3), 0, "different accessory product at same address");
}

int main(void)
{
    char path[] = "/tmp/nbt-switch-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    close(fd);
    check_switch(path, "AOA_PROTOCOL_OK version=2\n", AOA_SWITCH_WAIT);
    check_switch(path, "AOA_PROTOCOL_OK version=2\nAOA_START_SENT_INSPECT_USB\n", AOA_SWITCH_WAIT);
    check_switch(path, "AOA_PROTOCOL_OK version=2\nAOA_START_OUTCOME_UNKNOWN_INSPECT_USB_NO_RETRY\n",
                 AOA_SWITCH_WAIT);
    check_switch(path, "AOA_PROTOCOL_UNSUPPORTED\n", AOA_SWITCH_UNSUPPORTED);
    check_switch(path, "AOA_REQUEST id=51 index=0 result=0 status=02000004 bytes=0\n", AOA_SWITCH_UNSUPPORTED);
    check_switch(path, "AOA_REQUEST id=51 index=0 result=0 status=01000000 bytes=1\n", AOA_SWITCH_UNSUPPORTED);
    check_switch(path, "AOA_CONNECT_FAILED code=1\n", AOA_SWITCH_RETRY);
    unlink(path);
    test_reset_target();
    return 0;
}
