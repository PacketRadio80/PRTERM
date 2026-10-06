# PRTERM — Protocol Reference

Derived from the study material under `../0-POOL/study/`.
This file is the **planned** hookup, not fiction: the commands
and field names come verbatim from the reference projects.

---

## 1. M25/1 — wire protocol

Our own line protocol, kept in full. M25/1 is ours — the name, the commands and
the code — so it may be adopted as a product component wherever that is sensible.
If it ever is, it gets **adapted thoroughly**, never dropped in as-is. It is
documented here because it is a wire format, and a wire format is a binding
whatever happens above it.

Line-oriented text, one command per `\n` line.
**Keywords are case-sensitive** (exception: `SET AX25_UI` flags).

### Commands (host → TNC)

```
PING
GET STATUS
GET DEVICES
SET DEVICE <id>            (alias: SELECT DEVICE <id>)
SET CALLERID <id>          source call sign, uppercase
SET CALLID <id>            destination call sign
SET AX25_UI on|off
CONNECT
DISCONNECT
SEND <text>                payload = rest of the line
MONITOR on|off             receive-only; SEND -> "ERR monitor-only"
BAN <callsign>
UNBAN <callsign>
BANS
```

### Responses (TNC → host)

```
OK
ERR <msg>
STATUS hardware=… device=… devices=… mode=… callerid=… callid=… ax25_ui=…
       connected=… stack=… serial=… error=valid|invalid voice=valid|invalid
DEVICE id=… hardware=… serial=… stack=… enabled=… error=… voice=…
RX device=<id> <text>
EVENT connected|disconnected
```

TX echo (framed):

```
[AX25 UI <callerid>><callid>] <payload>
```

### Handshake

```
<- OK
<- STATUS …
   (with [network] tcp_password set:)
<- AUTH required
-> AUTH <password>          (plaintext; Unix socket skips auth)
```

### PRTERM mapping

| PRTERM UI            | M25/1                                    |
| -------------------- | ---------------------------------------- |
| `CALLERID` field     | `SET CALLERID <id>`                      |
| `CALLID` field (dest)| `SET CALLID <id>`                        |
| `SEND`               | `SEND <text>`                            |
| ban list             | `BAN` / `UNBAN` / `BANS`                 |
| duplex switch        | KISS param `FULLDUPLEX` (see §4)         |
| status display       | parse `GET STATUS` / `STATUS …`          |
| rig selection        | `GET DEVICES` / `SET DEVICE <id>`        |

---

## 2. TNC host frames (T-Modem / firmware level)

| Purpose            | Frame                                     |
| ------------------ | ----------------------------------------- |
| enter KISS         | `ESC @ K`  = `0x1B 0x40 0x4B`             |
| set MYCALL         | `ESC I <call>\r` = `0x1B 0x49 … 0x0D`     |

Reference implementation: `tf_mycall_frame("cb-0") == b"\x1bI CB-0\r"`

MYCALL is **not** a session command, but firmware/INI level:
`[transport.packet_radioN] mycall = CB-0`

---

## 3. KISS framing

| Symbol  | Byte   | Meaning                            |
| ------- | ------ | ---------------------------------- |
| `FEND`  | `0xC0` | frame start / end                  |
| `FESC`  | `0xDB` | escape                             |
| `TFEND` | `0xDC` | `FEND` escaped                     |
| `TFESC` | `0xDD` | `FESC` escaped                     |

Port byte (after `FEND`): command in the high nibble, port in the low nibble.

KISS parameters:

```
TXDELAY  PERSIST  SLOTTIME  TXTAIL  FULLDUPLEX  SETREADY  SECONDS
```

`FULLDUPLEX` is settable **in hardware** — that is the lever for PRTERM's
full-duplex mode, regardless of whether the radio link allows it.

---

## 4. Full duplex vs. half duplex

