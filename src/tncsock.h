/*
 * PRTERM - CB & Amateur Radio Terminal
 * tncsock.h - Verbindung zum TNC-Daemon (prterm-tncd).
 *
 * Hintergrund
 * ===========
 * Ein CGI-Prozess endet nach jeder Anfrage. Damit faellt der letzte
 * Dateideskriptor, und USB-Serienadapter setzen zurueck - DTR faellt
 * auch dann, wenn HUPCL aus ist. Der TNC2C verlaesst daraufhin den
 * KISS-Modus und geht in einen Echo-only-Zustand.
 *
 * Genau darum haelt ein eigener DAEMON die Ports offen. Das ist der
 * Aufbau, den auch der MAX25-Stack waehlt: ein Prozess, ein offener
 * Port, dauerhaft. Der Daemon betritt KISS, haelt es und prueft es.
 *
 * Das CGI beruehrt das Geraet ueberhaupt nicht mehr - es schreibt nur
 * noch Befehle an den Daemon. Das ist zugleich die wichtigste Sicherung
 * gegen unbeabsichtigte Sendungen: nichts, was das CGI tut, geht
 * ungefiltert auf die Luft.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_TNCSOCK_H
#define PRTERM_TNCSOCK_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Protokoll - zeilenweise ueber einen Unix-Domain-Socket.
 *
 * Anfrage:   BEFEHL [Argument]\n
 * Antwort:   OK [Daten]\n   oder   ERR [Text]\n
 *
 * Nutzdaten werden als Hex kodiert, damit keine Zeichenkonflikte mit
 * den Steuerzeichen des Protokolls auftreten koennen.
 */
#define PR_TNCSOCK_MAX_LINE 8192

/* Befehle */
#define PR_TNC_CMD_PING     "PING"
#define PR_TNC_CMD_TX       "TX"        /* TX <hex> - Bytes senden */
#define PR_TNC_CMD_RX       "RX"        /* empfangene Bytes abholen */
#define PR_TNC_CMD_STATUS   "STATUS"
#define PR_TNC_CMD_CHECKUP  "CHECKUP"   /* KISS sicherstellen */
#define PR_TNC_CMD_CLOSE    "QUIT"

typedef struct pr_tncsock {
    int fd;
    char last_err[256];
} pr_tncsock;

/* Verbindung zum Daemon einer Station. */
int  pr_tncsock_open(pr_tncsock *c, const char *socket_path,
                     char *err, size_t errlen);
void pr_tncsock_close(pr_tncsock *c);

/* Rohbefehl. Liefert 0 bei OK, sonst -1; die Antwort steht in out. */
int  pr_tncsock_cmd(pr_tncsock *c, const char *cmd,
                    char *out, size_t outlen, char *err, size_t errlen);

/* Komfort */
int  pr_tncsock_ping(pr_tncsock *c, char *err, size_t errlen);
int  pr_tncsock_tx(pr_tncsock *c, const unsigned char *data, size_t len,
                   char *err, size_t errlen);
/* Liefert die Anzahl der abgeholten Bytes, 0 wenn nichts da ist. */
long pr_tncsock_rx(pr_tncsock *c, unsigned char *out, size_t cap,
                   char *err, size_t errlen);
int  pr_tncsock_checkup(pr_tncsock *c, char *err, size_t errlen);

/* Hilfsfunktionen, die auch der Daemon nutzt */
size_t pr_tncsock_hex_encode(char *dst, size_t dstlen,
                             const unsigned char *src, size_t len);
size_t pr_tncsock_hex_decode(unsigned char *dst, size_t dstlen,
                             const char *src);

#endif /* PRTERM_TNCSOCK_H */
