# PRTERM — Protokoll-Referenz

Abgeleitet aus dem Studienmaterial unter `../0-POOL/study/`.
Diese Datei ist die **geplante** Anbindung, keine Fiktion: die Kommandos
und Feldnamen stammen wörtlich aus den Referenzprojekten.

---

## 1. M25/1 — Wire-Protokoll der MAX25-Stack-TNCs

Quelle: `MAX25-Stack/include/max25/protocol.md`

Zeilenorientierter Text, ein Kommando je `\n`-Zeile.
**Keywords sind case-sensitive** (Ausnahme: `SET AX25_UI`-Flags).

### Kommandos (Host → TNC)

```
PING
GET STATUS
GET DEVICES
SET DEVICE <id>            (alias: SELECT DEVICE <id>)
SET CALLERID <id>          Quellrufzeichen, Grossbuchstaben
SET CALLID <id>            Zielrufzeichen
SET AX25_UI on|off
CONNECT
DISCONNECT
SEND <text>                Payload = Rest der Zeile
MONITOR on|off             rein empfangen; SEND -> "ERR monitor-only"
BAN <callsign>
UNBAN <callsign>
BANS
```

### Antworten (TNC → Host)

```
OK
ERR <msg>
STATUS hardware=… device=… devices=… mode=… callerid=… callid=… ax25_ui=…
       connected=… stack=… serial=… error=valid|invalid voice=valid|invalid
DEVICE id=… hardware=… serial=… stack=… enabled=… error=… voice=…
RX device=<id> <text>
EVENT connected|disconnected
```

TX-Echo (umrahmt):

```
[AX25 UI <callerid>><callid>] <payload>
```

### Handshake

```
<- OK
<- STATUS …
   (mit [network] tcp_password gesetzt:)
<- AUTH required
-> AUTH <password>          (Klartext; Unix-Socket überspringt Auth)
```

### PRTERM-Mapping

| PRTERM UI            | M25/1                                    |
| -------------------- | ---------------------------------------- |
| `CALLERID`-Feld      | `SET CALLERID <id>`                      |
| `CALLID`-Feld (Ziel) | `SET CALLID <id>`                        |
| `SEND`               | `SEND <text>`                            |
| Ban-Liste            | `BAN` / `UNBAN` / `BANS`                 |
| Duplex-Umschalter    | KISS-Param `FULLDUPLEX` (siehe §4)       |
| Statusanzeige        | `GET STATUS` / `STATUS …` parsen         |
| Gerätewahl           | `GET DEVICES` / `SET DEVICE <id>`        |

---

## 2. TNC-Host-Frames (T-Modem / Firmware-Ebene)

Quelle: `MAX25-Stack/stacks/tncs/docs/TNC-RECOVERY.md`,
`MAX25-Stack/stacks/tncs/test_tnc_serial_recovery.py`

| Zweck              | Frame                                     |
| ------------------ | ----------------------------------------- |
| KISS betreten      | `ESC @ K`  = `0x1B 0x40 0x4B`             |
| MYCALL setzen      | `ESC I <call>\r` = `0x1B 0x49 … 0x0D`     |

Referenz-Implementierung: `tf_mycall_frame("cb-0") == b"\x1bI CB-0\r"`

MYCALL ist **kein** Session-Kommando, sondern Firmware-/INI-Ebene:
`[transport.packet_radioN] mycall = CB-0`

---

## 3. KISS-Framing

| Symbol  | Byte   | Bedeutung                          |
| ------- | ------ | ---------------------------------- |
| `FEND`  | `0xC0` | Rahmenanfang / -ende               |
| `FESC`  | `0xDB` | Escape                             |
| `TFEND` | `0xDC` | `FEND` nach Escape                 |
| `TFESC` | `0xDD` | `FESC` nach Escape                 |

Port-Byte (nach `FEND`): Command im Nibble high, Port im Nibble low.

KISS-Parameter (`HYBBX_KISS_CMD_*`, Quelle `hyBBX/plugins/packet_radio/tnc.c`):

```
TXDELAY  PERSIST  SLOTTIME  TXTAIL  FULLDUPLEX  SETREADY  SECONDS
```

`FULLDUPLEX` ist **hardwareseitig** setzbar — das ist der Hebel für PRTERMs
Vollduplex-Modus, unabhängig davon ob die Funkstrecke es hergibt.

---

## 4. Vollduplex vs. Halbduplex

Quelle: `MAX25-Stack/stacks/crdop/ROADMAP.md`,
`hybbx-tnc2c.ini`

| Modus       | Verhalten                                                            |
| ----------- | -------------------------------------------------------------------- |
| `half`      | CSMA + PERSIST, explizites PTT, VOX aus. **CB-Default.** `EXTRADELAY 150` |
| `full`      | RX + TX gleichzeitig, **kein CSMA**, Echounterdrückung / getrennte Pfade, `EXTRADELAY 0` |

`radio_duplex = half|full` in der INI.

