/*
 * PRTERM - CB & Amateur Radio Terminal
 * prterm_compat.h - Feature test macros.
 *
 * MUST be included first in every .c file, i.e. BEFORE every system
 * header. With strict -std=c11 (no GNU extensions) the glibc and
 * FreeBSD headers otherwise hide POSIX APIs.
 *
 * The lower bound is POSIX.1-2008 - that covers Linux and FreeBSD on
 * x86-64 and arm64 equally. Deliberately not used:
 *   cfsetspeed()   BSD/GNU, not POSIX -> cfsetispeed + cfsetospeed
 *   cfmakeraw()    BSD/GNU, not POSIX -> set fields by hand
 *   flock()        BSD, not POSIX     -> fcntl(F_SETLK)
 *   getrandom()    Linux-specific     -> /dev/urandom
 *   epoll/kqueue/timerfd              -> poll()
 *   /proc/self/exe                    -> argv[0]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_COMPAT_H
#define PRTERM_COMPAT_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* On glibc/Linux additionally enable BSD/default visibility so that
 * helpers of BSD origin are declared out of the box. On FreeBSD this
 * is superfluous, there the headers are open anyway. */
#if defined(__linux__) || defined(__GLIBC__)
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#endif /* PRTERM_COMPAT_H */
