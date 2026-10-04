# PRTERM — Erscheinungsbild

> **Vorgabe:** *„Erstelle etwas Schönes und Nices, mit hell-silbrigem
> Hintergrund mit dunkel-blauer ins Schwarz gehender Schrift.“*

---

## 1. Grundidee

Gebürstetes Silber als Fläche, Schrift die von dunklem Blau ins Schwarz
verläuft. Kein Dark-Theme, kein Flachdesign-Grau — eine **metallische,
helle** Oberfläche mit Tiefe.

```
Fläche    : hell, silbrig, leichter Verlauf (wie gebürstetes Aluminium)
Schrift   : dunkelblau  ->  schwarz   (vertikaler Verlauf)
Akzent    : Stahlblau
Kontur    : kühles Grau-Silber
```

---

## 2. Palette

### Fläche (silbrig)

| Token        | Wert      | Einsatz                          |
| ------------ | --------- | -------------------------------- |
| `--bg-0`     | `#d6dae1` | Grundfläche                      |
| `--bg-1`     | `#eef1f5` | Lichtkante, hohe Flächen         |
| `--bg-2`     | `#b7bec9` | Schattenkante                    |
| `--bg-3`     | `#a3abb8` | tiefe Kanten                     |
| `--panel`    | `#e4e8ee` | Karten/Panels                    |
| `--inset`    | `#c9cfd8` | Eingabefelder, Log-Fläche        |

Verlauf der Grundfläche:

```css
background:
  linear-gradient(160deg, #eef1f5 0%, #d6dae1 45%, #b7bec9 100%);
```

### Schrift (dunkelblau → schwarz)

| Token        | Wert      | Einsatz                          |
| ------------ | --------- | -------------------------------- |
| `--fg-top`   | `#17335f` | obere Verlaufskante              |
| `--fg-bot`   | `#04070d` | untere Verlaufskante             |
| `--fg`       | `#0c1c3c` | Standardtext (Flächenwert)       |
| `--fg-dim`   | `#3b4c68` | Nebentext, Labels                |
| `--fg-faint` | `#6b7789` | Hilfstext                        |

Verlaufsschrift auf Überschriften und Status:

```css
background: linear-gradient(180deg, #17335f 0%, #04070d 100%);
-webkit-background-clip: text;
background-clip: text;
-webkit-text-fill-color: transparent;
color: #0c1c3c;                /* Fallback ohne background-clip */
```

### Akzent & Kontur

| Token        | Wert      | Einsatz                          |
| ------------ | --------- | -------------------------------- |
| `--accent`   | `#1e4d8c` | Links, aktive Elemente           |
| `--accent-2` | `#2f6bb8` | Hover, Fokus                     |
| `--line`     | `#98a2b1` | Trennlinien                      |
| `--line-2`   | `#78828f` | stärkere Konturen                |
| `--focus`    | `#2f6bb8` | Fokusring                        |

### Zustände

| Token        | Wert      | Einsatz                          |
| ------------ | --------- | -------------------------------- |
| `--rx`       | `#0e6b4c` | Empfang, ok                      |
| `--tx`       | `#8a4b08` | Senden, PTT                      |
| `--warn`     | `#8a6a08` | Warnung                          |
| `--err`      | `#8c1f28` | Fehler, gebannt                  |
| `--banned`   | `#5c2029` | Ban-Kennzeichnung                |

---

## 3. Wirkung im Detail

* **Silber wirkt über den Verlauf**, nicht über Flächenfarben. Flaches
  `#d6dae1` allein wirkt grau und tot — erst der Verlauf gibt die
  Metall-Anmutung.
* **Konturen statt Schatten.** Sehr weiche Schatten (`0 1px 0 #fff`
  oben, `0 -1px 0 #b7bec9` unten) lassen Flächen leicht gewölbt wirken —
  wie geprägtes Blech.
* **Schriftverlauf sparsam einsetzen**: Überschriften, Statuszeile,
  Frequenzanzeige. Fließtext bleibt `--fg`, sonst wird die Seite unruhig.
* **Monospace** für alles, was Zeichen zählt (Terminal, Frequenz,
  Rufzeichen) — das ist ein Funkterminal.

---

## 4. Kontrast

Die Paletten sind auf Lesbarkeit ausgelegt, nicht nur auf Stimmung:

| Kombination                | Kontrast | Ergebnis      |
| -------------------------- | -------- | ------------- |
| `--fg` auf `--bg-0`        | ~13:1    | AAA           |
| `--fg` auf `--panel`       | ~12:1    | AAA           |
| `--fg-dim` auf `--bg-0`    | ~7:1     | AAA           |
| `--accent` auf `--bg-1`    | ~6:1     | AAA           |
| `--fg-faint` auf `--bg-0`  | ~4.6:1   | AA (gross)    |

Der Schriftverlauf endet bei `#04070d` — nahezu Schwarz auf Silber, also
immer ausreichend Kontrast, unabhängig von der Fenstergröße.

---

## 5. Umsetzung

* Alle Farben als CSS Custom Properties in `:root`
* Theme-Umschaltung über `data-theme="dark"` auf `<html>`
  (als Option vorgesehen, Default ist die Silber-Variante)
* Schriftfamilie und -größe kommen aus `[ui]` der INI
* Verlaufsschrift nur mit Fallback, damit sie bei deaktiviertem
  `background-clip` nicht unsichtbar wird
