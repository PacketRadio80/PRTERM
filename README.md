# PRTERM

HTTPd terminal for amateur/citizen packet radio — plugin-able, with an optional
Mailbox daemon and more.

PRTERM is written in C. It is a CGI program and runs behind a local webserver
with a proper GUI. You can communicate through LAN/Internet and over the air via
packet radio hardware and software, on the amateur and citizen bands. It is
extensible through plugins for more features, and it is usable remotely from a
browser.

> **Status: early development (0.1.0).**

---

## Features

| | |
|---|---|
| **Full duplex** | RX and TX are decoupled — reception keeps running while you transmit |
| **Band plan** | BNetzA allocation for CB Germany, 80 channels, channel twist included |
| **Compliance** | a gate in front of the transmit path: wrong frequency, mode or power never reaches the air |
| **CALLID / CALLERID** | the `6+2` rule, stricter than plain AX.25 |
| **Ban filter** | block by pattern (`*`, `?`) with a reason |
| **Administration** | clickable inside the terminal, **no separate URL** |
| **Plugins** | transports and features plug in; the mailbox daemon is one of them |
| **Fonts** | your own `.otf`/`.ttf`, size taken from the INI |
| **Languages** | the interface speaks the big five — English, Deutsch, Español, Português, Français — switchable in the administration |
| **Character grid** | as many characters as font size and screen resolution give you — measured, not guessed |
| **Multiple stations** | several complete stations on one channel, with transmit arbitration |
| **Platforms** | Linux + FreeBSD on x86-64 and arm64, portable to other systems |

---

## Quick start

### Build

