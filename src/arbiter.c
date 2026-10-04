/*
 * PRTERM - CB & Amateur Radio Terminal
 * arbiter.c - Senderegelung bei mehreren TNCs.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "arbiter.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void pr_arbiter_path(const char *runtime_dir, long freq_hz,
                     char *dst, size_t dstlen)
{
    /* Eine Datei je Frequenz - Geraete auf verschiedenen Kanaelen
     * behindern sich nicht. */
    snprintf(dst, dstlen, "%.480s/tx-%ld.lock", runtime_dir, freq_hz);
}

/*
 * Versucht, die Sperre zu holen. Liefert den Dateideskriptor oder -1.
 *
 * Es wird bewusst F_SETLK verwendet und nicht flock(): das ist reines
 * POSIX und damit auf Linux und FreeBSD identisch. Die Sperre faellt
 * automatisch, wenn der Prozess endet - ein haengengebliebenes CGI kann
 * den Kanal damit nicht dauerhaft blockieren.
 */
static int try_lock(int fd, const char *owner)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;

    if (fcntl(fd, F_SETLK, &fl) != 0)
        return -1;

    /* Eigentuemer merken - fuer Anzeige und Diagnose */
    if (owner != NULL && owner[0] != '\0') {
        char buf[PR_ARBITER_OWNER_LEN + 2];
        pr_strlcpy(buf, owner, sizeof buf);
        (void)ftruncate(fd, 0);
        (void)lseek(fd, 0, SEEK_SET);
        (void)write(fd, buf, strlen(buf));
    }
    return 0;
}

int pr_arbiter_acquire(const char *runtime_dir, long freq_hz,
                       const char *owner, int timeout_ms,
                       char *err, size_t errlen)
{
    char path[600];
    pr_arbiter_path(runtime_dir, freq_hz, path, sizeof path);

    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        snprintf(err, errlen, "Sendesperre nicht anlegbar: %s", strerror(errno));
        return -1;
    }

    if (try_lock(fd, owner) == 0)
        return fd;

    /* Belegt - mit Zeitlimit warten */
    long long deadline = pr_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        if (try_lock(fd, owner) == 0)
            return fd;

        if (pr_now_ms() >= deadline) {
            char other[PR_ARBITER_OWNER_LEN + 2];
            pr_strlcpy(other, "", sizeof other);
            ssize_t k = pread(fd, other, sizeof other - 1, 0);
            if (k > 0) other[k] = '\0';

            if (other[0] != '\0')
                snprintf(err, errlen,
                         "der Kanal ist belegt - %s sendet gerade", other);
            else
                snprintf(err, errlen, "der Kanal ist gerade belegt");
            close(fd);
            return -1;
        }
        usleep(50000);                  /* 50 ms warten, dann erneut */
    }
}

void pr_arbiter_release(int fd)
{
    if (fd < 0)
        return;

    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    (void)fcntl(fd, F_SETLK, &fl);
    close(fd);
}

bool pr_arbiter_busy(const char *runtime_dir, long freq_hz,
                     char *owner, size_t ownerlen)
{
    char path[600];
    pr_arbiter_path(runtime_dir, freq_hz, path, sizeof path);

    int fd = open(path, O_RDONLY | O_CREAT, 0600);
    if (fd < 0)
        return false;

    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;

    /* Wenn wir die Sperre bekommen koennen, ist gerade niemand am Senden.
     * Sofort wieder freigeben. */
    bool busy = (fcntl(fd, F_SETLK, &fl) != 0);
    if (!busy) {
        fl.l_type = F_UNLCK;
        (void)fcntl(fd, F_SETLK, &fl);
    } else if (owner != NULL && ownerlen > 0) {
        ssize_t k = pread(fd, owner, ownerlen - 1, 0);
        if (k > 0) owner[k] = '\0';
        else       owner[0] = '\0';
    }

    close(fd);
    return busy;
}
