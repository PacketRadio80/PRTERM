# PRTERM — TNC-Initialisierung

Abgeleitet aus dem Studienmaterial (`../0-POOL/study/`).
Nur das **Verbindliche** ist übernommen — das Vorgehen ist PRTERMs eigenes.

---

## 1. Serielle Basiseinstellung (alle TNCs)

```
open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK)
cfmakeraw()                      # PRTERM: eigener Ersatz, cfmakeraw ist nicht POSIX
c_cflag |= CLOCAL | CREAD
c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB)
c_cflag |= CS7|CS8  [+ PARENB|PARODD]  [+ CSTOPB]   # je Profil
c_cc[VMIN] = 0
c_cc[VTIME] = 5
cfsetispeed/cfsetospeed()        # siehe Portabilitaet
tcsetattr(TCSANOW)
tcflush(TCIOFLUSH)
TIOCMSET: TIOCM_RTS | TIOCM_DTR hoch    # nur wenn Profil es verlangt
Settle 2 s                           # wichtig: Port NICHT schliessen!
```

- **Keine Hardware-Flusskontrolle** — `CRTSCTS` wird nie gesetzt, `IXON/IXOFF` aus.
- Nach jedem Write: `tcdrain()`.
- Der Port bleibt zwischen Boot-Wait und Stack-Attach **offen** —
  ein DTR-Drop versetzt den TNC in Echo-only-Modus.

> **Portabilität:** `cfmakeraw()` ist BSD/GNU, nicht POSIX. PRTERM setzt die
> Felder von Hand. `cfsetspeed()` ist Linux; sonst `cfsetispeed`+`cfsetospeed`.
> `TIOCMGET/TIOCMSET` nur unter `#ifdef TIOCMGET`, und abschaltbar, weil
> `TIOCMGET` auf PTYs oft fehlschlägt.

---

## 2. Profile

| Profil   | Gerät                  | Leitung | Baud | RTS/DTR | KISS-Einstieg   |
| -------- | ---------------------- | ------- | ---- | ------- | --------------- |
| `tnc2c`  | Landolt TNC2C          | **7E1** | 19200 | an     | `ESC @K`        |
| `pktnc2` | PK-TNC2                | **8N1** | 9600 | aus    | `kiss on\r`     |
| `tmodem` | T-Modem (half-TNC)     | 8N1     | 115200 | —    | natives KISS    |
| `generic`| klassische TNC2-Klone  | 8N1     | 2400 | aus    | `kiss on\r`     |

**Achtung — Feld vs. Handbuch:** das TNC2C-Handbuch verlangt 7E1, in der Praxis
läuft der Landolt TNC2C bei **19200 8N1**. PRTERM macht das Profil konfigurierbar
und warnt beim Öffnen mit abweichender Leitung.

> Baycom/PC-COM/SER12 werden von PRTERM **nicht** unterstützt: diese Platinen
> sind reine Modems, der Host besitzt HDLC + Bit-Takt direkt über die UART-
> Register. Das ist eine eigene Geräteklasse und kein TNC.

---

## 3. Frames (byte-genau)

### Konstanten

| Zweck                    | Bytes                                 |
| ------------------------ | ------------------------------------- |
| Version/Probe            | `1B 56 0D`                            |
| Kaltstart aus EPROM      | `1B 51 52 45 53 0D`  (`ESC QRES`)     |
| **KISS betreten**        | `1B 40 4B`  (`ESC @K`, **ohne** `\r`) |
| Echo aus                 | `1B 45 30 0D`                         |
| **KISS verlassen**       | `C0 FF C0`                            |
| MYCALL setzen            | `1B 49 20 <CALL> 0D`                  |
| Puffer leeren            | `11 18`  (`^Q^X`)                     |
| Hostmode verlassen       | `00 ×300` + `00 01 06 'JHOST 0' 0D`   |

`tf_mycall_frame("cb-0") == b"\x1bI CB-0\r"`  ← **mit** Leerzeichen nach `I`

### Reihenfolge (Composite-Rezept)

1. Port öffnen, konfigurieren, RTS/DTR hoch, **2 s warten**.
2. Passiv 1,5 s auf Banner lauschen.
3. Nur bei Echo-only: Recovery-Leiter
   `C0 FF C0` → `11 18` + 300×`00` + `JHOST 0` → `ESC V` → `ESC QRES` →
   `ESC E0` → `ESC V` → zweites `ESC QRES` → `kiss off\r` + `INFO\r`.
4. `1B 49 20 <CALL> 0D`  (MYCALL) — Fehler: `?` in den ersten 32 Bytes.
5. `1B 40 4B`  (KISS on) — TAPR-Klasse: `kiss on\r`.
6. KISS-Parameter senden (§4).
7. Betrieb: `C0 00 <escaped Payload> C0`.
8. Abschalten: `C0 FF C0` — Port **offen lassen** (hält DTR).

---

## 4. KISS-Framing & Parameter