```sh
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

Produces four binaries in `build/`:

| Binary | Purpose |
|---|---|
| `prterm.cgi` | the terminal — CGI and command line in one |
| `prterm-tncd` | device daemon — keeps serial ports open and the TNC in KISS |
| `prterm-ini` | edit `prterm.ini` from scripts, comment- and order-preserving |
| `prterm-probe` | detect a TNC and look at it (profile sweep, raw dump) |

### Configure

```sh
cp build/prterm.cgi /path/to/cgi-bin/
cp prterm.ini       /path/to/cgi-bin/
```

Create a password hash and put it in `[admin] pass_hash`:

```sh
./prterm.cgi --hash-password "myPassword"
```

With an empty `pass_hash` **no login is possible** — that is the safe state.

### Web server

PRTERM is a standard RFC 3875 CGI binary. Anything that can run a CGI
serves it unchanged. Ready-made configs for the four we test live:

- [`docs/lighttpd.conf`](docs/lighttpd.conf) + [`docs/lighttpd-systemd.conf`](docs/lighttpd-systemd.conf) — the reference deploy (this host).
- [`docs/apache.conf`](docs/apache.conf) — Apache 2.4 with `mod_cgi` + `mod_alias`.
- [`docs/nginx.conf`](docs/nginx.conf) — NGinx bridged through `fcgiwrap`.
- [`docs/busybox-httpd.conf`](docs/busybox-httpd.conf) — `CONFIG_HTTPD_CGI=y` build, dev only.

The compatibility overview, decision table, and per-server caveats are in
[`docs/WEB-SERVERS.md`](docs/WEB-SERVERS.md). For the smallest possible snippet:

```apache
# Apache
ScriptAlias /prterm /usr/lib/cgi-bin/prterm/prterm.cgi
```

```nginx
# nginx + fcgiwrap
location = /prterm.cgi {
    include fastcgi_params;
    fastcgi_pass unix:/run/fcgiwrap.sock;
    fastcgi_param SCRIPT_FILENAME /usr/lib/cgi-bin/prterm/prterm.cgi;
}
```

```sh
# busybox httpd (for trying it out)
busybox httpd -p 8080 -h /usr/lib/cgi-bin/prterm -u lighttpd
```

No redirects, no WebSocket of its own to configure, no reverse proxy. PRTERM
is one HTML document; links are formed relative to `SCRIPT_NAME`, so the
script follows whatever path the server gives it.

---

## The one URL

```
/prterm.cgi
```

No sub-paths, no `/admin`, no second address. Everything hangs off query
parameters and form fields of that one URL. Links are built relative
(`<form action="">`), so the script works under any path the server gives it.

| Request | Answer |
|---|---|
| `GET  /prterm.cgi` | HTML5 — the whole application |
| `GET  /prterm.cgi?action=state` | JSON — rig state |
| `GET  /prterm.cgi?action=log` | JSON — messages |
| `GET  /prterm.cgi?action=font` | the configured font file (binary) |
| `POST /prterm.cgi` `action=…` | perform an action, then redirect or JSON |

Operation actions: `tx`, `ptt`, `monitor`, `set`.
Administration actions: `login`, `logout`, `save_site`, `save_station`,
`save_radio`, `save_callsign`, `save_ui`, `ban_add`, `ban_del`, `pass_change`,
`config_save`. Every action carries a `csrf` token.

The application is **one** HTML document with several views that switch on a
click. There is no state in the URL — reload the page and you are back in the
terminal. Authentication hangs on a cookie (`HttpOnly`, `SameSite=Strict`), not
on the address.

The font route serves **only** the file named in `[ui] font_file`. There is no
generic file download, otherwise that route becomes a read oracle for the whole
filesystem.

---

## Command line

The binary is useful without a web server too:

```sh
./prterm.cgi --help
./prterm.cgi --check-ini prterm.ini       # validate the configuration
./prterm.cgi --print-config prterm.ini    # show the effective configuration
./prterm.cgi --gen-ini prterm.ini         # write a commented example
./prterm.cgi --channels                   # print the CB Germany channel table
./prterm.cgi --hash-password "pw"         # hash for [admin] pass_hash
./prterm.cgi --add-ban 'DL9*' Spam
./prterm.cgi --selftest [INI] [STATION]   # check device state
./prterm.cgi --checkup [INI] [STATION]    # make sure KISS is active, clear buffers
./prterm.cgi --reset-tnc [INI] [STATION]  # emergency reset of the TNC
```

```sh
prterm-ini list [SECTION]
prterm-ini get  SECTION KEY
prterm-ini set  SECTION KEY VALUE
prterm-ini del  SECTION KEY
```

Both write `prterm.ini` back **comment- and order-preserving** — the file stays
readable.

```sh
prterm-probe /dev/ttyUSB0                        # profile sweep
prterm-probe --all                               # ask every node
prterm-probe --dump /dev/ttyUSB0 19200 8N1       # listen and log
prterm-probe --raw  /dev/ttyUSB0 19200 8N1
```

`prterm-probe` only ever sends serial commands — never a radio transmission.

---

## Configuration

`prterm.ini`, in the same folder as the binary. It is fully commented; the file
itself is the reference. Sections:

| Section | Contents |
|---|---|
| `[site]` | name, subtitle, language |
| `[station]` | CALLERID, QTH, locator |
| `[station:<name>]` | additional stations — see [Multiple stations](#multiple-stations) |
| `[ui]` | font file and size, line height, density, grid, theme |
| `[radio]` | duplex, driver, port, baud, `radio_baud`, modem, line format, frequency, mode |
| `[admin]` | enable/disable, `pass_hash`, session lifetime |
| `[callsign]` | CALLID/CALLERID rules (`6+2`) |
| `[ban]` | blocked call patterns with a reason |
| `[debug]` | trace level (`off`…`trace`) for daemon and driver — `trace` hexdumps every serial byte into the journal / error log |
| `[paths]` | runtime directory |

A typical `[radio]` block:

```ini
[radio]
duplex     = full              ; full | half
driver     = tnc2              ; sim | tnc2
port       = /dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller-if00-port0
baud       = 19200             ; serial rate TO the TNC
radio_baud = 2400              ; rate ON THE CHANNEL — hardware, never changed
line       = 8n1
freq_hz    = 27235000
mode       = am
```

`baud` and `radio_baud` are deliberately separate: the first is the serial link,
the second is the modem in the TNC. `radio_baud` is a property of the hardware,
never a command — PRTERM knows it, shows it, and never tries to change it.

---

## Band plan

CB Germany, 80 channels after the BNetzA general allocation
(Vfg. Nr. 21/2021):

```
Channel  1–40 : 26.965 – 27.405 MHz   (CEPT, harmonised across Europe)
Channel 41–80 : 26.565 – 26.955 MHz   (national extension)
```

| Mode | Power | Channels |
|---|---|---|
| FM / PM | 4 W ERP | 1–80 |
| **AM** | 4 W ERP | **1–40** |
| SSB | 12 W PEP | 1–40 |

Details the table gets right:

- **Channel twist at 23** — 22 = 27.225 → **23 = 27.255** → 24 = 27.235 → 25 = 27.245
- **Data channels**: 6, 7, 24, 25, 52, 53, 76, 77
- **Gateway channels**: 11, 29, 34, 39, 40, 41, 61, 71, 80

Print it yourself with `./prterm.cgi --channels`.

---

## Hardware

| Driver | Devices | Protocol | Status |
|---|---|---|---|
| `sim` | — | simulation | works |
| `tnc2` | Landolt TNC2C, PK-TNC2 (TNC2 clones) | KISS + ESC host mode | in use |
| `tmodem` | T-Modem (half-TNC) | ESC host frames | planned |

Connected via a serial port or a USB-SERIAL converter.

| OS | Classic TNCs | T-Modem |
|---|---|---|
| Linux | `/dev/ttyUSB0`, `/dev/ttyS0`, `/dev/serial/by-id/…` | `/dev/ttyACM0` |
| FreeBSD | `/dev/cuaU0` | `/dev/cuaU0` |

**Not supported today:** Baycom and ARDOP. They are on the roadmap, on two
different paths:

- **Baycom** — will be studied and built into PRTERM later.
- **ARDOP** — later still, and only as an **external plugin that talks to ARDOP
  software**. No ARDOP code goes inside PRTERM; it is an adapter, not an
  integration.

Out of scope for good: sound-card modems such as VARA. Those boards are bare
modems — the host would have to do HDLC and bit timing straight into the UART
registers. That is a different class of device, not a TNC.

### The device daemon

A CGI process dies after every request. When the last file descriptor goes,
USB serial adapters reset and the TNC drops out of KISS — which is exactly why
sending used to break follow-up transmissions.

`prterm-tncd` fixes that: it opens each configured port once, enters KISS once,
and keeps both alive. One Unix socket per station:

```
<runtime_dir>/tnc-<station>.sock
```

It stays in the foreground; for permanent operation systemd starts it
(`docs/lighttpd-systemd.conf` shows the pattern).

### Multiple stations

Not two TNCs on one radio — **two complete stations**, each with its own TNC,
radio, antenna, serial port and identity, sharing one channel:

```
Station A:  TNC2C   ──►  Radio A  ──►  Antenna A
Station B:  PK-TNC2 ──►  Radio B  ──►  Antenna B
                                │
                        both on channel 24 (27.235 MHz)
