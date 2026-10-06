# PRTERM — Appearance

> **Requirement:** *"Create something beautiful and nice, with a light
> silvery background and dark blue lettering that fades into black."*

---

## 1. Basic idea

Brushed silver as the surface, lettering that runs from dark blue into
black. No dark theme, no flat-design grey — a **metallic, light**
surface with depth.

```
Surface   : light, silvery, slight gradient (like brushed aluminium)
Text      : dark blue  ->  black   (vertical gradient)
Accent    : steel blue
Contour   : cool grey-silver
```

---

## 2. Palette

### Surface (silvery)

| Token        | Value     | Use                              |
| ------------ | --------- | -------------------------------- |
| `--bg-0`     | `#d6dae1` | base surface                     |
| `--bg-1`     | `#eef1f5` | light edge, raised surfaces      |
| `--bg-2`     | `#b7bec9` | shadow edge                      |
| `--bg-3`     | `#a3abb8` | deep edges                       |
| `--panel`    | `#e4e8ee` | cards/panels                     |
| `--inset`    | `#c9cfd8` | input fields, log area           |

Gradient of the base surface:

```css
background:
  linear-gradient(160deg, #eef1f5 0%, #d6dae1 45%, #b7bec9 100%);
```

### Text (dark blue → black)

| Token        | Value     | Use                              |
| ------------ | --------- | -------------------------------- |
| `--fg-top`   | `#17335f` | upper gradient edge              |
| `--fg-bot`   | `#04070d` | lower gradient edge              |
| `--fg`       | `#0c1c3c` | body text (surface value)        |
| `--fg-dim`   | `#3b4c68` | secondary text, labels           |
| `--fg-faint` | `#6b7789` | help text                        |

Gradient lettering on headings and status:

```css
background: linear-gradient(180deg, #17335f 0%, #04070d 100%);
-webkit-background-clip: text;
background-clip: text;
-webkit-text-fill-color: transparent;
color: #0c1c3c;                /* fallback without background-clip */
```

### Accent & contour

| Token        | Value     | Use                              |
| ------------ | --------- | -------------------------------- |
| `--accent`   | `#1e4d8c` | links, active elements           |
| `--accent-2` | `#2f6bb8` | hover, focus                     |
| `--line`     | `#98a2b1` | separators                       |
| `--line-2`   | `#78828f` | stronger contours                |
| `--focus`    | `#2f6bb8` | focus ring                       |

### States

| Token        | Value     | Use                              |
| ------------ | --------- | -------------------------------- |
| `--rx`       | `#0e6b4c` | reception, ok                    |
| `--tx`       | `#8a4b08` | transmitting, PTT                |
| `--warn`     | `#8a6a08` | warning                          |
| `--err`      | `#8c1f28` | error, banned                    |
| `--banned`   | `#5c2029` | ban marking                      |

---

## 3. Effect in detail

* **Silver works through the gradient**, not through surface colors. Flat
  `#d6dae1` alone looks grey and dead — only the gradient gives the
  metallic look.
* **Contours instead of shadows.** Very soft shadows (`0 1px 0 #fff`
  on top, `0 -1px 0 #b7bec9` at the bottom) make surfaces look slightly
  domed — like embossed sheet metal.
* **Use the lettering gradient sparingly**: headings, status line,
  frequency display. Body text stays `--fg`, otherwise the page gets restless.
* **Monospace** for everything that counts characters (terminal, frequency,
  call sign) — this is a radio terminal.

---

## 4. Contrast

The palettes are designed for readability, not just for mood:

| Combination                | Contrast | Result      |
| -------------------------- | -------- | ----------- |
| `--fg` on `--bg-0`         | ~13:1    | AAA         |
| `--fg` on `--panel`        | ~12:1    | AAA         |
| `--fg-dim` on `--bg-0`     | ~7:1     | AAA         |
| `--accent` on `--bg-1`     | ~6:1     | AAA         |
| `--fg-faint` on `--bg-0`   | ~4.6:1   | AA (large)  |

The lettering gradient ends at `#04070d` — nearly black on silver, so
always sufficient contrast, independent of the window size.

---

## 5. Implementation

* All colors as CSS custom properties in `:root`
* Theme switching via `data-theme="dark"` on `<html>`
  (provided as an option, the default is the silver variant)
* Font family and size come from `[ui]` of the INI
* Gradient lettering only with a fallback, so it does not become
  invisible with `background-clip` disabled
