# PRTERM — TNC Initialization

Derived from the study material (`../0-POOL/study/`).
Only what is **binding** has been adopted — the procedure is PRTERM's own.

---

## 1. Serial base settings (all TNCs)

```
open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK)
cfmakeraw()                      # PRTERM: own replacement, cfmakeraw is not POSIX
c_cflag |= CLOCAL | CREAD
c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB)
c_cflag |= CS7|CS8  [+ PARENB|PARODD]  [+ CSTOPB]   # per profile
c_cc[VMIN] = 0
c_cc[VTIME] = 5
cfsetispeed/cfsetospeed()        # see portability
tcsetattr(TCSANOW)
tcflush(TCIOFLUSH)
TIOCMSET: TIOCM_RTS | TIOCM_DTR high    # only if the profile requires it
Settle 2 s                           # important: do NOT close the port!
```

- **No hardware flow control** — `CRTSCTS` is never set, `IXON/IXOFF` off.
- After every write: `tcdrain()`.
- The port stays **open** between boot wait and stack attach —
  a DTR drop puts the TNC into echo-only mode.

> **Portability:** `cfmakeraw()` is BSD/GNU, not POSIX. PRTERM sets the
> fields by hand. `cfsetspeed()` is Linux; otherwise `cfsetispeed`+`cfsetospeed`.
> `TIOCMGET/TIOCMSET` only under `#ifdef TIOCMGET`, and switchable, because
> `TIOCMGET` often fails on PTYs.

---

## 2. Profiles

| Profile  | Rig                    | Line    | Baud   | RTS/DTR | KISS entry     |
| -------- | ---------------------- | ------- | ------ | ------- | -------------- |
| `tnc2c`  | Landolt TNC2C          | **7E1** | 19200  | on      | `ESC @K`       |
| `pktnc2` | PK-TNC2                | **8N1** | 9600   | off     | `kiss on\r`    |
| `tmodem` | T-Modem (half-TNC)     | 8N1     | 115200 | —       | native KISS    |
| `generic`| classic TNC2 clones    | 8N1     | 2400   | off     | `kiss on\r`    |

**Caution — field vs. manual:** the TNC2C manual requires 7E1, in practice
the Landolt TNC2C runs at **19200 8N1**. PRTERM makes the profile configurable
and warns when opening with a deviating line format.

> Baycom/PC-COM/SER12 are **not** supported by PRTERM: these boards
> are bare modems, the host does HDLC + bit timing directly through the UART-
> registers. That is its own device class and not a TNC.

---

## 3. Frames (byte-exact)

### Constants

| Purpose                  | Bytes                                       |
| ------------------------ | ------------------------------------------- |
| Version/probe            | `1B 56 0D`                                  |
| Cold start from EPROM    | `1B 51 52 45 53 0D`  (`ESC QRES`)           |
| **Enter KISS**           | `1B 40 4B`  (`ESC @K`, **without** `\r`)    |
| Echo off                 | `1B 45 30 0D`                               |
| **Leave KISS**           | `C0 FF C0`                                  |
| Set MYCALL               | `1B 49 20 <CALL> 0D`                        |
| Flush buffer             | `11 18`  (`^Q^X`)                           |
| Leave host mode          | `00 ×300` + `00 01 06 'JHOST 0' 0D`         |

`tf_mycall_frame("cb-0") == b"\x1bI CB-0\r"`  ← **with** a space after `I`

### Sequence (composite recipe)

1. Open the port, configure it, RTS/DTR high, **wait 2 s**.
2. Listen passively for a banner for 1.5 s.
3. Only in echo-only: recovery ladder
   `C0 FF C0` → `11 18` + 300×`00` + `JHOST 0` → `ESC V` → `ESC QRES` →
   `ESC E0` → `ESC V` → second `ESC QRES` → `kiss off\r` + `INFO\r`.
4. `1B 49 20 <CALL> 0D`  (MYCALL) — error: `?` in the first 32 bytes.
5. `1B 40 4B`  (KISS on) — TAPR class: `kiss on\r`.
6. Send KISS parameters (§4).
7. Operation: `C0 00 <escaped payload> C0`.
8. Shut down: `C0 FF C0` — **leave the port open** (holds DTR).

