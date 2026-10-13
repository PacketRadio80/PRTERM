# PRTERM — Design

## 0. Project principle (binding)

PRTERM is **a project of its own** and the intended successor of the systems
documented in the study material.

> The old is **studied**, but only adopted where it **cannot be done
> without**.

Concretely that means:

- No code, no directory structure, no naming conventions of the
  reference projects are copied.
- Only **external obligations** are adopted, i.e. things that
  PRTERM cannot change unilaterally:
  - the **wire protocol** of the TNCs (M25/1, KISS, ESC host frames)
  - **AX.25 addressing** and wire encoding
  - **device/tty names** of the platforms
  - proven **security parameters** (bans, retry counters)
- Everything else — architecture, UI, configuration, naming — is new
  and follows PRTERM's requirements.

---

## 1. Goal

Web terminal for CB and amateur radio as a **standard CGI** (RFC 3875).

- C11 + HTML5/CSS, configuration exclusively INI
- **No installation**: copying `prterm.cgi` + `prterm.ini` into a CGI folder
  is enough. Assets are embedded in the binary.
- **Full duplex**: RX keeps running during TX — exactly what classic
  CB radio cannot do.
- **CALLID filter/ban**, **CALLERID** with the `6+2` rule.
- **Admin area** for configuration.

## 2. Target platforms

| OS      | Architectures      |
| ------- | ------------------ |
| Linux   | x86-64, arm64      |
| FreeBSD | x86-64, arm64      |

Portability rules:

- Strict C11 + POSIX.1-2008/XSI, **no** GNU extensions, no `_GNU_SOURCE`
- `poll()` instead of `epoll`/`kqueue`
- `fcntl(F_SETLK)` instead of `flock()`
- `termios` by hand instead of `cfmakeraw()` (not POSIX)
- `/dev/urandom` instead of `getrandom()`
- no `/proc` paths; all paths come from the INI

## 3. Supported hardware

| Driver   | Rig                                  | Protocol               | Status    |
| -------- | ------------------------------------ | ---------------------- | --------- |
| `sim`    | —                                    | Simulation             | active path |
| `tnc2`   | Landolt TNC2C, PK-TNC2 (TheFirmware TNC-2 class) | KISS + ESC host mode   | in use    |
| `tnc2` (`native`) | BayCom/Compa/PC-COM SER12 via `prterm-baycom` | KISS over PTY  | in use    |
| `tmodem` | T-Modem (half-TNC)                   | ESC host frames        | planned   |

**Supported:** Baycom-class SER12 modems (PC-COM, Compa, TFPCX) run through the
standalone `prterm-baycom` daemon — see [`BAYCOM.md`](BAYCOM.md) and the README
for the continuous-carrier (PTT/TOT/watchdog/CSMA) protection.

**Not supported today:** ARDOP. It comes later, and only as an external plugin
that talks to ARDOP software — no integration.

**Out of scope for good:** VARA and other sound-card modems.

**Future:** an own TNC on Raspberry Pi & Co. — it will then take the same
slot as `tmodem`, which serves as a stopgap until then.

## 4. Architecture

```
                  Browser
                     |  HTTP
              +------+------+
              | Webserver    |  busybox httpd / apache / nginx+fcgiwrap
              +------+------+
                     |  CGI (RFC 3875)
              +------+------+
              | prterm.cgi   |
              +------+------+
                     |
     +-------+-------+-------+-------+-------+
     |       |       |       |       |       |
   cgi.c  pages  session  state  callsign config
     |                       |       |
   html.c                    |     ini.c
                             |
                       +-----+-----+
                       | radio.c   |   <- VTable: rig_vtbl
                       +-----+-----+
                             |
              +--------------+--------------+
              |              |              |
           simrig        tmodem          tnc2
```

### Layers

| Module        | Task                                                          |
| ------------- | ------------------------------------------------------------- |
| `cgi`         | RFC 3875 request/response, query/form/cookie, headers         |
| `ini`         | INI parser (read/write), the only persistence for config      |
| `config`      | Typed model on top of the INI, defaults, validation           |
| `callsign`    | CALLID/CALLERID validation + ban matching                     |
| `state`       | Rig state + log; file locks, so the CGI can stay stateless    |
| `radio`       | Rig abstraction (`rig_vtbl`), driver registry                 |
| `session`     | Admin auth: SHA-256 hash, session files, CSRF                 |
| `html`        | Escaping + layout                                             |
| `pages`       | Terminal page, admin pages, JSON API                          |