```

Because they run at different fixed radio baud rates (2400 vs. 1200) they cannot
understand each other — expected and intended, they are independent stations.

Even so, **only one station transmits at a time on a given frequency**. The
arbitration lives in `src/arbiter.c`: one lock per frequency, taken with
`fcntl(F_SETLK)` so it works across CGI processes, and it drops automatically
when a process dies — no stuck channel. Reception is uncritical, both stations
listen and write into one shared log.

---

## Platforms and portability

Primary targets:

| OS | Architectures |
|---|---|
| Linux | x86-64, arm64 |
| FreeBSD | x86-64, arm64 |

That is what PRTERM is built and tested on. Other systems and distributions
should be reachable **without surgery** — a port is meant to be an afternoon's
work, not a fork.

### Why that holds

Strict C11 + POSIX.1-2008/XSI. No GNU extensions, no `_GNU_SOURCE`. The
feature-test macros live in one header, `src/prterm_compat.h`, which is included
first in every translation unit. The only operating-system `#ifdef` in the whole
project is the one that switches on `_DEFAULT_SOURCE` for glibc.

What is deliberately not used, and what stands in for it:

| Not used | Used instead | Because |
|---|---|---|
| `epoll`, `kqueue`, `timerfd` | `poll()` | POSIX |
| `flock()` | `fcntl(F_SETLK)` | BSD, not POSIX |
| `cfmakeraw()` | termios fields set by hand | BSD/GNU, not POSIX |
| `cfsetspeed()` | `cfsetispeed()` + `cfsetospeed()` | Linux-specific |
| `getrandom()` | `/dev/urandom` | Linux-specific |
| `/proc/self/exe` | `argv[0]` | `/proc` is not portable |

Everything genuinely platform-dependent is behind a **feature test** instead of
a platform name: `TIOCMBIS`/`TIOCMSET`/`TIOCMGET` for modem lines, `B57600` …
`B921600` for uncommon baud rates, `CRTSCTS` for hardware flow control. What a
platform does not have simply compiles out.