---

## 4. KISS framing & parameters

```
FEND  = 0xC0      TFEND = 0xDC
FESC  = 0xDB      TFESC = 0xDD

Escape: 0xC0 -> 0xDB 0xDC      0xDB -> 0xDB 0xDD
Frame : FEND | (port<<4)|cmd | escaped payload | FEND
```

| cmd  | Name           | Value     |
| ---- | -------------- | --------- |
| 0x00 | DATA           | AX.25     |
| 0x01 | TXDELAY        | 1 byte    |
| 0x02 | PERSIST        | 1 byte    |
| 0x03 | SLOTTIME       | 1 byte    |
| 0x04 | TXTAIL         | 1 byte    |
| 0x05 | **FULLDUPLEX** | 0/1       |
| 0x06 | SETHARDWARE    | —         |

CSMA defaults:

```
TXDELAY = 50
SLOTTIME = 10
PERSIST = 255   (CB - enforced, lower = warning)
PERSIST = 63    (amateur radio)
TXTAIL  = profile-dependent
```

**FCS:** before sending, the AX.25 FCS is removed, **if** the CRC-16 is correct
(reflected polynomial `0x8408`, init `0xFFFF`, final XOR `0xFFFF`).

### TNC2 host mode alternative (without KISS)

Sequence, each `cmd + "\r"`:

```
                       (empty line)
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

## 5. T-Modem — its own way

Dual protocol on USB CDC:

- Byte **`0xC0`** → KISS frames
- **printable ASCII lines** → service commands, terminated with `\r`/`\n`

```
PTT ON|OFF        (alias PTT 1|0)
STATUS            -> state, PTT, signal, packet, bitrate, raw CDT/RXD
HELP   ID   QUIET ON|OFF   VERBOSE ON|OFF
INI   GET <key>   SET <key>=<value>   SAVE   RELOAD   DEFAULTS
```

Responses: `OK …` / `ERR …`. The boot banner runs exactly once.
There is **no baud change** — the AFSK bit rate is fixed at build time.

Device nodes: `/dev/ttyACM*` (Linux), `/dev/cuaU0` (FreeBSD).

---

## 6. Duplex

| Mode | Behaviour                                                         |
| ----- | ----------------------------------------------------------------- |
| half  | CSMA + PERSIST, explicit PTT, VOX off. **usual for CB.**          |
| full  | RX+TX simultaneously, no CSMA, echo suppression, separate paths.  |

Hardware hook: KISS param `FULLDUPLEX` (0x05) or host `FULLDUP ON|OFF`.
Half-duplex wait loop before TX: `SLOTTIME * 10 ms` (min. 50 ms) after the last RX.
TX pacing: **min. 1.5 s** transmit gap between RF frames.

> **PRTERM deviation (deliberate, own solution):** the references enforce on
> CB `duplex=half`. PRTERM offers full duplex as **operational** full duplex:
> RX and TX path are decoupled, reception keeps running while transmitting.
> The **legal** check (band plan, allocation, power) sits in front of it and
> can prevent transmitting altogether — see `docs/REGULATIONS.md`.

---

## 7. Device nodes & platform

| OS      | Classic TNCs | T-Modem    |
| ------- | ------------ | ---------- |
| Linux   | `/dev/ttyUSB0`, `/dev/ttyS0`, `/dev/serial/by-id/…` | `/dev/ttyACM0` |
| FreeBSD | `/dev/cuaU0`    | `/dev/cuaU0` |

FreeBSD uses `cuaU*` (callout), not `ttyU*`.

**Startup sequence:** one process per `/dev/tty*`. Do not close the port
between prep and attach (DTR drop → echo-only).

---

## 8. Baud detection (offline, no runtime handshake)

There is **no** baud rate negotiation. The rate is in the INI and is
determined offline — profile sweep over

```
19200-7E1  19200-8N1  9600-7E1  9600-8N1  4800-7E1  2400-7E1  1200-7E1
```

with probes `\r`, `INFO\r`, `HELP\r`, `?\r` and needle scoring
(`cmd:` +200, `MYCALL` +120, `TXDELAY` +80, `KISS` +40).

PRTERM provides this as `prterm.cgi --probe-tnc`.
