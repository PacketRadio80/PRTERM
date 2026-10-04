# PRTERM — Regeln & Bestimmungen

> **Grundsatz:** PRTERM hält sich vollständig an die Amateurfunk- und
> CB-Funk-Bestimmungen sowie an die allgemeinen Vorgaben und Techniken.
> Die **Lösungen** sind unsere eigenen — die **Vorgaben** sind einzuhalten.

Die Compliance-Schicht liegt **vor** dem Sendepfad. Was sie ablehnt, geht
nicht auf die Luft, auch nicht als KISS-`DATA`-Frame.

---

## 1. Was PRTERM durchsetzt

| Ebene                     | Regel                                                       |
| ------------------------- | ----------------------------------------------------------- |
| **Frequenz**              | Senden nur innerhalb zugeteilter Bereiche                   |
| **Kanalraster**           | Rastertreue wo vorgeschrieben (CB: 40 Kanäle)               |
| **Leistung**              | maximale Sendeleistung je Band                              |
| **Bandbreite/Modus**      | nur im Band erlaubte Betriebsarten                          |
| **Identifikation**        | gültiges Rufzeichen, `CALLID`/`CALLERID`-Regel 6+2          |
| **Betriebsart**           | Halbduplex-Zwang auf Bändern ohne Vollduplex-Zuteilung      |

---

## 2. CB-Funk (Beispiel: CEPT / Deutschland)

```
Bereich   : 26.965 MHz ... 27.405 MHz
Kanalraster: 10 kHz  ->  40 Kanäle
Modi      : FM (und AM wo zugelassen)
Leistung  : 4 W FM / 1 W AM  (CEPT; landesspezifisch prüfen!)
Identifikation: wie vorgeschrieben, keine Fremdidentifikation
```

> **Wichtig:** CB ist ein **genehmigungsfreier** Dienst mit strikten
> technischen Bedingungen (Typenzulassung, Bauartzulassung). PRTERM ersetzt
> **keine** Bauartzulassung und darf nicht dazu benutzt werden, zugelassene
> Geräte außerhalb ihrer Zulassung zu betreiben. PRTERM ist eine
> **Bedien-/Terminalsoftware**.

## 3. Amateurfunk

```
Zuteilung   : landesspezifische Bänder (z.B. 160m..70cm)
Lizenz      : gültiges Rufzeichen erforderlich
Identifikation: Rufzeichen am Anfang/Ende der Aussendung, wie vorgeschrieben
Leistung    : lizenz-/bandabhängig
Modi        : wie im Bandplan vorgesehen
```

Amateurfunk ist **lizenzpflichtig**. PRTERM setzt keine Lizenz durch und
prüft keine Berechtigung — das ist Sache des Bedieners.

## 4. Technische Vorgaben

| Thema              | Vorgabe                                                      |
| ------------------ | ------------------------------------------------------------ |
| **AX.25**          | Adressierung 6+1 Byte shifted ASCII, SSID `0..15`, PID `0xF0` |
| **HDLC**           | FCS CRC-16, reflektiert `0x8408`, Init `0xFFFF`, XOR `0xFFFF` |
| **KISS**           | `FEND/FESC/TFEND/TFESC` = `C0/DB/DC/DD`                       |
| **CSMA**           | `TXDELAY`, `SLOTTIME`, `PERSIST` — CB: `PERSIST=255`          |
| **TX-Pacing**      | min. 1,5 s zwischen Sendungen                                |
| **Halbduplex**     | Trägersperre beachten, `SLOTTIME*10 ms` nach RX               |

## 5. Wo PRTERM bewusst eigene Wege geht

Das *Wie* ist unsere Lösung, solange das *Was* eingehalten wird:

- **CGI statt WebSocket**: "keine Installation" hat Vorrang.
- **INI-only**: eine Datei, kein Framework, kein Daemon-Zwang.
- **Vollduplex als Betriebsmodell**: RX/TX-Entkopplung im Terminal.
  Die *rechtliche* Prüfung bleibt unberührt und kann Senden verhindern.
- **CALLERID 6+2**: strenger als AX.25 (`-0..-15`), weil es die Vorgabe ist.
- **Eigene Architektur**: das Studienmaterial liefert nur das Verbindliche
  (Protokolle, AX.25, Gerätenamen, Sicherheitsparameter).

## 6. Grenzwerte (bewährt)

```
On-Air-Nachricht     max. 48 Byte
Sendepause           min. 1,5 s
Auto-Beacon-Abstand  min. 900 s
Band muss frei sein  min. 180 s
Ban-Liste            begrenzt (256)
```

## 7. Offene Punkte

Die konkreten Landesvorgaben sind **länderspezifisch** und werden als
konfigurierbare Bandtabellen in `[bands]` abgebildet, mit sinnvollen
Defaults. Wer PRTERM außerhalb der Defaults betreibt, ist für die
Einhaltung der lokalen Vorgaben verantwortlich.

- [ ] Bandtabellen je Land / Region
- [ ] Kanalraster-Validierung für CB
- [ ] Leistungsgrenzen je Band
- [ ] Identifikations-Pflicht (zwingende Rufzeichen-Aussendung)
