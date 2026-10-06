# PRTERM — Rules & Regulations

> **Principle:** PRTERM complies fully with the amateur radio and
> CB radio regulations as well as with the general requirements and techniques.
> The **solutions** are our own — the **requirements** must be observed.

The compliance layer sits **in front of** the transmit path. What it rejects
does not go on the air, not even as a KISS-`DATA`-frame.

---

## 1. What PRTERM enforces

| Level                     | Rule                                                        |
| ------------------------- | ----------------------------------------------------------- |
| **Frequency**             | transmit only within allocated ranges                       |
| **Channel raster**        | raster adherence where prescribed (CB: 40 channels)         |
| **Power**                 | maximum transmit power per band                             |
| **Bandwidth/mode**        | only operating modes permitted in the band                  |
| **Identification**        | valid call sign, `CALLID`/`CALLERID` rule 6+2               |
| **Operating mode**        | half-duplex constraint on bands without full-duplex allocation |

---

## 2. CB radio (example: CEPT / Germany)

```
Range     : 26.965 MHz ... 27.405 MHz
Channel raster: 10 kHz  ->  40 channels
Modes     : FM (and AM where permitted)
Power     : 4 W FM / 1 W AM  (CEPT; check national rules!)
Identification: as prescribed, no foreign identification
```

> **Important:** CB is a **licence-exempt** service with strict
> technical conditions (homologation, type approval). PRTERM replaces
> **no** type approval and must not be used to operate approved
> rigs outside their approval. PRTERM is
> **operating/terminal software**.

## 3. Amateur radio

```
Allocation  : national bands (e.g. 160m..70cm)
Licence     : valid call sign required
Identification: call sign at the beginning/end of the transmission, as prescribed
Power       : licence-/band-dependent
Modes       : as provided for in the band plan
```

Amateur radio is **subject to licensing**. PRTERM does not enforce a licence and
checks no authorization — that is the operator's responsibility.

## 4. Technical requirements

| Topic              | Requirement                                                  |
| ------------------ | ------------------------------------------------------------ |
| **AX.25**          | addressing 6+1 byte shifted ASCII, SSID `0..15`, PID `0xF0` |
| **HDLC**           | FCS CRC-16, reflected `0x8408`, init `0xFFFF`, XOR `0xFFFF` |
| **KISS**           | `FEND/FESC/TFEND/TFESC` = `C0/DB/DC/DD`                      |
| **CSMA**           | `TXDELAY`, `SLOTTIME`, `PERSIST` — CB: `PERSIST=255`         |
| **TX pacing**      | min. 1.5 s between transmissions                             |
| **Half duplex**    | respect carrier lockout, `SLOTTIME*10 ms` after RX           |

## 5. Where PRTERM deliberately goes its own ways

The *how* is our solution, as long as the *what* is observed:

- **CGI instead of WebSocket**: "no installation" takes priority.
- **INI-only**: one file, no framework, no forced daemon.
- **Full duplex as operating model**: RX/TX decoupling in the terminal.
  The *legal* check remains untouched and can prevent transmitting.
- **CALLERID 6+2**: stricter than AX.25 (`-0..-15`), because it is the requirement.
- **Own architecture**: the study material only provides what is binding
  (protocols, AX.25, device names, security parameters).

## 6. Limits (proven)

```
On-air message       max. 48 byte
Transmit gap         min. 1.5 s
Auto-beacon interval min. 900 s
Band must be free    min. 180 s
Ban list             limited (256)
```

## 7. Open items

The concrete national requirements are **country-specific** and are mapped as
configurable band tables in `[bands]`, with sensible
defaults. Whoever runs PRTERM outside the defaults is responsible for
compliance with the local requirements.

- [ ] band tables per country / region
- [ ] channel raster validation for CB
- [ ] power limits per band
- [ ] mandatory identification (transmitting the call sign)
