# PRTERM Quick Guide — on the air in 30 minutes

This guide is for **everyone** who wants to run PRTERM — no prior
knowledge needed. It takes you from a fresh system to the first
transmission with **lighttpd** on either of:

- **Debian-based Linux** (e.g. Raspberry Pi OS)
- **FreeBSD 15.x**

Every command is copy-and-paste ready. Background and details live in
the other documents (see [Read more](#10-read-more)).

## 0. What you need

| What | Examples |
|------|----------|
| Computer | Raspberry Pi 3/4/5 with Raspberry Pi OS, or a PC with FreeBSD 15.x |
| Radio | e.g. CB (channel 24 / 27.235 MHz) or amateur radio |
| TNC | Landolt TNC2C, PK-TNC2 (serial or USB-serial) |
| Cables | TNC ↔ computer (USB-serial adapter is fine), TNC ↔ radio |
| Browser | any current browser in the same network |

A BayCom/PC-COM modem is a special case (chapter 7) — a normal TNC is
all this guide assumes.

## 1. Install the software

**Debian / Raspberry Pi OS:**

```sh
sudo apt update
sudo apt install lighttpd cmake ninja-build gcc
```

**FreeBSD 15.x:**

```sh
sudo pkg install lighttpd cmake ninja
```

(The C compiler, clang, ships with FreeBSD.)

## 2. Build PRTERM

```sh
git clone https://github.com/PacketRadio80/PRTERM.git
cd PRTERM
cmake -S . -B build -G Ninja
ninja -C build
```

Done. Two of the binaries in `build/` matter for daily use:

| File | Purpose |
|------|---------|
| `build/prterm.cgi` | the terminal (the browser interface) |
| `build/prterm-tncd` | device daemon — keeps the TNC open and in KISS |

## 3. Install into the web tree

PRTERM is **two files in one folder** — nothing else is installed:

```sh
# Debian / Raspberry Pi OS
sudo mkdir -p /var/www/html/prterm
sudo cp build/prterm.cgi build/prterm-tncd /var/www/html/prterm/
sudo cp prterm.ini /var/www/html/prterm/
sudo chmod 555 /var/www/html/prterm/prterm.cgi

# FreeBSD 15.x (default lighttpd document root)
sudo mkdir -p /usr/local/www/data/prterm
sudo cp build/prterm.cgi build/prterm-tncd /usr/local/www/data/prterm/
sudo cp prterm.ini /usr/local/www/data/prterm/
sudo chmod 555 /usr/local/www/data/prterm/prterm.cgi
```

`prterm.ini` must stay **next to** `prterm.cgi` — that is where PRTERM
reads it from. The example file works for a first test; you will adjust
the `[station:...]` sections (which TNC is on which port?) and `[admin]`
(password) first.

**Which serial port?**

```sh
ls /dev/serial/by-id/    # Debian/RaspiOS with USB-serial adapters
ls /dev/cuau*            # FreeBSD
```

Example station section:

```ini
[station:tnc2c]
driver      = tnc2
port        = /dev/serial/by-id/usb-FTDI_...-port0   ; or /dev/cuau0
baud        = 19200
radio_baud  = 2400
kiss_init   = esc
mode        = fm
freq_hz     = 27235000
callerid    = YOUR-CALL
enabled     = true
```

**Set one absolute runtime path** (both daemons must agree on it):

```ini
[paths]
runtime_dir = /var/prterm/prterm.runtime
```

```sh
sudo mkdir -p /var/prterm/prterm.runtime
sudo chown www-data:www-data /var/prterm/prterm.runtime    # Debian user
# FreeBSD: sudo chown www:www /var/prterm/prterm.runtime
```

**Set the admin password:**

```sh
./build/prterm.cgi --hash-password "myPassword"
```

Put the printed `sha256$...` string into `prterm.ini` under
`[admin] pass_hash` (any editor, or
`./build/prterm-ini --file /path/to/prterm.ini set admin pass_hash 'sha256$...'`).
With an empty `pass_hash` **no login is possible** — the safe state.

## 4. Users and groups (important)

PRTERM has two running parts: the CGI (inside the web server) and the
device daemon `prterm-tncd`. **Both must run as the same user — your
web server's user.** That user also needs access to the serial port
(a group membership):

| System | Web server user | Serial-port group |
|--------|-----------------|-------------------|
| Debian / Raspberry Pi OS | `www-data` | `dialout` |
| FreeBSD 15.x | `www` (port default) | `dialer` |
| other | `grep server.username` in the lighttpd config | `ls -l /dev/<port>` shows it |

```sh
sudo usermod -aG dialout www-data     # Debian / Raspberry PiOS
sudo pw groupmod dialer -m www        # FreeBSD
```

Restart `prterm-tncd` after changing groups (membership is picked up at
process start).

> **Why the same user?** CGI and daemon share a state directory
> (`prterm.runtime/` with sockets and lock files). One user = no
> permission problems. And the CGI must **never** run as root — the
> only root component in the whole stack is the optional BayCom daemon
> (chapter 7).

## 5. Set up lighttpd

The shipped config enables CGI and protects `prterm.ini` from being
downloaded. Copy it and restart:

**Debian / Raspberry Pi OS:**

```sh
sudo cp deploy/10-prterm.conf /etc/lighttpd/conf-enabled/10-prterm.conf
sudo systemctl restart lighttpd
```

**FreeBSD 15.x:**

```sh
sudo cp deploy/10-prterm.conf /usr/local/etc/lighttpd/conf.d/10-prterm.conf
# if your lighttpd.conf has no "include" for conf.d, paste the file's
# content into /usr/local/etc/lighttpd/lighttpd.conf instead
sudo sysrc lighttpd_enable=YES
sudo service lighttpd restart
```

## 6. Start the device daemon (prterm-tncd)

A CGI process dies after every request, which would drop the TNC out of
KISS — `prterm-tncd` exists to hold the port and KISS open. It runs as
the **web server user** and reads the **same** `prterm.ini` as the CGI:

**Debian / Raspberry Pi OS (systemd):**

```sh
sudo cp deploy/prterm-tncd.service /etc/systemd/system/prterm-tncd.service
# edit it: User=www-data, Group=www-data, SupplementaryGroups=dialout
#          ExecStart=/var/www/html/prterm/prterm-tncd /var/www/html/prterm/prterm.ini
sudo systemctl daemon-reload
sudo systemctl enable --now prterm-tncd
```

**FreeBSD 15.x (rc.d):**

```sh
sudo cp deploy/prterm-tncd.rc /usr/local/etc/rc.d/prterm-tncd
sudo chmod 555 /usr/local/etc/rc.d/prterm-tncd
# the script uses /var/prterm paths and the lighttpd user — adjust or
# install prterm-tncd + prterm.ini there
sudo sysrc prterm_tncd_enable=YES
sudo service prterm-tncd start
```

**Dependency note:** `prterm-tncd` is **always** required — the CGI
only ever talks to the daemon. `prterm-baycom(d)` is **not** required
for normal TNCs; it is only needed for BayCom/PC-COM hardware, where it
provides the KISS interface (`kiss-<tag>`) that `prterm-tncd` opens.
Without BayCom hardware you never install it — and even with it, it is
`prterm-baycom(d)` that serves KISS to `prterm-tncd`, not the other way
around.

## 7. Optional: BayCom/PC-COM modem (prterm-baycom)

Only for BayCom/Compa/PC-COM hardware (SER12 AFSK modem, e.g. TCM3105).
`prterm-baycom` bit-bangs the UART registers directly and therefore
**must run as root** — it is the only PRTERM component that does.

Start order is fixed: **prterm-baycom(d) first, prterm-tncd second**
(the daemon creates the KISS PTY, tncd opens it; both must use the same
`runtime_dir`). Full details: [BAYCOM.md](BAYCOM.md).

## 8. Test it

1. Browser: `http://<IP-of-your-computer>/prterm/prterm.cgi`
2. The terminal appears. Log in with the admin password from chapter 3.
3. Send a short test (e.g. `CQ CQ DE YOUR-CALL`) and watch the TX
   counter in the status bar.
4. Command line checks if something looks wrong:

```sh
./build/prterm.cgi --selftest /path/to/prterm.ini tnc2c   # device state
./build/prterm.cgi --checkup  /path/to/prterm.ini tnc2c   # KISS is active
```

## 9. Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Browser shows 404 | wrong URL/path | try `/prterm/prterm.cgi` exactly |
| "no rig connected" | `prterm-tncd` not running or wrong port | `systemctl status prterm-tncd` / `service prterm-tncd status`; check `port` in the INI |
| "permission denied" on the port | web server user not in the serial group | chapter 4, then restart `prterm-tncd` |
| "channel not clear" | 150 s band-free rule (legal requirement) | wait — PRTERM refuses to stomp on an occupied channel |
| INI not found | `prterm.ini` not next to `prterm.cgi` | chapter 3 |
| Transmissions break off after the first | `prterm-tncd` missing (the CGI cannot hold the port) | chapter 6 |
| 500 / blank page | `prterm.cgi` not executable | `chmod 555 prterm.cgi`; check the lighttpd error log |
| Garbage on the air | wrong `radio_baud`/`modem` for your TNC | `./build/prterm-probe` detects the TNC profile |

## 10. Read more

| Topic | Document |
|-------|----------|
| Users, groups, privileges (full model) | [DESIGN.md §8](DESIGN.md) |
| Web server compatibility | [WEB-SERVERS.md](WEB-SERVERS.md) |
| BayCom/PC-COM modem | [BAYCOM.md](BAYCOM.md) |
| TNC initialization and wire behavior | [TNC-INIT.md](TNC-INIT.md) |
| Architecture and design rules | [DESIGN.md](DESIGN.md) |
| Configuration reference | `prterm.ini` comments, `--gen-ini` |
