# PRTERM — Font & Character Space

> **Requirement:** *"We keep as many characters as font size and screen
> resolution allow, and in the .ini one should be able to enter .otf/.ttf
> fonts including their size."*

---

## 1. Two independent controls

| What              | Where from                                 |
| ----------------- | ------------------------------------------ |
| **Font**          | `.otf` / `.ttf` — file **+** size          |
| **Character grid**| **derived** from font size × resolution    |

The font is a **user file**, not a build artifact. The user puts it
next to the CGI and enters it in the INI.

---

## 2. Configuration `[ui]`

```ini
[ui]
font_file   = ./fonts/prterm.ttf   ; .otf or .ttf
font_size   = 14                   ; CSS pixel
line_height = 1.2                  ; multiple of the font size
density     = compact              ; compact = maximum character grid
rows        = 0                    ; 0 = derive
columns     = 0                    ; 0 = derive
theme       = dark
```

`font_file` is relative to `prterm.ini` or absolute. Empty = system monospace.
`font_size` is passed through as a CSS custom property (`--pr-font-size`),
not hard-wired in CSS — so the font can be swapped without rebuilding.

---

## 3. Serving the font

The file is too large and too individual to be embedded in the binary
(in contrast to `prterm.css`/`prterm.js`). So an asset route serves it:

```
GET /prterm.cgi/font
```

- MIME according to the extension: `font/ttf`, `font/otf`, `font/woff2`
- `Cache-Control: public, max-age=31536000, immutable`
- `Content-Length` correct, no CGI caching
- the path comes exclusively from the INI — **no** path specification from the request

> **Security:** the route serves *only* the configured font file.
> There is no generic file download, otherwise it becomes a
> read oracle for the whole filesystem.

The `@font-face` rule is generated dynamically in the `<head>`, because `format()`
depends on the extension:

```css
@font-face {
  font-family: "PRTERM";
  src: url("…/prterm.cgi/font") format("truetype");   /* .ttf */
  /*                           format("opentype");      .otf */
  font-display: swap;
}
```

---

## 4. Character grid from size and resolution

The terminal uses the entire window area. The grid is **measured**,
not guessed:

```
character width = width of a <span> with 10 × "M"  /  10
line height     = font_size × line_height
columns         = floor(usable width   /  character width)
rows            = floor(usable height  /  line height)
```

- `height: 100dvh` (dvh, not vh — correct on mobile)
- `ResizeObserver` on the terminal container, recompute on
  window change and on font change
- `rows = 0` / `columns = 0` in the INI ⇒ **derived**
  `rows = n` / `columns = n` ⇒ forced (e.g. for screenshots or
  fixed console width)

The derived values go to the API, so the log delivers exactly as many
lines as are displayed — no scroll calculation in the browser.

### `density = compact`

Reduces everything that eats up character grid:

- line spacing of the log = exactly `line_height`
- status line can be shown/hidden, narrow by default
- no padding at the log, `margin: 0`
- scrollbar `overlay` where available
- TX line at the bottom edge, one line high

---

## 5. Grid fidelity

Since the font is variable, the log must **not** enforce fixed column
counts. Long lines wrap (`white-space: pre-wrap`), so the full width is used.
Whoever wants, sets `columns` fixed and gets a hard console width with
`overflow-x` instead of wrapping.

---

## 6. Open items

- [ ] font validation: only allow `.ttf`/`.otf`/`.woff2`, check the magic
- [ ] fallback chain: `font_file` empty or unreadable ⇒ system monospace
- [ ] limit `font_size` (e.g. 6–96 px) against broken input
- [ ] optional: embedded default font if none is given
