# BayCom/Compa PC-COM — PRTERM Integration

## What is the PC-COM?

The **Albrecht PC-COM** is a simple AFSK modem based on the **TCM3105**
(Texas Instruments). It is **not a TNC** — it only modulates/demodulates
1200-baud AFSK signals. All HDLC framing, KISS protocol handling and
bit-clocking are done by the host (PRTERM).

**Class:** BayCom-based SER12 (same family as BayCom, Compa, TFPCX)

## Hardware Connection

```
PC-COM (DE-9)  ─── Serial Cable  ───  Mainboard COM1 (uart0)
     │
     └── 3.5mm Jack ─── CB Radio (microphone/input)
```

| Signal | DE-9 Pin | Direction | Function |
|--------|----------|-----------|----------|
| TXD    | 3        | Host→Modem | AFSK modulation (charge pump) |
| RXD    | 2        | Modem→Host | Demodulated bits (NOT used!) |
| RTS    | 7        | Host→Modem | PTT (transmit switch) |
| CTS    | 8        | Modem→Host | Demodulated bit (SER12 RX) |
| DTR    | 4        | Host→Modem | Power / control |
| DSR    | 6        | Modem→Host | — |
| DCD    | 1        | Modem→Host | Carrier Detect (optional) |
| GND    | 5        | —         | Ground |

**Important:** With BayCom SER12 the demodulated RX signal arrives on
**CTS (pin 8)**, not on RXD (pin 2)! The host reads CTS as a single-bit
stream via the UART MSR register.

## UART Registers (direct hardware access)

The PC-COM requires **direct access to the UART registers** (ioperm/inb/outb).
Normal serial read/write commands do not work.

| Register | Offset | Function in BayCom SER12 |
|----------|--------|--------------------------|
| THR      | +0     | TX: write 0x00 = feed charge pump |
| RBR      | +0     | NOT used (RX via MSR) |
| IER      | +1     | Interrupt Enable (THRE + MSR) |
| FCR      | +2     | FIFO OFF |
| LCR      | +3     | 6N1 (6-bit, no parity, 1 stop) |
| MCR      | +4     | RTS=PTT, DTR+OUT2=idle state |
| MSR      | +6     | **CTS (bit 4) = RX data bit** |
| DLL/DLM  | +0/+1  | Baud divisor (when DLAB=1) |

### MCR Values

| State   | MCR  | Bits |
|---------|------|------|
| Idle    | 0x0D | DTR + OUT2 |
| PTT on  | 0x0E | DTR + OUT2 + RTS |
| TX bit=1 | 0x0F | DTR + OUT2 + RTS + DTR-bit |

### Baud Divisor

| State   | Divisor | UART Baud | Effective |
|---------|---------|-----------|-----------|
| RX/Idle | 144     | 800       | CTS polling, Soft-DCD PLL |
| TX      | 12      | 9600      | 1 bit/tick at 1200 baud |

The TX divisor is `(115200/8)/baud` (12 at 1200 baud): a 6N1 word clocks
8 bit-times per THR write, so one THR pulse lands exactly on one 1200-baud
air bit. The divisor is written **once on PTT rise** — re-writing it every
bit toggles DLAB mid-shift and starves the charge pump (MCR keys, but no RF).

## RX Path

```
Radio audio → TCM3105 demodulator → CTS pin (DE-9 pin 8)
    → MSR register (iobase+6, bit 4)
    → Soft-DCD PLL (bit timing recovery, NRZI decode)
    → HDLC framing (flag detect, bit-unstuffing, CRC-CCITT check)
    → KISS PTY (runtime_dir/kiss-bc0)
    → prterm-tncd (tnc2 driver, validates+strips FCS)
    → Browser terminal
```

### Soft-DCD PLL

The PLL runs at 1200 baud and reconstructs bit timing from CTS
transitions. It detects valid signals through a 3-window energy model:
transitions near the bit center = signal, transitions far away = noise.
After ~120 bits DCD is stable. Received frames are delivered to the KISS
PTY **with** their FCS trailer (PRTERM's tnc2 driver validates and strips
it, the same convention as TheFirmware TNCs).

## TX Path

```
Browser → prterm.cgi → prterm-tncd (tnc2 driver)
    → KISS PTY (runtime_dir/kiss-bc0)
    → HDLC framing (preamble flags, bit-stuffing, CRC-CCITT append)
    → NRZI encode + SER12 bit-bang (charge pump via THR 0x00)
    → DTR = data bit, RTS = PTT (MCR 0x0E|bit)
    → TCM3105 modulator → Radio
```

Charge pump: THR 0x00 written every bit-time produces SPACE pulses on TXD
that the TCM3105 modulates as AFSK. During PTT the engine **busy-spins on
an absolute bit deadline** (no nanosleep, no syscalls) so the pulse train
stays jitter-free; idle RX keeps the absolute nanosleep schedule.