| Mode       | Behaviour                                                            |
| ----------- | -------------------------------------------------------------------- |
| `half`      | CSMA + PERSIST, explicit PTT, VOX off. **CB default.** `EXTRADELAY 150` |
| `full`      | RX + TX simultaneously, **no CSMA**, echo suppression / separate paths, `EXTRADELAY 0` |

`radio_duplex = half|full` in the INI.

**PRTERM semantics:**

- `half` → during `ptt=1`, RX is switched/throttled (`rx_muted=1`).
- `full` → at `ptt=1`, RX keeps running **unimpeded** and keeps being
  logged. That is exactly the point that classic CB radio cannot do.

---

## 5. Call sign rules

AX.25 in general:

```
Call body : 1..6 characters  A-Z 0-9
SSID      : optional -0 .. -15
```

AX.25 limits:

```
call body      1..6 characters
SSID           0..15
address        7 bytes encoded
digipeaters    max 8
frame          max 330 bytes
PID 0xF0   UI control 0x03
```

### PRTERM is deliberately stricter

```
CALLID    : base   <= 6 characters, no suffix
CALLERID  : base   <= 6 characters + optional "-<digit>"
            total  <= 8 characters  ("6 + 2", i.e. 6 base + "-" + 1 digit)
```

Configurable via `[callsign]`:

```ini
callid_max_len       = 6
callerid_base_len    = 6
callerid_max_total   = 8
callerid_allow_ssid  = true
callerid_ssid_digits = 1      # 1 -> -0..-9 ; 2 -> -0..-15 (AX.25-conform)
```

> Whoever needs AX.25 conformity up to `-15` sets `callerid_ssid_digits = 2`
> and `callerid_max_total = 9`. The default respects the requirement 6+2 = 8.

### AX.25 wire encoding (for later TNC hookup)

```python
call.upper().ljust(6)[:6]        # 6 byte, padded with blanks
each byte   << 1                 # Shifted ASCII
SSID byte: ((ssid & 0x0F) << 1) | 0x60   # last address byte: 0x61
```

---

## 6. Banning / filtering

| Mechanism            | Effect                                             |
| ---------------------- | -------------------------------------------------- |
| `BAN <callsign>`       | blocks AX.25 source, incoming UI frames **silently discarded** |
| `ban_callid=`          | immediate, permanent                               |
| IP bans                | `login_fail` 5 / 10 min, `link_auth_fail` 5 / 10 min, `rate_limit` 30 / 60 s |

There is **no** user ignore list — moderation = bans + permission levels.

PRTERM maps this: `[ban]` with patterns (`*`, `?`) + reason,
plus monitoring of failed logins per IP.

---

## 7. Terminal UI — fields & layout

```
#topbar     brand · menu · DEVICE <select> · CALLERID · CALLID
            ax25-ui · connected · #conn-status
#term       <pre> RX log, white-space: pre-wrap, auto-scroll, ring buffer
#input-bar  #cmd  — one text field, enterkeyhint="send"
.hint       hint line
```

Theme: dark — `#0a0a12` background, `#c8d0d8` text, `ui-monospace`.

**Missing in the reference UIs:** S-meter, frequency, mode, channel.
PRTERM introduces them; the field names come from the T-Modem `STATUS` response:

```
state   PTT   signal   packet   bitrate   raw CDT/RXD
```

---

## 8. Limits

```
On-air message max. 48 byte
Auto-beacon interval   min. 900 s
Band must be free      min. 180 s
Ban list               max 256 entries
```

---

## 9. Miscellaneous

- `frequency_mhz = 27.235`, `radio_baud`, `radio_band = cb`, `radio_duplex`
- Broadcast is **sequential**, ~60 s between links
- The in-kernel `baycom_ser_fdx` path is **not used** — it freezes interactive
  hosts, so Baycom stays a userspace concern. Baycom is on the PRTERM roadmap,
  to be studied and built in later.
- The references use **WebSocket + reverse proxy**; PRTERM deliberately uses
  **standard CGI with polling**, because "no installation" takes priority
