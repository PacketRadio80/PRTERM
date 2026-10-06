# PRTERM — URL Interface

> **Requirement:** *"The administration of the CGI gets no URL of its own,
> it is simply clickable in PRTERM via the browser."*

PRTERM therefore has **exactly one URL**.

---

## 1. The single address

```
/prterm.cgi
```

No sub-paths. No `/admin`. No second address.

Everything the CGI does hangs off query parameters and form fields
of the same URL.

---

## 2. Call types

| Call                                          | Response                        |
| --------------------------------------------- | ------------------------------- |
| `GET  /prterm.cgi`                            | HTML5 — the whole app           |
| `GET  /prterm.cgi?action=state`               | JSON — state                    |
| `GET  /prterm.cgi?action=log`                 | JSON — messages                 |
| `GET  /prterm.cgi?action=font`                | font file (binary)              |
| `POST /prterm.cgi`  `action=…`                | action, then redirect or JSON   |

`SCRIPT_NAME` is preset by the server; PRTERM builds links relatively
(`<form action="">`), so it hangs under any path —
`/cgi-bin/prterm.cgi`, `/prterm/prterm.cgi`, whatever.

---

## 3. Actions (POST)

All actions run through a hidden field `action` in the same form
or as a `fetch` POST to the same URL.

### Operation

| `action`  | Fields                                   | Purpose                |
| --------- | ---------------------------------------- | ---------------------- |
| `tx`      | `text`                                   | transmit               |
| `ptt`     | `on`                                     | PTT on/off             |
| `set`     | `freq_hz`, `mode`, `duplex`, `channel`   | change operation       |
| `monitor` | `on`                                     | receive only           |

### Administration

| `action`        | Fields                                        | Purpose             |
| --------------- | --------------------------------------------- | ------------------- |
| `login`         | `user`, `pass`                                | log in              |
| `logout`        | —                                             | log out             |
| `save_site`     | `site_name`, `subtitle`, `language`           | general             |
| `save_station`  | `callerid`, `qth`, `locator`                  | station             |
| `save_radio`    | `driver`, `port`, `baud`, `duplex`, …         | radio               |
| `save_callsign` | `callid_max_len`, `callerid_*`                | call sign rules     |
| `save_ui`       | `font_file`, `font_size`, `line_height`, …    | font & grid         |
| `ban_add`       | `pattern`, `reason`                           | create ban          |
| `ban_del`       | `pattern`                                     | remove ban          |
| `pass_change`   | `old`, `new`, `new2`                          | password            |
| `config_save`   | `text`                                        | save raw INI        |

All actions carry `csrf` with them (token from the session).

---

## 4. Visibility in the browser

The app is **one** HTML document with several views that switch
on click:

```
data-view="terminal"      the radio terminal (default)
data-view="admin"         administration area
data-view="admin/site"    … with subsections
```

* Without login: the terminal is there, the admin area is **hidden** or
  shows only the login box.
* After the click on "Administration" a **dialog** for logging in opens —
  without a page change.
* Once logged in, the admin panels appear directly in the same window.

The switch happens in the browser. There is **no** state in the URL —
no `#admin`, no `?page=`, no reload. Anyone who reloads the page lands
back in the terminal.

> **Why:** the administration is a tool in ongoing operation, not a
> separate system. It should be exactly where the work happens. And a
> single URL is the only interface that behaves the same everywhere
> without installation and without web server configuration.

---

## 5. Session & cookie

Authentication hangs **not** on the URL, but on a cookie:

```
Set-Cookie: PRTERM_SID=<hex>; Path=/; HttpOnly; SameSite=Strict
```

* `HttpOnly` — not readable from JavaScript
* `SameSite=Strict` — no CSRF via foreign pages
* additional `csrf` token in every form
* expiry time from `[admin] session_ttl_min`

---

## 6. Assets

CSS and JavaScript are **embedded in the binary** and emitted inline in the
`<head>` — so there are no further URLs that would need configuring.

Exception: the font file. It is a user file and is served under

```
GET /prterm.cgi?action=font
```

There is **no** generic file output — the route delivers
exclusively the file entered in `[ui] font_file`.

---

## 7. Consequence for the web server

The server needs a single line:

```
# Apache
ScriptAlias /prterm/ /path/to/cgi-bin/

# nginx + fcgiwrap
location /prterm.cgi { include fastcgi_params; fastcgi_pass unix:/run/fcgiwrap.sock; }

# busybox httpd
/prterm.cgi
```

No redirects, no alias table, no WebSocket, no reverse
proxy. That is exactly the reason why PRTERM is a CGI.