### Algorithms

The SER12 bit-bang, Soft-DCD PLL, HDLC framing (bit-stuffing, CRC-CCITT),
NRZI encode/decode and the CSMA p-persistence arbiter are ported byte-for-
byte from the **MAX25-Stack `max25-bcpr`** userspace reference, which is
itself derived from the Linux kernel `baycom_ser_fdx.c` / `hdlcdrv.c`
(Thomas Sailer, GPL-2.0). MAX25 is the verified ground truth for this
hardware. An offline test (`test_baycom_offline`) exercises CRC, the HDLC
roundtrip, a full SER12 wire-level NRZI+PLL roundtrip, the TOT trip and the
PTT watchdog without any hardware or root.

## TX Protection (important!)

**The BayCom/Compa modem CANNOT produce a continuous carrier.** It only
transmits while there is actual data to send. This is by design to
protect the radio hardware and avoid blocking the channel.

The following mechanisms are built into `prterm-baycom`:

### PTT Watchdog (FlexNet SER12.doc)

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `ptt_wd_key_ms` | 14500 | Maximum continuous key-down time (14.5 s) |
| `ptt_wd_pause_ms` | 500 | Forced pause after key-down (0.5 s) |

After 14.5 seconds of continuous transmission the watchdog **drops PTT
for 500 ms**, then resumes. This prevents the transmitter from being
stuck in transmit mode indefinitely.

### Time-Out Timer (TOT)

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `max_key_ms` | 25000 | Maximum single key-down (25 s) |
| `max_consecutive` | 3 | Maximum consecutive TX bursts |
| `max_bursts` | 8 | Maximum total bursts |

If a single transmission exceeds 25 seconds, or more than 3 consecutive
bursts are sent, or more than 8 total bursts occur, the TOT **trips and
unkeys the transmitter immediately**. The TOT does not reset
automatically — the daemon must be restarted.

### CSMA (Carrier Sense Multiple Access)

Before transmitting, `prterm-baycom` listens on the channel. If the
channel is busy, it waits according to the p-persistence algorithm
(default: slot time 10 ms, persistence 40/255).

### Summary

The BayCom/Compa modem with `prterm-baycom` will:

- **Only transmit when there is data to send** (HDLC frames from KISS)
- **Drop PTT after 14.5 s** of continuous key-down (watchdog)
- **Unkey after 25 s** of a single burst (TOT)
- **Stop after 3 consecutive or 8 total bursts** (TOT)
- **Listen before transmitting** (CSMA)

**If your modem still produces a continuous carrier, this is a bug.
Please open an issue at https://github.com/PacketRadio80/PRTERM/issues
with your platform, serial port, and the output of `prterm-baycom`.**

## Diagnostics & Telemetry

`prterm-baycom` writes runtime files into the state directory (`-s`) so a
dead TX can be traced without a scope:

| File | Content |
|------|---------|
| `baycom-diag.log` | Every PTY read, queued/rejected KISS frame, PTT on/off, TX summary |
| `tx-last-<tag>` | Last TX telemetry: `ptt_ms`, `thr_writes`, `thr_rate`, `max_tick_gap_us`, `mean_gap_us`, `gaps_gt_2x`, `baud_us` |
| `dcd-<tag>` | Current Soft-/hard-DCD state (`dcd=0|1`) |
| `tot-trip-<tag>` | Written once when the TOT latches (reason, burst/consecutive counters) |

**Reading `tx-last-bc0` after a transmit:**

- `thr_rate` should be close to the baud rate (≈1200 at 1200 baud). A much
  lower rate means the charge pump was starved (timing gaps).
- `max_tick_gap_us` should stay under ~2× `baud_us` (≈1666 µs at 1200 baud).
  Large gaps point to missing RT scheduling — the daemon must run as root so
  `SCHED_FIFO` succeeds (it logs `SCHED_FIFO ok prio=…` on stderr).
- `ptt_ms` is the key-down time of the last frame; a 19-byte frame is ~0.2 s.

**RX pin auto-detect.** At startup the daemon asserts **DTR only** (never
RTS — RTS is PTT and asserting it would key the radio with no data) and
monitors CTS/DSR/RI/DCD for 3 s, picking the pin with the most transitions
as the RX line. On FreeBSD the modem lines are read through `TIOCMGET`
(uart(4) owns the chip); on Linux the MSR is read directly via `inb`. The
result is logged as `==> RX on <pin>`.

### No-RX troubleshooting

If RX delivers no signal (diag log shows no `dcd=1`, `baycom_rx_check`
shows 0 transitions):

