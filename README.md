# PRTERM

**CB & Amateurfunk Terminal als Standard-CGI.**

Ein Web-Terminal für CB- und Amateurfunk. C11 + HTML5/CSS, Konfiguration
ausschließlich als INI. Keine Installation: `prterm.cgi` + `prterm.ini` in
einen CGI-Ordner kopieren — fertig.

> **Stand: frühe Entwicklung.** Privates Projekt, noch nicht veröffentlicht.

---

## Merkmale

| | |
|---|---|
| **Vollduplex** | RX und TX laufen entkoppelt — der Empfang geht während des Sendens weiter |
| **Bandplan** | BNetzA-Vorgaben für CB-Funk Deutschland, 80 Kanäle |
| **Compliance** | Senden nur auf zugeteilten Kanälen, mit passender Betriebsart und Leistung |
| **CALLID / CALLERID** | `6+2`-Regel, strenger als AX.25 |
| **Ban-Filter** | Sperrung nach Muster (`*`, `?`) |
| **Administration** | direkt im Terminal anklickbar, **ohne eigene URL** |
| **Schrift** | eigene `.otf`/`.ttf` mit Größe aus der INI |
| **Zeichenraum** | so viele Zeichen wie Schriftgröße und Auflösung hergeben |
| **Plattform** | Linux + FreeBSD, x86-64 + arm64 |

---

## Start

### Bauen

```sh
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

Ergebnis: `build/prterm.cgi` — eine einzige Datei.

### Einrichten

```sh
cp build/prterm.cgi /pfad/zu/cgi-bin/
cp prterm.ini       /pfad/zu/cgi-bin/
```

Passwort erzeugen und in `prterm.ini` unter `[admin] pass_hash` eintragen:

```sh
./prterm.cgi --hash-password "meinPasswort"
```

Ohne `pass_hash` ist **kein Login möglich** — das ist der sichere Zustand.

### Webserver

Eine Zeile genügt:

```apache
# Apache
ScriptAlias /prterm/ /pfad/zu/cgi-bin/
```

```nginx
# nginx + fcgiwrap
location /prterm.cgi {
    include fastcgi_params;
    fastcgi_pass unix:/run/fcgiwrap.sock;
}
```

```sh
# busybox httpd (zum Ausprobieren)
busybox httpd -p 8080 -h /irgendein/ordner
```

Keine Weiterleitungen, kein WebSocket, kein Reverse Proxy.

---

## Werkzeuge

Das Binary ist auch ohne Webserver nützlich:

```sh
./prterm.cgi --help
./prterm.cgi --check-ini prterm.ini      # Konfiguration prüfen
./prterm.cgi --channels                  # Kanaltabelle CB Deutschland
./prterm.cgi --hash-password "pw"        # Hash für [admin] pass_hash
./prterm.cgi --print-config prterm.ini
```

Zusätzlich `prterm-ini` zum Ändern der Konfiguration aus Skripten:

```sh
prterm-ini list [SEKTION]
prterm-ini get  SEKTION SCHLUESSEL
prterm-ini set  SEKTION SCHLUESSEL WERT
prterm-ini del  SEKTION SCHLUESSEL
```

Beide schreiben die `prterm.ini` **kommentar- und reihenfolgetreu** zurück —
die Datei bleibt lesbar.

---

## Konfiguration

`prterm.ini`, im selben Ordner wie das Binary. Auszug:

```ini
[station]
callerid = PRTERM-1          ; Basis max. 6 + SSID "-<Ziffer>" = max. 8

[radio]
duplex  = full               ; full = Vollduplex, half = Halbduplex
driver  = sim                ; sim | tnc2 | tmodem | max25
port    = /dev/ttyUSB0       ; FreeBSD: /dev/cuaU0
baud    = 115200
freq_hz = 27125000
mode    = am                 ; fm | am | ssb

[ui]
font_file = ./fonts/prterm.ttf   ; .otf oder .ttf
font_size = 14
density   = compact              ; maximaler Zeichenraum
theme     = silver               ; hell-silbrig, Schrift dunkelblau -> schwarz