### Statelessness

CGI processes live per request. Everything persistent lies in files under
`[paths] runtime_dir`:

```
prterm.runtime/
  state.ini      rig state (frequency, mode, PTT, duplex, counters)
  log            message log (ring-limited via max_log)
  sessions/      admin sessions (one file per session)
  lock           advisory lock for state mutations
```

## 5. Configuration

`prterm.ini`, in the same folder as the binary. Sections:

```
[site]      [station]   [radio]   [admin]   [callsign]   [ban]   [paths]
```

The INI is **also** the admin interface: the admin area edits
the same keys that the shell tools touch.

## 6. Security

- `pass_hash` is `sha256$<salt>$<hash>`; empty hash ⇒ **no login possible**
- Sessions with expiry time, CSRF token dependent on the session
- Failed logins per IP are counted and banned
- Bans apply to CALLID/CALLERID with patterns (`*`, `?`)
- All output escaped for HTML/attributes/JSON

## 7. Build

```
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

Final product: `build/prterm.cgi` (one file) + `prterm.ini`.

## 8. Users, groups and privileges

PRTERM has three runtime identities. Most "socket/permission denied"
problems are really user-setup problems — this section is the whole
model.

### Rule 1: CGI and prterm-tncd run as the same user

`prterm.cgi` (and the `-alter` / `-text` variants) and `prterm-tncd`
should run as **the same operating-system user** — normally the web
server user (`lighttpd`, `www`, `www-data`, `apache`, or the user given
to busybox httpd `-u`). Both sides share `[runtime] runtime_dir`:

- `tnc-<station>.sock` — created by `prterm-tncd`, connected to by the CGI
- `lock`, `last_tx_<station>.ts` — written by the CGI
- `kiss-<tag>` — PTY symlink from `prterm-baycom`, opened by `prterm-tncd`

With one uid all of this works with no `chmod`, no umask tricks and no
group wrangling. So the web server user is the one that starts
`prterm-tncd` (systemd `User=`, rc.d as that user, or `sudo -u
<webserver-user> ...`).

Two different users are technically possible (shared group owning the
runtime dir and every socket) but that is extra configuration for no
benefit — avoid it.

### Rule 2: the user needs the serial-port group

Serial device nodes are owned by a system group that differs per
OS/distribution. The user running `prterm-tncd` must be a member of that
group, otherwise `open()` on the port fails with "permission denied":

| OS / distribution | Device node          | Group              |
| ----------------- | -------------------- | ------------------ |
| FreeBSD           | `/dev/cuau*`, `/dev/ttyU*` | `dialer` (660) |
| Debian / Ubuntu   | `/dev/ttyS*`, `/dev/ttyUSB*` | `dialout`  |
| Arch / Gentoo     | same nodes           | `uucp`             |
| others            | `ls -l /dev/<port>` shows it | often `tty` or `dialout` |

Check with `ls -l /dev/<port>`, then add the user to the owning group:

```sh
pw groupmod dialer -m lighttpd      # FreeBSD
usermod -aG dialout lighttpd        # Linux
```

Group membership is picked up at process start — restart `prterm-tncd`
after changing groups.

### Rule 3: prterm-baycom runs as root

`prterm-baycom` is the single exception: it bit-bangs the SER12 waveform
directly in the UART registers, which needs I/O-port privileges
(`/dev/io` on FreeBSD, `ioperm(2)` on Linux) and `SCHED_FIFO`. There is
no unprivileged way to touch UART registers, so this daemon **must** run
as **root**.

Keep the blast radius small: only `prterm-baycom` is root. It hands the
modem to `prterm-tncd` as a KISS PTY (`kiss-<tag>`), so the rest of the
stack stays unprivileged. The `baycom_rx_*` diagnostic tools need root
for the same reason.

### Summary

| Component                      | User                    | Why |
| ------------------------------ | ----------------------- | --- |
| `prterm.cgi` / `-alter` / `-text` | web server user      | served by the web server |
| `prterm-tncd`                  | **same** web server user | shared runtime dir/sockets (Rule 1), serial group member (Rule 2) |
| `prterm-baycom`                | root                    | direct UART register access (Rule 3) |
| `prterm-ini`, `prterm-probe`   | the same user as above  | they read/write `prterm.ini` + runtime state |

Never run the CGI or `prterm-tncd` as root: the web-facing parts must
not hold privileges. Only the register-level modem daemon does.
