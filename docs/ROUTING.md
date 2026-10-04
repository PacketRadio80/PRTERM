# PRTERM — URL-Oberfläche

> **Vorgabe:** *„Die Administration des CGI bekommt keine eigene URL,
> sondern ist einfach anklickbar im PRTERM über den Browser.“*

PRTERM hat deshalb **genau eine URL**.

---

## 1. Die einzige Adresse

```
/prterm.cgi
```

Keine Unterpfade. Kein `/admin`. Keine zweite Adresse.

Alles, was das CGI tut, hängt an Query-Parametern und Formularfeldern
derselben URL.

---

## 2. Aufrufarten

| Aufruf                                        | Antwort                |
| --------------------------------------------- | ---------------------- |
| `GET  /prterm.cgi`                            | HTML5 — die ganze App  |
| `GET  /prterm.cgi?action=state`               | JSON — Zustand         |
| `GET  /prterm.cgi?action=log`                 | JSON — Nachrichten     |
| `GET  /prterm.cgi?action=font`                | Schriftdatei (binär)   |
| `POST /prterm.cgi`  `action=…`                | Aktion, danach Redirect oder JSON |

`SCRIPT_NAME` ist der Server vorgegeben; PRTERM bildet Links relativ
(`<form action="">`), damit es unter beliebigem Pfad hängt —
`/cgi-bin/prterm.cgi`, `/prterm/prterm.cgi`, was auch immer.

---

## 3. Aktionen (POST)

Alle Aktionen laufen über ein verstecktes Feld `action` im selben Formular
bzw. als `fetch`-POST auf dieselbe URL.

### Bedienung

| `action`  | Felder                                   | Zweck                  |
| --------- | ---------------------------------------- | ---------------------- |
| `tx`      | `text`                                   | senden                 |
| `ptt`     | `on`                                     | PTT ein/aus            |
| `set`     | `freq_hz`, `mode`, `duplex`, `channel`   | Betrieb ändern         |
| `monitor` | `on`                                     | Nur-Empfang            |

### Administration

| `action`        | Felder                                        | Zweck               |
| --------------- | --------------------------------------------- | ------------------- |
| `login`         | `user`, `pass`                                | anmelden            |
| `logout`        | —                                             | abmelden            |
| `save_site`     | `site_name`, `subtitle`, `language`           | Allgemein           |
| `save_station`  | `callerid`, `qth`, `locator`                  | Station             |
| `save_radio`    | `driver`, `port`, `baud`, `duplex`, …         | Funk                |
| `save_callsign` | `callid_max_len`, `callerid_*`                | Rufzeichenregeln    |
| `save_ui`       | `font_file`, `font_size`, `line_height`, …    | Schrift & Raster    |
| `ban_add`       | `pattern`, `reason`                           | Ban anlegen         |
| `ban_del`       | `pattern`                                     | Ban entfernen       |
| `pass_change`   | `old`, `new`, `new2`                          | Passwort            |
| `config_save`   | `text`                                        | Roh-INI speichern   |

Alle Aktionen tragen `csrf` mit (Token aus der Session).

---

## 4. Sichtbarkeit im Browser

Die App ist **ein** HTML-Dokument mit mehreren Ansichten, die per Klick
umschalten:

```
data-view="terminal"      das Funkterminal (Standard)
data-view="admin"         Administrationsbereich
data-view="admin/site"    … mit Unterbereichen
```

* Ohne Login: das Terminal ist da, die Admin-Fläche ist **versteckt** bzw.
  zeigt nur die Anmelde-Box.
* Nach dem Klick auf „Administration“ öffnet sich ein **Dialog** zur
  Anmeldung — ohne Seitenwechsel.
* Eingeloggt erscheinen die Admin-Panels direkt im selben Fenster.

Der Wechsel geschieht im Browser. Es gibt **keinen** Zustand in der URL —
kein `#admin`, kein `?page=`, kein Reload. Wer die Seite neu lädt, steht
wieder im Terminal.

> **Warum:** die Administration ist ein Werkzeug im laufenden Betrieb, kein
> getrenntes System. Sie soll genau dort sein, wo gearbeitet wird. Und eine
> einzelne URL ist die einzige Oberfläche, die sich ohne Installation und
> ohne Webserver-Konfiguration überall gleich verhält.

---

## 5. Session & Cookie

Die Authentifizierung hängt **nicht** an der URL, sondern an einem Cookie:

```
Set-Cookie: PRTERM_SID=<hex>; Path=/; HttpOnly; SameSite=Strict
```

* `HttpOnly` — nicht aus JavaScript lesbar
* `SameSite=Strict` — kein CSRF über fremde Seiten
* zusätzliches `csrf`-Token in jedem Formular
* Ablaufzeit aus `[admin] session_ttl_min`

---

## 6. Assets

CSS und JavaScript sind **ins Binary eingebettet** und werden inline im
`<head>` ausgegeben — damit gibt es keine weiteren URLs, die konfiguriert
werden müssten.

Ausnahme: die Schriftdatei. Sie ist eine Nutzer-Datei und wird unter

```
GET /prterm.cgi?action=font
```

ausgeliefert. Es gibt **keine** generische Dateiausgabe — die Route liefert
ausschliesslich die in `[ui] font_file` eingetragene Datei.

---

## 7. Konsequenz für den Webserver

Der Server braucht eine einzige Zeile:

```
# Apache
ScriptAlias /prterm/ /pfad/zu/cgi-bin/

# nginx + fcgiwrap
location /prterm.cgi { include fastcgi_params; fastcgi_pass unix:/run/fcgiwrap.sock; }

# busybox httpd
/prterm.cgi
```

Keine Weiterleitungen, keine Alias-Tabelle, kein WebSocket, kein Reverse
Proxy. Genau das ist der Grund, warum PRTERM ein CGI ist.
