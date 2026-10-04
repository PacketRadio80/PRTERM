/*
 * PRTERM - CB & Amateur Radio Terminal
 * serial.h - portable serielle Schnittstelle.
 *
 * POSIX.1-2008, keine GNU-Extensions. Laeuft auf Linux und FreeBSD,
 * x86-64 und arm64. Siehe src/prterm_compat.h.
 *
 * Bewusst nicht verwendet:
 *   cfmakeraw()  BSD/GNU   -> Felder von Hand setzen
 *   cfsetspeed() BSD/GNU   -> cfsetispeed + cfsetospeed ueber B*-Switch
 *   CRTSCTS      nicht portabel, und bei TNCs unerwuenscht
 *
 * SPDX-License-Identifier: MIT
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
    int   databits;    /* 7 oder 8 */
    int   parity;      /* pr_parity */
    int   stopbits;    /* 1 oder 2 */
    bool  rts_dtr;     /* Modemleitungen hochziehen (TNC2C ja, PK-TNC2 nein) */
    bool  open;
} pr_serial;

/*
 * Oeffnet und konfiguriert den Port.
 *
 *   dev       /dev/ttyUSB0, /dev/ttyACM0 (Linux), /dev/cuaU0 (FreeBSD)
 *   baud      1200 .. 115200 und mehr
 *   databits  7 oder 8
 *   parity    PR_PAR_NONE / _EVEN / _ODD
 *   stopbits  1 oder 2
 *   rts_dtr   true zieht TIOCM_RTS und TIOCM_DTR hoch - wichtig fuer
 *             TNC2C-Klone, deren Betrieb davon abhaengt. Auf PTYs kann
 *             das fehlschlagen und wird dann still uebergangen.
 *
 * Liefert 0 bei Erfolg, sonst -1 mit Fehlergrund.
 */
int  pr_serial_open(pr_serial *s, const char *dev,
                    long baud, int databits, int parity, int stopbits,
                    bool rts_dtr, char *err, size_t errlen);
void pr_serial_close(pr_serial *s);

/* Schreibt alles und wartet auf das Absetzen (tcdrain). */
int  pr_serial_write(pr_serial *s, const void *buf, size_t len,
                     char *err, size_t errlen);

/*
 * Liest mit Zeitlimit. Liefert:
 *    >0  Anzahl gelesener Bytes
 *     0  Zeitlimit abgelaufen
 *    -1  Fehler (Grund in err, falls errlen > 0)
 */
long pr_serial_read(pr_serial *s, void *buf, size_t cap,
                    int timeout_ms, char *err, size_t errlen);

/* Liest so lange, bis timeout_ms zwischen zwei Bytes vergehen (Sammeln
 * einer Antwort). Liefert Gesamtzahl der gelesenen Bytes. */
long pr_serial_read_quiet(pr_serial *s, void *buf, size_t cap,
                          int timeout_ms, int quiet_ms,
                          char *err, size_t errlen);

/* Puffer leeren (Eingabe, Ausgabe oder beides). */
int  pr_serial_flush(pr_serial *s, bool input, bool output);

bool pr_serial_ok(const pr_serial *s);

/* Leitungsstatus lesen; liefert -1 wenn nicht unterstuetzt. */
int  pr_serial_modem_lines(pr_serial *s);

#endif /* PRTERM_SERIAL_H */
