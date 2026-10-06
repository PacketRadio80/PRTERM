# PRTERM — TNC Operation

How to bring a TNC up, prove it works, and get it back without a power cycle.

`docs/TNC-INIT.md` covers the **bytes** — serial settings, profiles, frames,
KISS parameters. This document covers the **operation**: what to do in which
order, what must be true before you transmit, and how to recover when it is not.

---

## 1. One process per port

This is the rule everything else hangs on.

```
exactly one owner per /dev/tty*
```

A TNC has no notion of two programs talking to it at once. If `minicom`, a
boot script and a daemon all hold the same port, they interleave writes and the
TNC ends up in a state none of them intended.

In PRTERM the owner is `prterm-tncd`. It opens the port once, enters KISS once
and keeps both alive. Nothing else touches the device afterwards — the CGI
writes commands to the daemon and never opens the serial port itself.

Before starting anything, make sure nobody else has the port:

```sh
fuser -v /dev/ttyUSB0     # or: lsof /dev/ttyUSB0
pkill minicom screen      # if anything is still attached
```

---

## 2. DTR decides whether the TNC boots at all

A falling DTR puts a TNC2C into **echo-only** mode: it stops responding to
commands and no longer acknowledges anything. This is not a bug you can retry
your way out of — it is a hardware state.

Two consequences, both non-negotiable:

1. **DTR must be high when the TNC powers up.** The program that holds the port
   has to be running *before or during* TNC power-on, not after it.
2. **Never close the port to "reset" the link.** Closing drops DTR and creates
   exactly the problem you were trying to fix. If the port is already open,
   leave it open.

PRTERM holds DTR and RTS asserted on open and re-asserts them on every write.
`prterm-tncd` keeps the descriptor open for exactly this reason.

> **The consequence that is easy to miss:** if you start PRTERM before the TNC
> has reached host mode, PRTERM cannot recover a cold-booted TNC on its own.
> The DTR sequencing has to happen at power-on.

---

## 3. Start order

```
  1. stop conflicting owners
  2. prepare each TNC  (per port)
  3. prove the TNC answers
  4. start PRTERM
  5. verify on air
```

### 1 — Stop conflicting owners

See §1. One port, one owner.

### 2 — Prepare each TNC

Per port. `prterm-probe` does the profile sweep and tells you what it found:

```sh
prterm-probe /dev/ttyUSB0                     # sweep profiles, report the match
prterm-probe --dump /dev/ttyUSB0 19200 8N1    # listen and log, no probing
prterm-probe --raw  /dev/ttyUSB0 19200 8N1    # raw bytes
```

It only ever sends serial commands. It never transmits.

### 3 — Prove the TNC answers

```sh
prterm.cgi --selftest prterm.ini              # device state
prterm.cgi --checkup  prterm.ini              # KISS active, buffers cleared
```

A known-good banner is the pass criterion. If the TNC answers with `?` in the
first bytes, it is not in host mode — go back to §2.

### 4 — Start PRTERM

`prterm-tncd` first, then the CGI. The daemon holds the port; the CGI talks to
the daemon.

### 5 — Verify on air

| Test | Action | Pass |
|---|---|---|
| Local session | open the terminal | login and RX log present |
| Manual transmit | send one message | PTT keys at the radio |
| On-air decode | second receiver or remote monitor | frame decodes with your CALLERID |
| Follow-up transmit | send again immediately | **also goes out** |

That last row is the one that catches the class of bug where the first
transmission works and the second does not — usually a driver that re-enters
KISS after sending and drops the next frame.

---

## 4. Host protocols and KISS entry

Not every TNC speaks KISS the same way. Two independent choices:

| Host protocol | Use |
|---|---|
| `kiss` | the normal path — AX.25 frames over the serial link |
| `hostmode` / `host` / `tnc2` | TNC2 host converse, interactive |
| `sixpack` | DF6BU 6PACK over serial |