### What a port involves

1. **Build** — CMake ≥ 3.16 and a C11 compiler. Nothing else is assumed.
2. **Device names** — serial nodes come from `prterm.ini`, never from a
   compiled-in list. FreeBSD uses `cuaU*` (callout, not `ttyU*`), Linux
   `ttyUSB*`/`ttyACM*`; `/dev/serial/by-id/…` is the recommended form wherever
   it exists, because the numbering moves when adapters are swapped.
3. **Modem lines** — optional. With no `TIOCM*` on the platform the RTS/DTR
   handling compiles out and PRTERM still runs.
4. **Web server** — anything that speaks CGI. Shipped configs cover Apache,
   nginx + fcgiwrap, busybox httpd and lighttpd.

If a port needs more than that, it is a bug worth reporting.

---

## Architecture

```
            Browser
               |  HTTP
        +------+------+
        | Webserver    |  busybox httpd / apache / nginx+fcgiwrap / lighttpd
        +------+------+
               |  CGI (RFC 3875)
        +------+------+
        | prterm.cgi   |        <- core: terminal, band plan, compliance
        +------+------+
               |
   +-----------+-----------+------------------+
   |           |           |                  |
  cgi        pages      session            radio.c   <- vtable: pr_rig_vtbl
   |           |           |                  |
  html       admin       state         +------+------+
                                  simrig   tnc2   tmodem
                                             |
                                        +----+----+
                                        |prterm-tncd|  <- keeps ports open
                                        +---------+

   MailboxD plugs in at the shared runtime directory and the admin area
   └── MailboxD      standalone mailbox daemon — own tab in main + admin
```

Everything persistent lives in files under `[paths] runtime_dir`, which is what
lets the CGI stay stateless:

```
prterm.runtime/
  state.ini      rig state (frequency, mode, PTT, duplex, counters)
  log            message log, ring-limited by [radio] max_log
  sessions/      admin sessions, one file each
  lock           advisory lock for state mutation
  tnc-*.sock     sockets to prterm-tncd
```

Each radio driver is a `pr_rig_vtbl` — open, close, refresh, get_state,
set_freq, set_mode, set_ptt, set_duplex, set_monitor, send, carrier_test, drain.
The compliance check runs **before** `send` is ever called.

---

## MailboxD — the mailbox daemon

MailboxD is **the mailbox and BBS, and nothing else**: mail, chat, conference and
the `/` commands. It is standalone additional software to PRTERM — its own
daemon, its own v1.0.0 release.

**PRTERM does everything else.** The radio, the band plan, the compliance gate,
the TNC drivers, the terminal — all of it is PRTERM. MailboxD has **no radio side
at all** and never talks to a TNC: RF reaches it only through PRTERM. That is
also why the radio transports are gone from MailboxD entirely — `packet_radio`,
`baycom` and the rest are PRTERM's business, or nobody's.

What it is not is a separate application to install, configure and look after:
PRTERM is the only place you ever touch it.

- **Setup happens entirely inside the PRTERM admin area.** No second URL, no
  configuration file of its own to edit, no separate web UI. Same rule as PRTERM
  itself: one URL, everything clickable where you work.
- **When installed and enabled, MailboxD adds a tab of its own** — one in the
  main window next to the terminal, and one in the admin area next to site,
  station, radio and callsign. Not installed or disabled, neither tab exists.
- **It shares PRTERM's identity and rules**: the same station identity, the same
  ban list, the same compliance gate in front of the transmit path.

Access is `telnet` (plain / secured / crypted) and `websocket` — the browser
gets it for free, because PRTERM is already there.

Interconnection between MailboxD instances is planned and deliberately not part
of v1.0.0.

---

## Development

### Tests

```sh
ctest --test-dir build --output-on-failure
```

Thirteen suites, all independent of hardware:

| Suite | Covers |
|---|---|
| `ini` | the INI parser, comment- and order-preserving writes |
| `bands` | band plan, channel lookup, power limits, transmit gate |
| `callsign` | CALLID/CALLERID rules, ban patterns |
| `config` | typed config model, add/remove bans, write-through |
| `cgi` | RFC 3875 helpers, escaping, URL encode/decode, buffers |
| `kiss` | KISS framing and escaping |
| `probe` | TNC banner detection, echo stripping, profile scoring |
| `state` | runtime init, state load/save, log tail, locking |
| `session` | password hash, the built-in password, sessions and expiry, CSRF, login throttle |
| `arbiter` | TX arbitration across processes: free channel, busy with owner, per frequency |
| `ui` | page composition: CALL:/RX/TX:/CQ: in the top bar, `<device>@<freq>@<baud>Baud` entries, send bar, mode/duplex as administration settings, Mailbox tab only when MailboxD is on, all five languages |
| `trace` | trace module: level names, filtering, timestamped lines, capped hexdumps |
| `tncd` | `prterm-tncd` against a **fake TNC on a pseudo terminal**, both device classes (TheFirmware and TAPR): KISS entry order **with entry verification**, no blind firmware reset at cold start, frame relay, RX pass-through, in-place repair via `CHECKUP`, leave KISS on shutdown — plus the `tnc2` driver path end to end incl. FCS validation |

### Build options

`PRTERM_SANITIZE` (ASan/UBSan), `PRTERM_WERROR`, `PRTERM_BUILD_TOOLS`, and one
`PRTERM_RIG_*` switch per driver.

---

## Roadmap

**PRTERM is the core** — band plan, compliance, radio drivers, terminal. Together
with MailboxD it becomes an **all-in-one stack**: one binary set, one
configuration, one admin area.

Also planned:

- **Baycom** — studied first, then built into PRTERM
- **ARDOP** — external plugin that talks to ARDOP software; no integration
- interconnection between MailboxD instances
- T-Modem driver (`PRTERM_RIG_TMODEM` switch exists, source not yet)
- band tables per country / region in `[bands]`
- channel raster validation and per-band power limits
- mandatory identification (transmitting the call sign)
- an own TNC on Raspberry Pi & Co., taking the slot `tmodem` holds until then

---

## Regulations

PRTERM follows the amateur radio and CB radio rules as well as the general
technical standards. The **solutions** are our own — the **rules** are obeyed.

The compliance layer sits in front of the transmit path. What it rejects does not
reach the air, not even as a KISS `DATA` frame.

> **Note:** PRTERM is operating software. It is **not** a type approval and
> **not** a radio licence. Compliance with local regulations is the operator's
> responsibility. Details in [`docs/REGULATIONS.md`](docs/REGULATIONS.md).

---

## Documentation

| File | Contents |
|---|---|
| [`docs/DESIGN.md`](docs/DESIGN.md) | architecture, project principles, portability |
| [`docs/ROUTING.md`](docs/ROUTING.md) | the single URL and every action |
| [`docs/REGULATIONS.md`](docs/REGULATIONS.md) | the rules and how PRTERM enforces them |
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | TNC protocols, AX.25, KISS |
| [`docs/TNC-INIT.md`](docs/TNC-INIT.md) | TNC initialisation, profiles, byte-exact frames |
| [`docs/TNC-OPERATION.md`](docs/TNC-OPERATION.md) | running a TNC — start order, verification, recovery |
| [`docs/MULTI-TNC.md`](docs/MULTI-TNC.md) | several complete stations on one channel |
| [`docs/UI-THEME.md`](docs/UI-THEME.md) | colours and how the look is achieved |
| [`docs/UI-FONTS.md`](docs/UI-FONTS.md) | fonts and the character grid |
| [`docs/WEB-SERVERS.md`](docs/WEB-SERVERS.md) | webserver compat (lighttpd, Apache, NGinx, busybox httpd) |
| [`docs/lighttpd.conf`](docs/lighttpd.conf) | lighttpd config snippet |
| [`docs/lighttpd-systemd.conf`](docs/lighttpd-systemd.conf) | lighttpd systemd unit snippet |
| [`docs/apache.conf`](docs/apache.conf) | Apache 2.4 config snippet |
| [`docs/nginx.conf`](docs/nginx.conf) | NGinx + fcgiwrap config snippet |
| [`docs/busybox-httpd.conf`](docs/busybox-httpd.conf) | Busybox httpd usage |

---

## License

**Non-Profit GNU/GPLv3 and newer Software/Data** — see [`LICENSE`](LICENSE).

Copyright (C) 2026 PRTERM contributors+

PRTERM bundles no third-party code — SHA-256, KISS framing and the INI parser are
written from scratch. If anything external is ever added, its license is listed
here.
