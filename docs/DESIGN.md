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

| Driver   | Rig                                  | Protocol               | Status         |
| -------- | ------------------------------------ | ---------------------- | -------------- |
| `sim`    | —                                    | Simulation             | active path    |
| `tmodem` | T-Modem (half-TNC)                   | ESC host frames        | transitional   |
| `tnc2`   | Landolt TNC2C, PK-TNC2 (TNC2 clones) | KISS + ESC host mode   | planned        |

**Not supported today:** Baycom and ARDOP. Baycom will be studied and built into
PRTERM later; ARDOP comes later still, and only as an external plugin that talks
to ARDOP software — no integration.

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
