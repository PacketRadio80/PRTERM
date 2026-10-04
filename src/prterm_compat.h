/*
 * PRTERM - CB & Amateur Radio Terminal
 * prterm_compat.h - Feature-Test-Macros.
 *
 * MUSS als allererstes in jeder .c-Datei eingebunden werden, also VOR jedem
 * System-Header. Mit strictem -std=c11 (keine GNU-Extensions) verstecken
 * glibc und FreeBSD-Header sonst POSIX-APIs.
 *
 * Untergrenze ist POSIX.1-2008 - das deckt Linux und FreeBSD auf x86-64
 * und arm64 gleichmaessig ab. Bewusst nicht verwendet:
 *   cfsetspeed()   BSD/GNU, nicht POSIX  -> cfsetispeed + cfsetospeed
 *   cfmakeraw()    BSD/GNU, nicht POSIX  -> Felder von Hand setzen
 *   flock()        BSD, nicht POSIX      -> fcntl(F_SETLK)
 *   getrandom()    Linux-spezifisch      -> /dev/urandom
 *   epoll/kqueue/timerfd                 -> poll()
 *   /proc/self/exe                       -> argv[0]
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_COMPAT_H
#define PRTERM_COMPAT_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* Auf glibc/Linux zusaetzlich die BSD-/Default-Sichtbarkeit einschalten,
 * damit auch von Haus aus BSD-stammende Helfer deklariert sind.
 * Auf FreeBSD ist das ueberfluessig, dort sind die Header ohnehin offen. */
#if defined(__linux__) || defined(__GLIBC__)
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#endif /* PRTERM_COMPAT_H */
