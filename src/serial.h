/*
 * PRTERM - CB & Amateur Radio Terminal
 * serial.h - portable serial interface.
 *
 * POSIX.1-2008, no GNU extensions. Runs on Linux and FreeBSD,
 * x86-64 and arm64. See src/prterm_compat.h.
 *
 * Deliberately not used:
 *   cfmakeraw()  BSD/GNU   -> set fields by hand
 *   cfsetspeed() BSD/GNU   -> cfsetispeed + cfsetospeed via B* switch
 *   CRTSCTS      not portable, and unwanted for TNCs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_SERIAL_H
#define PRTERM_SERIAL_H

#include <stdbool.h>
#include <stddef.h>

typedef enum pr_parity {
    PR_PAR_NONE = 0,
    PR_PAR_EVEN = 1,
    PR_PAR_ODD  = 2
} pr_parity;

typedef struct pr_serial {
    int   fd;
    char  dev[256];
    long  baud;
    int   databits;    /* 7 or 8   */
    int   parity;      /* pr_parity */
    int   stopbits;    /* 1 or 2   */
    bool  rts_dtr;     /* Pull up modem lines (TNC2C yes, PK-TNC2 no)        */
    bool  open;
} pr_serial;

/*
 * Opens and configures the port.
 *
 *   dev       /dev/ttyUSB0, /dev/ttyACM0 (Linux), /dev/cuaU0 (FreeBSD)
 *   baud      1200 .. 115200 and more
 *   databits  7 or 8
 *   parity    PR_PAR_NONE / _EVEN / _ODD
 *   stopbits  1 or 2
 *   rts_dtr   true pulls TIOCM_RTS and TIOCM_DTR up - important for
 *             TNC2C clones whose operation depends on it. On PTYs this
 *             can fail and is then silently skipped.
 *
 * Returns 0 on success, otherwise -1 with the error reason.
 */
int  pr_serial_open(pr_serial *s, const char *dev,
                    long baud, int databits, int parity, int stopbits,
                    bool rts_dtr, char *err, size_t errlen);
void pr_serial_close(pr_serial *s);

/* Writes everything and waits for the drain (tcdrain).  */
int  pr_serial_write(pr_serial *s, const void *buf, size_t len,
                     char *err, size_t errlen);

/*
 * Reads with a timeout. Returns:
 *    >0  number of bytes read
 *     0  timeout expired
 *    -1  error (reason in err, if errlen > 0)
 */
long pr_serial_read(pr_serial *s, void *buf, size_t cap,
                    int timeout_ms, char *err, size_t errlen);

/* Reads until timeout_ms pass between two bytes (collecting a
 * response). Returns the total number of bytes read. */
long pr_serial_read_quiet(pr_serial *s, void *buf, size_t cap,
                          int timeout_ms, int quiet_ms,
                          char *err, size_t errlen);

/* Flush the buffer (input, output or both).     */
int  pr_serial_flush(pr_serial *s, bool input, bool output);

/*
 * CHANGE line format and baud rate without closing the port.
 *
 * This matters: closing drops DTR, and a TNC2C treats a falling
 * DTR as a signal to enter an echo-only state where it does not
 * respond. Anyone trying several profiles must therefore not
 * close the port in between.
 */
int  pr_serial_reconfigure(pr_serial *s, long baud, int databits,
                           int parity, int stopbits, char *err, size_t errlen);

/*
 * Holds the DTR/RTS lines while a device is powered on.
 * The port stays open the whole time.
 */
int  pr_serial_hold_dtr(pr_serial *s, char *err, size_t errlen);

/* Parses a line format like "8n1" / "7e1". Returns false if invalid.       */
bool pr_serial_parse_line(const char *s, int *databits, int *parity, int *stopbits);

bool pr_serial_ok(const pr_serial *s);

/* Read line status; returns -1 if unsupported.              */
int  pr_serial_modem_lines(pr_serial *s);

#endif /* PRTERM_SERIAL_H */
