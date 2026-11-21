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
    return 0;
}