[ban]
DL9*     = Spam
KB1ABC-3 = Störer
```

Vollständige Erläuterung: `prterm.ini` selbst — sie ist durchkommentiert.

---

## Bandplan

Enthalten ist **CB-Funk Deutschland** mit 80 Kanälen nach der
BNetzA-Allgemeinzuteilung (Vfg. Nr. 21/2021):

```
Kanal  1–40 : 26.965 – 27.405 MHz   (CEPT, europäisch harmonisiert)
Kanal 41–80 : 26.565 – 26.955 MHz   (nationaler Erweiterungsbereich)
```

| Sendeart | Leistung | Kanäle |
|---|---|---|
| FM / PM | 4 W ERP | 1–80 |
| **AM** | 4 W ERP | **1–40** |
| SSB | 12 W PEP | 1–40 |

Besonderheiten, die in der Tabelle korrekt abgebildet sind:

- **Kanaldreher bei 23** — 22 = 27.225 → **23 = 27.255** → 24 = 27.235 → 25 = 27.245
- **Datenkanäle**: 6, 7, 24, 25, 52, 53, 76, 77
- **Gateway-Kanäle**: 11, 29, 34, 39, 40, 41, 61, 71, 80

Aufrufbar mit `./prterm.cgi --channels`.

---

## Hardware

| Treiber | Geräte | Protokoll | Stand |
|---|---|---|---|
| `sim` | — | Simulation | läuft |
| `tnc2` | Landolt TNC2C, PK-TNC2 | KISS + ESC-Hostmode | in Arbeit |
| `tmodem` | T-Modem (half-TNC) | ESC-Hostframes | geplant |
| `max25` | MAX25-Stack-TNCs | M25/1 | geplant |

Anschluss über COM-Port bzw. USB-SERIAL-Konverter.

| OS | Klassische TNCs | T-Modem |
|---|---|---|
| Linux | `/dev/ttyUSB0`, `/dev/ttyS0` | `/dev/ttyACM0` |
| FreeBSD | `/dev/cuaU0` | `/dev/cuaU0` |

**Bewusst nicht unterstützt:** Baycom, ARDOP, VARA, Soundkarten-Modems.

---

## Aufbau

```
prterm.cgi      alles in einer Datei, inkl. CSS und JavaScript
prterm.ini      einzige Konfiguration
fonts/          optional, eigene Schrift
prterm.runtime/ Laufzeitdaten (Zustand, Log, Sessions) - wird angelegt
```

Quellcode:

```
src/
  main.c        Einstieg: CGI oder Kommandozeile
  cgi.c         RFC 3875 Request/Response
  ini.c         INI-Parser, kommentar- und reihenfolgetreu
  config.c      typisiertes Modell ueber der INI
  bands.c       Bandplan + Compliance-Gate
  callsign.c    CALLID/CALLERID, Ban-Muster, AX.25-Encoding
  radio.c       Rig-Abstraktion (VTable)
  simrig.c      Simulation
  state.c       Zustand + Log (fuer zustandslose CGI-Prozesse)
  session.c     Anmeldung, Sessions, CSRF
  html.c        Layout, eingebettete Assets
  admin.c       Admin-Aktionen
  pages.c       Seiten + JSON-API
web/
  prterm.css    Erscheinungsbild
  prterm.js     Client-Logik
docs/           Entwurfsdokumente
```

---

## Dokumentation

| Datei | Inhalt |
|---|---|
| `docs/DESIGN.md` | Architektur, Projektprinzipien, Portabilität |
| `docs/ROUTING.md` | die einzige URL und alle Aktionen |
| `docs/REGULATIONS.md` | Vorgaben und wie PRTERM sie durchsetzt |
| `docs/UI-THEME.md` | Farben und Wirkung |
| `docs/UI-FONTS.md` | Schrift und Zeichenraum |
| `docs/PROTOCOL.md` | TNC-Protokolle, AX.25, KISS |
| `docs/TNC-INIT.md` | TNC-Initialisierung, Profile, Frames |

---

## Regeln und Bestimmungen

PRTERM hält sich an die Amateurfunk- und CB-Funk-Bestimmungen sowie an die
allgemeinen Vorgaben und Techniken. Die **Lösungen** sind eigene, die
**Vorgaben** werden eingehalten.

Die Compliance-Schicht liegt vor dem Sendepfad: was sie ablehnt, geht nicht
auf die Luft.

> **Hinweis:** PRTERM ist Bediensoftware und ersetzt keine Bauartzulassung
> und keine Funklizenz. Für die Einhaltung der lokalen Vorgaben ist der
> Bediener verantwortlich. Einzelheiten in `docs/REGULATIONS.md`.

---

## Lizenz

Noch nicht festgelegt — Projekt ist privat.
