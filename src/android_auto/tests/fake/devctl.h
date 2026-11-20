#ifndef TEST_DEVCTL_H
#define TEST_DEVCTL_H

struct iov_t;
static inline int devctlv(int, int, int, int, const iov_t*, iov_t*, int*) { return -1; }

#endif