1. **Cable: RX pin not connected** — most common BayCom/PC-COM fault. The
   cable often only wires TXD, RTS, DTR and GND, not the demodulated RX bit.
2. **TCM3105 RX stage defective** — transmit works, receive does not.
3. **Audio path** — radio audio does not reach the TCM3105 (3.5mm jack
   miswired or volume too low).
4. **Wrong RX pin** — run `baycom_rx_detect` and compare with the daemon's
   auto-detect result.

## Tools

| Tool | Purpose |
|------|---------|
| `prterm-baycom` | BayCom SER12 daemon (bit-bang, KISS PTY) |
| `baycom_rx_check` | Direct MSR/CTS measurement (diagnosis) |
| `baycom_rx_detect` | Auto-detect which MSR pin carries RX signal |
| `prterm-tncd` | TNC daemon (KISS via PTY) |
| `prterm-probe` | TNC detection |

### baycom_rx_check

```sh
# FreeBSD (root):
./baycom_rx_check 0x3f8 10    # iobase, seconds

# Expectation with open squelch:
# CTS=0: ~50%, CTS=1: ~50%, Transitions: >50
# Result: CTS=0 100%, Transitions=0 -> no RX signal
```

### baycom_rx_detect

```sh
# FreeBSD (root):
./baycom_rx_detect /dev/cuau0 3    # device, seconds

# Monitors all 4 MSR input pins (CTS, DSR, RI, DCD).
# The pin with the most transitions is the RX signal.
```

### prterm-baycom

```sh
# FreeBSD (root):
./prterm-baycom -d /dev/cuau0 -i 0x3f8 -b 1200 -t bc0 \
    -s /var/www/html/prterm/prterm.runtime

# Creates: prterm.runtime/kiss-bc0 (KISS PTY)
# Connection: prterm-tncd uses tnc2 driver via the PTY
```

## INI Configuration

Add a `[station:bc0]` section to `prterm.ini`:

```ini
[station:bc0]
driver      = tnc2
port        = ./prterm.runtime/kiss-bc0
baud        = 9600
radio_baud  = 1200
modem       = tcm3105
line        = 8n1
kiss_init   = native
mode        = fm
freq_hz     = 26875000
callerid    = PRTERM-3
antenne     = PC-COM DiPol
enabled     = true
```

**`kiss_init = native`** tells `prterm-tncd` that the device is already
in KISS mode (via the PTY from `prterm-baycom`). No terminal probe or
KISS entry sequence is needed.

## Building

```sh
cd PRTERM
cmake -S . -B build -G Ninja
ninja -C build

# The tools are built when PRTERM_BUILD_TOOLS=ON (default):
#   build/prterm-baycom
#   build/baycom_rx_check
#   build/baycom_rx_detect

# Offline algorithm test (no hardware, no root):
ctest --test-dir build -R baycom_offline --output-on-failure
```

The daemon needs **root** (direct I/O port access via `/dev/io` on FreeBSD
or `ioperm(2)` on Linux, plus `SCHED_FIFO`). It is portable across Linux
and FreeBSD; the only platform difference is how the RX modem line is read
(ioctl `TIOCMGET` on FreeBSD, `inb` on the MSR on Linux) — see
`dev_read_msr()`.

## Deployment (FreeBSD rc.d)

Install the daemon binary and the rc.d script, then enable:

```sh
sudo cp build/prterm-baycom /var/prterm/prterm-baycom
sudo chmod 555 /var/prterm/prterm-baycom
sudo cp deploy/prterm-baycomd /usr/local/etc/rc.d/prterm-baycomd
sudo chmod 555 /usr/local/etc/rc.d/prterm-baycomd

sysrc prterm_baycomd_enable=YES
sysrc prterm_baycomd_device=/dev/cuau0
sysrc prterm_baycomd_iobase=0x3f8
service prterm-baycomd start
```

The state directory (`-s`) must match PRTERM's `runtime_dir` so prterm-tncd
finds `kiss-<tag>`. Start `prterm-baycom` **before** `prterm-tncd`: the PTY
symlink must exist when the daemon opens the station. A Linux systemd unit
is provided in `deploy/prterm-baycom.service`.

## Sources

- `0-POOL/study/MAX25-Stack/stacks/max25-bcpr/` — SER12 userspace port
- `0-POOL/study/MAX25-Stack/docs/BAYCOM.md` — BayCom overview
- `0-POOL/study/MAX25-Stack/docs/BAYCOM-FREEZES.md` — Freeze warnings
- `0-POOL/study/MAX25-Stack/stacks/max25-bcpr/tools/max25-bcpr-ultimate-diag.sh` — Diagnosis tool
- Thomas Sailer: `baycom_ser_fdx.c` / `hdlcdrv.c` (Linux kernel, GPL-2.0)