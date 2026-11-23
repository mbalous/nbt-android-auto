/* Project-written opaque QNX 6.5 x86 attribute storage used by accessory_window. */
#ifndef _QNX_OPEN_SYS_IOFUNC_H
#define _QNX_OPEN_SYS_IOFUNC_H
#include <sys/types.h>
#include <sys/dispatch.h>

typedef struct _iofunc_attr {
  unsigned char _opaque[88];
} __attribute__((aligned(8))) iofunc_attr_t;
struct _client_info;

__BEGIN_DECLS
void iofunc_func_init(unsigned int, resmgr_connect_funcs_t *, unsigned int, resmgr_io_funcs_t *);
void iofunc_attr_init(iofunc_attr_t *, mode_t, iofunc_attr_t *, struct _client_info *);
__END_DECLS
#endif