How the TNC is put into KISS, and taken back out:

| Entry | | Exit | |
|---|---|---|---|
| `none` | already in KISS | `none` | stay in KISS |
| `kiss_on` | `kiss on` command | `kiss_off` | `kiss off` command |
| `esc_at_k` | `ESC @ K` frame | `kiss_frame` | `C0 FF C0` frame |
| `auto` | probe both | `auto` | probe both |

Details and byte sequences: `docs/TNC-INIT.md`.

---

## 5. TNC profiles

| Profile | Also known as | Line | RTS/DTR |
|---|---|---|---|
| `tnc2c` | `tnc2-c` | 7E1 | held |
| `tnc2` | `pktnc2`, `pk-tnc2`, `tapr`, `thefirmware` | 8N1 | held |
| `pk232` | `pk-232`, `aea` | 8N1 | off |
| `mfj1278` | `mfj-1278` | 7E1 | off |
| `kantronics` | `kpc`, `kpc3` | 8N1 | off |
| `generic` | `tnc` | 8N1 | off |

The serial line format is where most silent failures come from. The TNC2C
manual says 7E1; in practice the Landolt TNC2C runs **19200 8N1** — you can tell
because its banner arrives as clean ASCII, which would show the parity bit at
7E1. PRTERM warns when the configured line differs from the profile.

---

## 6. AX.25 sources — who sets what

| Item | Owner |
|---|---|
| `MYCALL` on the TNC | set once at preparation |
| source address of outgoing UI frames | the caller, per message |
| FCS on KISS `DATA` | **stripped before sending** — the TNC adds the CRC on air |
| `persist` on CB | `255` |

The FCS rule matters: if you hand a frame to a TNC that already carries a CRC,
the TNC appends a second one and nobody decodes you. PRTERM strips the FCS
**only** when the CRC-16 actually checks out.

`persist = 255` on CB is deliberate. Lower values make the station defer to
traffic that is not there.

---

## 7. Before you transmit

| Check | Pass criterion |
|---|---|
| Radio wired, antenna connected | never key into a dummy load you have not verified |
| First tests on the bench or low power | find the bug before you find the band |
| Squelch | **on** while local decode is good; open it only if you must |
| Channel and mode | `./prterm.cgi --channels` — compliance runs before TX |

Fringe and DX work needs an open squelch. Local work does not.

---

## 8. Recovery without a power cycle

| Symptom | Action |
|---|---|
| TNC answers `?` or nothing | not in host mode — DTR sequencing, see §2 |
| Echo-only on one port | stop PRTERM → recover that port → restart |
| PTT never keys although the log says transmit | check FCS strip, `persist`, and that `MYCALL` was set |
| Stuck after a crash | send the KISS return frame **before** re-preparing; do not skip the prep |
| Something is badly wrong | `prterm.cgi --reset-tnc prterm.ini` |

The emergency reset is the last step, not the first. It exists so you have an
escape hatch that does not require walking to the rack.

---

## 9. Reboot order

After the host has power-cycled:

1. **TNC power-on with DTR high** — the port owner must be running before or
   during power-on
2. **Prepare each TNC** — mandatory after a cold boot
3. **Start PRTERM** — it re-asserts RTS/DTR
4. **Verify on air** — within a few minutes, while you still remember what you
   changed

> Do **not** start PRTERM before the TNC has reached host mode. It cannot
> recover a cold-booted TNC without the DTR sequencing at power-on.

---

## See also

| Topic | Document |
|---|---|
| Serial settings, profiles, byte-exact frames | [`TNC-INIT.md`](TNC-INIT.md) |
| KISS framing and AX.25 wire format | [`PROTOCOL.md`](PROTOCOL.md) |
| Several complete stations on one channel | [`MULTI-TNC.md`](MULTI-TNC.md) |
| Band plan and compliance | [`REGULATIONS.md`](REGULATIONS.md) |
