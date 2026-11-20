#ifndef TEST_NEUTRINO_H
#define TEST_NEUTRINO_H

struct iov_t { void* base; int length; };
#define SETIOV(iov, ptr, len) do { (iov)->base = (ptr); (iov)->length = (len); } while (0)

#endif