```
FEND  = 0xC0      TFEND = 0xDC
FESC  = 0xDB      TFESC = 0xDD

Escape: 0xC0 -> 0xDB 0xDC      0xDB -> 0xDB 0xDD
Frame : FEND | (port<<4)|cmd | escaped payload | FEND
```

| cmd  | Name         | Wert      |
| ---- | ------------ | --------- |
| 0x00 | DATA         | AX.25     |
| 0x01 | TXDELAY      | 1 Byte    |
| 0x02 | PERSIST      | 1 Byte    |
| 0x03 | SLOTTIME     | 1 Byte    |
| 0x04 | TXTAIL       | 1 Byte    |
| 0x05 | **FULLDUPLEX** | 0/1     |
| 0x06 | SETHARDWARE  | —         |

CSMA-Vorgaben:

```
TXDELAY = 50
SLOTTIME = 10
PERSIST = 255   (CB - erzwungen, tiefer = Warnung)
PERSIST = 63    (Amateurfunk)
TXTAIL  = profilabhaengig
```

**FCS:** vor dem Senden wird der AX.25-FCS entfernt, **wenn** die CRC-16 stimmt
(reflektiertes Polynom `0x8408`, Init `0xFFFF`, Final-XOR `0xFFFF`).

### TNC2-Hostmode-Alternative (ohne KISS)

Reihenfolge, je `cmd + "\r"`:

```
                       (leere Zeile)
MYCALL <mycall>
TXDELAY <n>
Persist <n>
SlotTime <n>
TXTAIL <n>
FULLDUP ON|OFF
PACLEN 256
MAXFRAME 4
C <dest_call>
```

---

## 5. T-Modem — eigener Weg

Dual-Protokoll auf USB CDC:

- Byte **`0xC0`** → KISS-Frames
- **druckbare ASCII-Zeilen** → Service-Kommandos, terminiert mit `\r`/`\n`

```
PTT ON|OFF        (alias PTT 1|0)
STATUS            -> state, PTT, signal, packet, bitrate, raw CDT/RXD
HELP   ID   QUIET ON|OFF   VERBOSE ON|OFF
INI   GET <key>   SET <key>=<value>   SAVE   RELOAD   DEFAULTS
```

Antworten: `OK …` / `ERR …`. Boot-Banner läuft genau einmal.
Es gibt **keinen Baud-Wechsel** — die AFSK-Bitrate ist build-fest.

Geräteknoten: `/dev/ttyACM*` (Linux), `/dev/cuaU0` (FreeBSD).

---

## 6. Duplex

| Modus | Verhalten                                                          |
| ----- | ------------------------------------------------------------------ |
| half  | CSMA + Persist, explizites PTT, VOX aus. **CB-üblich.**            |
| full  | RX+TX gleichzeitig, kein CSMA, Echounterdrückung, getrennte Pfade. |

Hardware-Hook: KISS-Param `FULLDUPLEX` (0x05) bzw. Host `FULLDUP ON|OFF`.
Halbduplex-Warteschleife vor TX: `SLOTTIME * 10 ms` (min. 50 ms) nach letztem RX.
TX-Pacing: **min. 1,5 s** Sendepause zwischen RF-Rahmen.

> **PRTERM-Abweichung (bewusst, eigene Lösung):** die Referenzen erzwingen auf
> CB `duplex=half`. PRTERM bietet Vollduplex als **Betriebs**-Vollduplex:
> RX- und TX-Pfad sind entkoppelt, Empfang läuft während des Sendens weiter.
> Die **rechtliche** Prüfung (Bandplan, Zuteilung, Leistung) liegt davor und
> kann Senden ganz verhindern — siehe `docs/REGULATIONS.md`.

---

## 7. Geräteknoten & Plattform

| OS      | Klassische TNCs | T-Modem    |
| ------- | --------------- | ---------- |
| Linux   | `/dev/ttyUSB0`, `/dev/ttyS0`, `/dev/serial/by-id/…` | `/dev/ttyACM0` |
| FreeBSD | `/dev/cuaU0`    | `/dev/cuaU0` |

FreiBSD nutzt `cuaU*` (callout), nicht `ttyU*`.

**Startreihenfolge:** ein Prozess je `/dev/tty*`. Port zwischen Prep und Attach
nicht schliessen (DTR-Drop → Echo-only).

---

## 8. Baud-Finding (offline, kein Runtime-Handshake)

Es gibt **keine** Baudrate-Unterhandlung. Die Rate steht in der INI und wird
offline ermittelt — Profilsweep über

```
19200-7E1  19200-8N1  9600-7E1  9600-8N1  4800-7E1  2400-7E1  1200-7E1
```

mit Probes `\r`, `INFO\r`, `HELP\r`, `?\r` und Needle-Scoring
(`cmd:` +200, `MYCALL` +120, `TXDELAY` +80, `KISS` +40).

PRTERM stellt das als `prterm.cgi --probe-tnc` bereit.
