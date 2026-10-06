# PRTERM — Multiple Stations

> **Requirement:** *"Several must work simultaneously on the
> same channel."*
>
> **Clarification:** *"Both have their own rig and their own antenna."*

---

## 1. The model

This is **not** two TNCs on one rig, but **two
complete stations**:

```
Station A:  TNC2C   ──►  Rig A  ──►  Antenna A
Station B:  PK-TNC2 ──►  Rig B  ──►  Antenna B
                                │
                          both on channel 24 (27.235 MHz)
```

Each station is **self-contained**: its own TNC, its own rig,
its own antenna, its own serial port, its own identity.

---

## 2. Why different radio baud rates are no problem

| Station  | Radio baud rate | Capability |
| -------- | --------------- | ---------- |
| TNC2C    | **2400**        | hard-wired (modem TCM3105) |
| PK-TNC2  | **1200**        | hard-wired |

> *"Which can be switched neither up nor down."*

The radio baud rate is **hardware**, not a setting. PRTERM may
know it, document it and display it — but **never try to change it**.

The fact that the two cannot understand each other on the same channel is
therefore **expected and intended**: they are independent stations.

---

## 3. Configuration

```ini
[radio]
freq_hz   = 27235000     ; channel 24 - shared frequency
mode      = fm

[station:tnc2c]
driver     = tnc2
port       = /dev/serial/by-id/usb-FTDI_USB_Serial_Converter_FTC7OKUL-if00-port0
baud       = 19200        ; serial to the TNC
radio_baud = 2400         ; FIXED - hardware, not changeable
modem      = tcm3105
line       = 8n1
callerid   = DL1ABC-1
antenne    = Vertikal

[station:pktn2c]
driver     = tnc2
port       = /dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller-if00-port0
baud       = 9600         ; serial to the TNC
radio_baud = 1200         ; FIXED - hardware, not changeable
line       = 8n1
callerid   = DL1ABC-2
antenne    = Richtantenne
```

`radio_baud` is deliberately **not** sent to the rig — it is a
property, not a command.

---

## 4. Transmit arbitration

Even with separate rigs the rule holds: **on the same frequency only one
station transmits at a time.** Otherwise the signals interfere over the air,
regardless of the antenna.

The arbitration lives in `src/arbiter.c`:

```
transmit request ──► lock per frequency ──► transmit ──► release
```

* one lock **per frequency** — stations on different channels
  do not block each other
* via `fcntl(F_SETLK)` — effective across multiple CGI processes
* drops automatically when a process ends — no stuck channel
* on rejection, shows **who** is transmitting at the time

Reception is uncritical: both stations listen simultaneously and write into
one shared log.

---

## 5. Status

|                                          |                                     |
| ---------------------------------------- | ----------------------------------- |
| Configuring multiple stations            | in progress                         |
| Transmit arbitration (arbiter)           | **built** (`src/arbiter.c`)         |
| Shared log                               | foundation in place                 |
| Own identity per station                 | prepared                            |
| Showing both in the web interface        | open                                |
| Radio baud rate as hardware property     | **built**                           |

---

## 6. Why it is this way and no other

The two stations could be completely separate programs. PRTERM
brings them together anyway, because:

* there shall be **one** place for the receive log and operation
* the **transmit arbitration** only works if it sees all stations
* the **compliance** (band plan, power) must apply equally to all