**PRTERM-Semantik:**

- `half` → während `ptt=1` wird RX geschaltet/gedrosselt (`rx_muted=1`).
- `full` → RX läuft bei `ptt=1` **ungehindert weiter** und wird weiter
  protokolliert. Genau das ist der Punkt, der mit klassischem CB-Funk nicht geht.

---

## 5. Rufzeichen-Regeln

Quelle: `MAX25-Stack/include/max25/protocol.md`,
`hyBBX/include/hybbx/ax25.h`

AX.25 allgemein:

```
Call body : 1..6 Zeichen  A-Z 0-9
SSID      : optional -0 .. -15
```

Konstanten (`hybbx/ax25.h`):

```
HYBBX_AX25_CALL_MAX     6
HYBBX_AX25_SSID_MAX     15
HYBBX_AX25_ADDR_ENCODED 7
HYBBX_AX25_MAX_DIGI     8
HYBBX_AX25_FRAME_MAX  330
PID 0xF0   UI control 0x03
```

### PRTERM ist bewusst strenger

```
CALLID    : Basis  <= 6 Zeichen, kein Suffix
CALLERID  : Basis  <= 6 Zeichen + optional "-<Ziffer>"
            Gesamt <= 8 Zeichen  ("6 + 2", also 6 Basis + "-" + 1 Ziffer)
```

Konfigurierbar über `[callsign]`:

```ini
callid_max_len       = 6
callerid_base_len    = 6
callerid_max_total   = 8
callerid_allow_ssid  = true
callerid_ssid_digits = 1      # 1 -> -0..-9 ; 2 -> -0..-15 (AX.25-konform)
```

> Wer AX.25-konform bis `-15` braucht, setzt `callerid_ssid_digits = 2`
> und `callerid_max_total = 9`. Der Default respektiert die Vorgabe 6+2 = 8.

### AX.25-Wire-Encoding (für spätere TNC-Anbindung)

```python
call.upper().ljust(6)[:6]        # 6 Byte, mit Blanks aufgefüllt
jedes Byte  << 1                 # Shifted ASCII
SSID-Byte: ((ssid & 0x0F) << 1) | 0x60   # letztes Adressbyte: 0x61
```

---

## 6. Bannen / Filtern

Quelle: `MAX25-Stack/include/max25/protocol.md`, `hyBBX/docs/SECURITY.md`

| Mechanismus            | Wirkung                                              |
| ---------------------- | ---------------------------------------------------- |
| `BAN <callsign>`       | blockt AX.25-Quelle, eingehende UI-Frames **still verworfen** |
| `ban_callid=` (hyBBX)  | sofort, permanent                                    |
| IP-Bans (hyBBX)        | `login_fail` 5 / 10 min, `link_auth_fail` 5 / 10 min, `rate_limit` 30 / 60 s |

Es gibt **keine** Nutzer-Ignore-Liste — Moderation = Bans + Rechtestufen.

PRTERM bildet das ab: `[ban]` mit Mustern (`*`, `?`) + Grund,
plus Überwachung von Login-Fehlversuchen pro IP.

---

## 7. Terminal-UI — Felder & Layout

Quelle: `MAX25-Stack/stacks/web/share/reverse-proxy/docroot/max25-websocket/index.php`

```
#topbar     Marke · Menü · DEVICE <select> · CALLERID · CALLID
            ax25-ui · connected · #conn-status
#term       <pre> RX-Log, white-space: pre-wrap, Auto-Scroll, Ringpuffer
#input-bar  #cmd  — ein Textfeld, enterkeyhint="send"
.hint       Hinweiszeile
```

Theme: dunkel — `#0a0a12` Hintergrund, `#c8d0d8` Text, `ui-monospace`.

**In den Referenz-UIs fehlen** S-Meter, Frequenz, Modus, Kanal.
PRTERM führt sie ein; die Feldnamen kommen von der T-Modem-`STATUS`-Antwort:

```
state   PTT   signal   packet   bitrate   raw CDT/RXD
```

---

## 8. Grenzwerte

Quelle: `hyBBX/include/hybbx/limits.h`

```
On-Air-Nachricht      max. 48 Byte
Auto-Beacon-Abstand   min. 900 s
Band muss frei sein   min. 180 s
HYBBX_SECURITY_BAN_MAX 256
```

---

## 9. Sonstiges

- `frequency_mhz = 27.235`, `radio_baud`, `radio_band = cb`, `radio_duplex`
- Broadcast ist **sequenziell**, ~60 s Abstand zwischen Links
- Kernel-`baycom_ser_fdx` ist aus den Referenzbäumen **entfernt** (Friert Hosts ein,
  siehe `docs/BAYCOM-FREEZES.md`) — Baycom wird von PRTERM nicht unterstützt
- Referenzen nutzen **WebSocket + Reverse-Proxy**; PRTERM nutzt bewusst
  **Standard-CGI mit Polling**, weil "keine Installation" Vorrang hat
