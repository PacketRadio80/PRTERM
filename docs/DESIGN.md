# PRTERM — Design

## 0. Projektprinzip (verbindlich)

PRTERM ist **ein eigenes Projekt** und der gedachte Nachfolger der im
Studienmaterial dokumentierten Systeme.

> Das Alte wird **studiert**, aber nur übernommen, wo es **nicht anders geht**.

Konkret heisst das:

- Kein Code, keine Verzeichnisstruktur, keine Namenskonventionen der
  Referenzprojekte werden kopiert.
- Übernommen werden nur **externe Verbindlichkeiten**, also Dinge, die
  PRTERM nicht einseitig ändern kann:
  - das **Wire-Protokoll** der TNCs (M25/1, KISS, ESC-Hostframes)
  - **AX.25-Adressierung** und Wire-Encoding
  - **Geräte-/Tty-Namen** der Plattformen
  - bewährte **Sicherheitsparameter** (Bans, Retry-Zähler)
- Alles andere — Architektur, UI, Konfiguration, Namensgebung — ist neu
  und folgt den Anforderungen von PRTERM.

---

## 1. Ziel

Web-Terminal für CB- und Amateurfunk als **Standard-CGI** (RFC 3875).

- C11 + HTML5/CSS, Konfiguration ausschliesslich INI
- **Keine Installation**: `prterm.cgi` + `prterm.ini` in einen CGI-Ordner
  kopieren genügt. Assets sind ins Binary eingebettet.
- **Vollduplex**: RX läuft während TX weiter — genau das, was mit
  klassischem CB-Funk nicht geht.
- **CALLID-Filter/Ban**, **CALLERID** mit `6+2`-Regel.
- **Admin-Bereich** zum Konfigurieren.

## 2. Zielplattformen

| OS      | Architekturen      |
| ------- | ------------------ |
| Linux   | x86-64, arm64      |
| FreeBSD | x86-64, arm64      |

Portabilitätsregeln:

- Strict C11 + POSIX.1-2008/XSI, **keine** GNU-Extensions, kein `_GNU_SOURCE`
- `poll()` statt `epoll`/`kqueue`
- `fcntl(F_SETLK)` statt `flock()`
- `termios` von Hand statt `cfmakeraw()` (nicht POSIX)
- `/dev/urandom` statt `getrandom()`
- keine `/proc`-Pfade; alle Pfade kommen aus der INI

## 3. Unterstützte Hardware

| Treiber   | Geräte                              | Protokoll                | Status       |
| --------- | ----------------------------------- | ------------------------ | ------------ |
| `sim`     | —                                   | Simulation               | aktiver Pfad |
| `tmodem`  | T-Modem (half-TNC)                  | ESC-Hostframes           | Übergang     |
| `tnc2`    | Landolt TNC2C, PK-TNC2 (TNC2-Klone) | KISS + ESC-Hostmode      | geplant      |
| `max25`   | MAX25-Stack-TNCs                    | M25/1 Zeilenprotokoll    | geplant      |

**Bewusst nicht unterstützt:** Baycom, ARDOP, VARA, Soundkarten-Modems.

**Zukunft:** eigenes TNC auf Raspberry Pi & Co. — belegt dann denselben Slot
wie `tmodem`, das bis dahin als Ersatz dient.

## 4. Architektur

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
           simrig        tmodem          max25 / tnc2
```

### Schichten

| Modul        | Aufgabe                                                       |
| ------------ | ------------------------------------------------------------- |
| `cgi`        | RFC 3875 Request/Response, Query/Form/Cookie, Header           |
| `ini`        | INI-Parser (lesen/schreiben), die einzige Persistenz für Config |
| `config`     | Typisiertes Modell über der INI, Defaults, Validierung         |
| `callsign`   | CALLID/CALLERID-Validierung + Ban-Matching                     |
| `state`      | Rig-Zustand + Log; Datei-Locks, damit CGI zustandslos bleiben kann |
| `radio`      | Rig-Abstraktion (`rig_vtbl`), Driver-Registry                  |
| `session`    | Admin-Auth: SHA-256-Hash, Session-Dateien, CSRF                |
| `html`       | Escaping + Layout                                            |
| `pages`      | Terminal-Seite, Admin-Seiten, JSON-API                        |

### Zustandslosigkeit

CGI-Prozesse leben pro Request. Alles Persistente liegt in Dateien unter
`[paths] runtime_dir`:

```
prterm.runtime/
  state.ini      Rig-Zustand (Frequenz, Modus, PTT, Duplex, Zähler)
  log            Nachrichten-Log (ringbegrenzt über max_log)
  sessions/      Admin-Sessions (eine Datei je Session)
  lock           Advisory-Lock für State-Mutationen
```

## 5. Konfiguration

`prterm.ini`, im selben Ordner wie das Binary. Sektionen:

```
[site]      [station]   [radio]   [admin]   [callsign]   [ban]   [paths]
```

Die INI ist **auch** das Admin-Interface: der Admin-Bereich editiert
dieselben Schlüssel, die Shell-Tools auch anfassen.

## 6. Sicherheit

- `pass_hash` ist `sha256$<salt>$<hash>`; leerer Hash ⇒ **kein Login möglich**
- Sessions mit Ablaufzeit, CSRF-Token abhängig von der Session
- Login-Fehlversuche pro IP werden gezählt und gebannt
- Bans wirken auf CALLID/CALLERID mit Mustern (`*`, `?`)
- Alle Ausgaben HTML/Attribute/JSON-escapet

## 7. Build

```
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

Endprodukt: `build/prterm.cgi` (~eine Datei) + `prterm.ini`.
