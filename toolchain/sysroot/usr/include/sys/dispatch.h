/* Project-written subset for the QNX 6.5 x86 resource-manager ABI.
 * Opaque storage and callback slots cover accessory_window only. */
#ifndef _QNX_OPEN_SYS_DISPATCH_H
#define _QNX_OPEN_SYS_DISPATCH_H
#include <_qnx_open.h>
#include <time.h>

typedef struct _dispatch dispatch_t;
typedef struct _dispatch_context dispatch_context_t;
typedef struct _resmgr_context resmgr_context_t;
typedef union _io_open io_open_t;
typedef struct _iofunc_attr RESMGR_HANDLE_T;
typedef int (*_qnx_open_resmgr_open_fn)(resmgr_context_t *, io_open_t *, RESMGR_HANDLE_T *, void *);
typedef int (*_qnx_open_resmgr_slot_fn)(void);

typedef struct {
  unsigned int _count;
  _qnx_open_resmgr_open_fn open;
  _qnx_open_resmgr_slot_fn unlink;
  _qnx_open_resmgr_slot_fn rename;
  _qnx_open_resmgr_slot_fn mknod;
  _qnx_open_resmgr_slot_fn readlink;
  _qnx_open_resmgr_slot_fn link;
  _qnx_open_resmgr_slot_fn _unblock;
  _qnx_open_resmgr_slot_fn mount;
} resmgr_connect_funcs_t;
typedef struct { unsigned int _opaque[27]; } resmgr_io_funcs_t;
typedef struct {
  unsigned int _reserved0;
  unsigned int nparts_max;
  unsigned int msg_max_size;
  unsigned int _reserved[5];
} resmgr_attr_t;

#define _RESMGR_CONNECT_NFUNCS 8
#define _RESMGR_IO_NFUNCS 26
#define _RESMGR_DETACH_ALL 0
enum _file_type { _FTYPE_ANY = 0 };

__BEGIN_DECLS
dispatch_t *dispatch_create(void);
int dispatch_destroy(dispatch_t *);
dispatch_context_t *dispatch_context_alloc(dispatch_t *);
void dispatch_context_free(dispatch_context_t *);
int dispatch_timeout(dispatch_t *, struct timespec *);
dispatch_context_t *dispatch_block(dispatch_context_t *);
int dispatch_handler(dispatch_context_t *);
int resmgr_attach(dispatch_t *, resmgr_attr_t *, const char *, enum _file_type, unsigned int, const resmgr_connect_funcs_t *, const resmgr_io_funcs_t *, RESMGR_HANDLE_T *);
int resmgr_detach(dispatch_t *, int, unsigned int);
__END_DECLS
#endif
