/* Compare the resource-manager layouts used by accessory_window with the QNX SDP. */
#include <stddef.h>
#include <stdio.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>

#define SHOW_TYPE(t) do { struct align_##t { char byte; t value; }; \
    printf(#t " size=%lu align=%lu\n", (unsigned long)sizeof(t), \
           (unsigned long)offsetof(struct align_##t, value)); } while (0)
#define SHOW_FIELD(t, field) printf(#t "." #field "=%lu\n", (unsigned long)offsetof(t, field))
#define SHOW_VALUE(c) printf(#c "=%ld\n", (long)(c))

int main(void) {
    SHOW_TYPE(resmgr_connect_funcs_t);
    SHOW_TYPE(resmgr_io_funcs_t);
    SHOW_TYPE(resmgr_attr_t);
    SHOW_TYPE(iofunc_attr_t);
    SHOW_FIELD(resmgr_connect_funcs_t, open);
    SHOW_FIELD(resmgr_connect_funcs_t, unlink);
    SHOW_FIELD(resmgr_connect_funcs_t, rename);
    SHOW_FIELD(resmgr_connect_funcs_t, mknod);
    SHOW_FIELD(resmgr_connect_funcs_t, readlink);
    SHOW_FIELD(resmgr_connect_funcs_t, link);
    SHOW_FIELD(resmgr_connect_funcs_t, mount);
    SHOW_FIELD(resmgr_attr_t, nparts_max);
    SHOW_FIELD(resmgr_attr_t, msg_max_size);
    printf("resmgr_attr_t.nparts_max width=%lu\n",
           (unsigned long)sizeof(((resmgr_attr_t *)0)->nparts_max));
    printf("resmgr_attr_t.msg_max_size width=%lu\n",
           (unsigned long)sizeof(((resmgr_attr_t *)0)->msg_max_size));
    SHOW_VALUE(_RESMGR_CONNECT_NFUNCS);
    SHOW_VALUE(_RESMGR_IO_NFUNCS);
    SHOW_VALUE(_FTYPE_ANY);
    SHOW_VALUE(_RESMGR_DETACH_ALL);
    return 0;
}
