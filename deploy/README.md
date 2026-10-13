# PRTERM deployment files

Service/init files for permanent operation. Two platforms are supported;
pick the set that matches your host.

**All paths below are examples** (`/var/prterm` prefix, `lighttpd` user).
Adjust them to your installation. The one hard rule: `prterm-baycom` and
`prterm-tncd` must point at the **same** runtime directory — baycomd
creates the KISS PTY there (`kiss-<tag>`), tncd opens it. Start order is
always **baycomd first, tncd second**.

## FreeBSD (rc.d)

| File | Installs as | Purpose |
|------|-------------|---------|
| `prterm-baycomd` | `/usr/local/etc/rc.d/prterm-baycomd` | BayCom/Compa/PC-COM SER12 modem daemon (root: ioperm + SCHED_FIFO) |
| `prterm-tncd.rc` | `/usr/local/etc/rc.d/prterm-tncd` | TNC daemon (holds serial ports + KISS, runs as lighttpd) |

Enable and start:

```sh
sysrc prterm_baycomd_enable=YES
sysrc prterm_baycomd_device=/dev/cuau0     # real 8250/16550 UART, not USB-serial
sysrc prterm_baycomd_iobase=0x3f8          # COM1
sysrc prterm_tncd_enable=YES

service prterm-baycomd start
service prterm-tncd start
```

## Linux (systemd)

| File | Installs as | Purpose |
|------|-------------|---------|
| `prterm-baycom.service` | `/etc/systemd/system/prterm-baycom.service` | BayCom SER12 modem daemon |
| `prterm-tncd.service` | `/etc/systemd/system/prterm-tncd.service` | TNC daemon |
| `10-prterm.conf` | lighttpd conf.d | CGI routing + block `.ini`/runtime from being served |
| `tmpfiles-lighttpd.conf` | `/etc/tmpfiles.d/` | PID dir ownership for lighttpd |

Enable and start:

```sh
systemctl daemon-reload
systemctl enable --now prterm-baycom.service
systemctl enable --now prterm-tncd.service
```

`prterm-tncd.service` declares `After=prterm-baycom.service` so the PTY
exists before tncd tries to open it.

## BayCom hardware note

`prterm-baycom` bit-bangs the SER12 waveform through the UART registers,
so it needs a **real 8250/16550 UART with I/O-port access** (`/dev/ttyS0`
/ `cuau0`, iobase `0x3f8`) — a USB-serial adapter has no reachable
MSR/THR registers for this. It runs as root (ioperm / `/dev/io` +
`SCHED_FIFO`). TX is protected against a continuous carrier: see
`../docs/BAYCOM.md` (PTT watchdog, TOT, CSMA).
