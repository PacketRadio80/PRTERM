# PRTERM — Web Server Compatibility

PRTERM is a standard RFC 3875 CGI program. It speaks HTTP/1.1 with a normal
`Content-Length` and `Content-Type`, and reads its environment the way
CGI specifies (`QUERY_STRING`, `REQUEST_METHOD`, `REMOTE_ADDR`, `COOKIE`, …).
Anything that can host a CGI binary serves PRTERM unchanged. The list below
is what we ship a config for today.

| Web server | CGI handling | Required pieces | Reference |
|---|---|---|---|
| **lighttpd**  | `mod_cgi`, `cgi.assign` (native) | lighttpd only | [`docs/lighttpd.conf`](lighttpd.conf) + [`lighttpd-systemd.conf`](lighttpd-systemd.conf) |
| **Apache 2.4+** | `mod_cgi` + `mod_alias` (native)    | apache2 / httpd only | [`docs/apache.conf`](apache.conf) |
| **NGinx** | via `fcgiwrap` (FastCGI ⟶ exec)    | nginx + fcgiwrap | [`docs/nginx.conf`](nginx.conf) |
| **Busybox httpd** | built-in CGI when `CONFIG_HTTPD_CGI=y` | busybox package | [`docs/busybox-httpd.conf`](busybox-httpd.conf) |

**Recommendation:** lighttpd 1.4 (the deployment on this host) — it's
the smallest footprint that still supports per-thread sockets, sensible
HTTP semantics, a CGI idiom that PRTERM was written against, and POSIX
file-descriptor passing through `mod_cgi` without needing an extra fork
(`/usr/bin/sendmail`-style wins). Apache is fine if you already run it.
NGinx + fcgiwrap is fine; NGinx alone is not enough. Busybox httpd is for
out-of-band tests only.

## Quick decision by use case

| Use case | Server | Why |
|----------|--------|-----|
| Production home station, internal LAN | **lighttpd** | tiny, fast, CGI-native, well-tested with PRTERM |
| Mixed web host (already runs Apache)    | **Apache**   | one less process; one less config to maintain |
| Mixed web host (already runs NGinx)    | **NGinx + fcgiwrap** | reuse the existing NGinx front; add only fcgiwrap |
| Rescue box, SBC, ramdisk init         | **busybox httpd** | tiny static binary, no dependencies, no service |
| Anything you want to debug interactively | **lighttpd with `-D` / `--debug`** | one-shot, no service |

## Common denominators across these servers

| Concern | How PRTERM handles it |
|---------|----------------------|
| **Cookie / session** | `HttpOnly`, `SameSite=Strict`, set by the CGI itself. No framework or auth backend. |
| **`prterm.ini` confidentiality** | All four configs above include a `*.ini` deny rule. PRTERM is the only reader — keep the INI outside the document root when in doubt. |
| **Static assets** | PRTERM embeds CSS and JS in the binary. There is nothing to serve except the `<SCRIPT>`-generated font route (`action=font` → `[ui] font_file`). |
| **Reverse-proxy compatibility** | PRTERM emits absolute-redirect-free responses. A reverse proxy in front works as long as it doesn't rewrite `QUERY_STRING`; with Apache / NGinx it is simpler to ScriptAlias / fastcgi_pass and skip the proxy. |
| **TLS / HTTPS** | PRTERM does NOT speak TLS itself. Terminate TLS at the webserver, then forward as plain HTTP (lighttpd & Apache) or maintain the same clear-text in NGinx. PRTERM's `pass_hash` covers the application login; TLS is a separate layer. |
| **CORS** | PRTERM is a single-origin application. There is no CORS endpoint; browsers see same-origin. |

## Things to watch out for

### NGinx + fcgiwrap

- `fcgiwrap` runs **one process per request**. For a personal terminal
  this is fine (low QPS, normal CGI latency a few ms). For high traffic
  scale up via `multiwatch` / `systemd` and a process pool, but PRTERM
  doesn't need that.
- `fastcgi_param SCRIPT_FILENAME` **must** point at the absolute path of
  `prterm.cgi`. The default `SCRIPT_NAME`-only setup will leave the env
  variable undefined and PRTERM will refuse to start the response.

### Apache

- Keep `Options +ExecCGI` set on the ScriptAlias dir AND `SetHandler
  cgi-script` on the same path; one without the other leaves either
  static serving (no exec) or partial CGI under wrong path.
- `ScriptAlias /prterm /path/to/prterm.cgi` (no trailing slash) keeps the
  CGI script as a single object. Use `ScriptAlias /prterm/ …` (with
  trailing slash) for directory mode.

### Busybox httpd

- `*.ini` **is served** by default unless denied by file permissions. We
  rely on the OS here: INI at `0640 root:lighttpd`, httpd running as
  `lighttpd`. If you run a different busybox setup, move the INI out of
  the doc root.
- No HTTPS, no access list, no auth realm beyond a single global user.
  Treat as a *dev-only* server.

### All four

- The INI path is hard-coded to `realpath()` of the cgi binary's directory.
  If the cgi lives under `/usr/lib/cgi-bin/prterm/prterm.cgi`, the INI is
  read from `/usr/lib/cgi-bin/prterm/prterm.ini`. Drop the INI next to the
  cgi, not three levels up.
- PRTERM writes to `[runtime] runtime_dir` (default `./prterm.runtime/`).
  This must be writable by the cgi's effective user (Apache: `apache` /
  `www-data`; lighttpd: `lighttpd`; NGinx + fcgiwrap: the user running
  fcgiwrap, default `www-data`; busybox: the user passed to `-u`).

## Featured Deploy (lighttpd, current reference)

The `deploy/10-prterm.conf` shipped with the source corresponds to:
- lighttpd drops `SCRIPT_NAME=/prterm/` and `SCRIPT_FILENAME=
  /var/www/html/prterm/prterm.cgi`.
- CGI runs as user `lighttpd` — that user owns `/var/www/html/prterm/`
  and the `prterm.runtime/` sockets.
- request logs go to `/var/log/lighttpd/access.log` (CGI to error log).
- `prterm-tncd` runs as `lighttpd` too — group `dialout` so it can talk
  to the USB-serial devices (`/dev/serial/by-id/...`). This is the
  detail in [`docs/lighttpd-systemd.conf`](lighttpd-systemd.conf).

See [`CONTINUE.md`](../CONTINUE.md) for the install / restart / smoke-test
workflow once the webserver side is wired.
