# PRTERM — Schrift & Zeichenraum

> **Vorgabe:** *„Wir halten uns so viele Zeichen wie Schriftgröße und
> Bildschirmauflösung hergeben auf, und in der .ini soll man .otf/.ttf
> Fonts eintragen können samt Größe.“*

---

## 1. Zwei unabhängige Stellschrauben

| Was               | Woher                                      |
| ----------------- | ------------------------------------------ |
| **Schrift**       | `.otf` / `.ttf` — Datei **+** Größe        |
| **Zeichenraum**   | aus Schriftgröße × Auflösung **abgeleitet** |

Die Schrift ist eine **Nutzer-Datei**, kein Build-Artefakt. Der Nutzer legt
sie neben das CGI und trägt sie in der INI ein.

---

## 2. Konfiguration `[ui]`

```ini
[ui]
font_file   = ./fonts/prterm.ttf   ; .otf oder .ttf
font_size   = 14                   ; CSS-Pixel
line_height = 1.2                  ; Vielfaches der Schriftgroesse
density     = compact              ; compact = maximaler Zeichenraum
rows        = 0                    ; 0 = ableiten
columns     = 0                    ; 0 = ableiten
theme       = dark
```

`font_file` ist relativ zur `prterm.ini` oder absolut. Leer = System-Monospace.
`font_size` wird als CSS-Custom-Property durchgereicht (`--pr-font-size`),
nicht in CSS fest verdrahtet — damit ist die Schrift ohne Neubau wechselbar.

---

## 3. Auslieferung der Schrift

Die Datei ist zu gross und zu individuell, um ins Binary eingebettet zu
werden (im Gegensatz zu `prterm.css`/`prterm.js`). Also dient eine
Asset-Route:

```
GET /prterm.cgi/font
```

- MIME je nach Endung: `font/ttf`, `font/otf`, `font/woff2`
- `Cache-Control: public, max-age=31536000, immutable`
- `Content-Length` korrekt, kein CGI-Caching
- Pfad kommt ausschliesslich aus der INI — **keine** Pfadangabe aus dem Request

> **Sicherheit:** die Route liefert *nur* die konfigurierte Schriftdatei aus.
> Es gibt keinen generischen Datei-Download, sonst wird daraus ein
> Lese-Orakel für das ganze Dateisystem.

Die `@font-face`-Regel wird im `<head>` dynamisch erzeugt, weil `format()`
von der Endung abhängt:

```css
@font-face {
  font-family: "PRTERM";
  src: url("…/prterm.cgi/font") format("truetype");   /* .ttf */
  /*                           format("opentype");      .otf */
  font-display: swap;
}
```

---

## 4. Zeichenraum aus Größe und Auflösung

Das Terminal nutzt die gesamte Fensterfläche. Das Raster wird **gemessen**,
nicht geschätzt:

```
Zeichenbreite  = Breite eines <span> mit 10 × "M"  / 10
Zeilenhöhe     = font_size × line_height
Spalten        = floor(nutzbare Breite  / Zeichenbreite)
Zeilen         = floor(nutzbare Höhe   / Zeilenhöhe)
```

- `height: 100dvh` (dvh, nicht vh — mobil korrekt)
- `ResizeObserver` auf dem Terminal-Container, neu berechnen bei
  Fensteränderung und bei Schriftwechsel
- `rows = 0` / `columns = 0` in der INI ⇒ **abgeleitet**
  `rows = n` / `columns = n` ⇒ erzwungen (z.B. für Screenshots oder
  feste Konsolenbreite)

Die abgeleiteten Werte gehen an die API, damit das Log genau so viele
Zeilen liefert wie dargestellt werden — keine Scrollberechnung im Browser.

### `density = compact`

Reduziert alles, was Zeichenraum frisst:

- Zeilenabstand des Logs = exakt `line_height`
- Statuszeile ein-/ausblendbar, standardmässig schmal
- keine Innenabstände am Log, `margin: 0`
- Scrollbar `overlay` wo verfügbar
- TX-Zeile am unteren Rand, eine Zeile hoch

---

## 5. Rastertreue

Da die Schrift variabel ist, muss das Log **keine** festen Spaltenzahlen
erzwingen. Lange Zeilen umbrechen (`white-space: pre-wrap`), damit die
volle Breite genutzt wird. Wer will, setzt `columns` fest und bekommt
eine harte Konsolenbreite mit `overflow-x` statt Umbruch.

---

## 6. Offene Punkte

- [ ] Font-Validierung: nur `.ttf`/`.otf`/`.woff2` zulassen, Magic prüfen
- [ ] Fallback-Kette: `font_file` leer oder nicht lesbar ⇒ System-Monospace
- [ ] `font_size` begrenzen (z.B. 6–96 px) gegen kaputte Eingaben
- [ ] Optional: eingebettete Default-Schrift, falls keine angegeben ist
