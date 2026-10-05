/* ==========================================================================
   PRTERM - CB & Amateurfunk Terminal
   prterm.js

   Eine URL, mehrere Ansichten. Kein Framework, kein Build-Schritt.
   ========================================================================== */
(function () {
  "use strict";

  /* ----------------------------------------------------------------------
     Zustand
     ---------------------------------------------------------------------- */
  var S = {
    view: "terminal",
    station: "",
    since: 0,
    msgs: [],
    callerid: "",
    loggedIn: false,
    cols: 0,
    rows: 0,
    timer: null,
    pollMs: 1000,
    lastLog: "",
    busy: false
  };

  function $(id) { return document.getElementById(id); }
  function qs(sel, root) { return (root || document).querySelector(sel); }
  function qsa(sel, root) {
    return Array.prototype.slice.call((root || document).querySelectorAll(sel));
  }

  /* ----------------------------------------------------------------------
     Zeichenraster: so viele Zeichen wie Schriftgröße und Auflösung hergeben
     ---------------------------------------------------------------------- */
  function measureGrid() {
    var term = $("term");
    if (!term) return;

    var probe = document.createElement("span");
    probe.setAttribute("aria-hidden", "true");
    probe.style.cssText =
      "position:absolute;visibility:hidden;white-space:pre;" +
      "font-family:" + getComputedStyle(term).fontFamily + ";" +
      "font-size:" + getComputedStyle(term).fontSize + ";";
    probe.textContent = "MMMMMMMMMM"; /* 10 Zeichen */
    document.body.appendChild(probe);

    var charW = probe.getBoundingClientRect().width / 10;
    var style = getComputedStyle(term);
    var lineH = parseFloat(style.lineHeight) || 16;
    probe.remove();

    if (charW <= 0) return;

    var rect = term.getBoundingClientRect();
    S.cols = Math.max(20, Math.floor(rect.width / charW));
    S.rows = Math.max(5, Math.floor(rect.height / lineH));

    var el = $("gridinfo");
    if (el) el.textContent = S.cols + "x" + S.rows;
  }

  /* ----------------------------------------------------------------------
     Ansichten umschalten (keine URL, kein Reload)
     ---------------------------------------------------------------------- */
  function switchView(name) {
    S.view = name;
    qsa(".view").forEach(function (v) {
      v.classList.toggle("is-active", v.getAttribute("data-view") === name);
    });
    qsa("[data-goto]").forEach(function (b) {
      b.classList.toggle("is-active", b.getAttribute("data-goto") === name);
    });
    if (name === "terminal") {
      requestAnimationFrame(function () {
        measureGrid();
        scrollTerm();
      });
    }
    if (name === "admin" && !S.loggedIn) openLogin();
  }

  /* ----------------------------------------------------------------------
     Terminal
     ---------------------------------------------------------------------- */
  function scrollTerm() {
    var t = $("term");
    if (t) t.scrollTop = t.scrollHeight;
  }

  function esc(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;").replace(/</g, "&lt;")
      .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  /*
   * Zwei Sichten auf dieselben Nachrichten:
   *
   *   All            alles, was auf dem Kanal faellt - jeder Rundspruch,
   *                  jedes fremde Gespraech. Gebannte Stationen bleiben
   *                  draussen (filtert bereits der Server).
   *   Frequenz@Baud  nur, was an DIESE Station adressiert ist.
   *
   * In "All" steht bei jeder Nachricht das Geraet, das sie aufgefangen
   * hat - sonst waeren mehrere TNCs auf einem Kanal nicht zu unterscheiden.
   */
  function msgVisible(m) {
    /* System- und eigene Meldungen immer */
    if (m.kind !== "R") return true;
    if (!S.station) return true;
    /* Stationstab: nur an mich adressiert */
    var me = (S.callerid || "").toUpperCase();
    var to = (m.to || "").toUpperCase();
    return me !== "" && to === me;
  }

  function renderLog(items) {
    var t = $("term");
    if (!t) return;

    var html = "";
    var stick = t.scrollHeight - t.scrollTop - t.clientHeight < 24;

    for (var i = 0; i < items.length; i++) {
      var m = items[i];
      if (!msgVisible(m)) continue;
      var kind = m.kind || "S";
      var cls = "ln ln-" + ({ R: "rx", T: "tx", S: "sys", W: "warn", E: "err" }[kind] || "sys");
      var time = "";
      if (m.ts) {
        var d = new Date(m.ts * 1000);
        time =
          ("0" + d.getHours()).slice(-2) + ":" +
          ("0" + d.getMinutes()).slice(-2) + ":" +
          ("0" + d.getSeconds()).slice(-2);
      }
      html +=
        '<span class="' + cls + '">' +
        '<span class="t">' + time + "</span> " +
        /* Welches Geraet hat es gefangen - nur in "All" noetig. */
        (m.station && !S.station
          ? '<span class="dev">@' + esc(m.station) + "</span> " : "") +
        (m.from ? '<span class="who">' + esc(m.from) + "</span> " : "") +
        '<span class="tx">' + esc(m.text) + "</span>" +
        (m.db && m.db !== 0 ? ' <span class="db">' + m.db + " dB</span>" : "") +
        "</span>";
    }
    if (html) {
      t.insertAdjacentHTML("beforeend", html);
      /* begrenzen, damit das DOM nicht waechst */
      while (t.childNodes.length > 900) t.removeChild(t.firstChild);
      if (stick) scrollTerm();
    }
  }

  /* Terminal vollstaendig aus dem Bestand neu aufbauen - wird beim
   * Umschalten zwischen "All" und einer Station gebraucht. */
  function rerenderLog() {
    var t = $("term");
    if (!t) return;
    t.innerHTML = "";
    renderLog(S.msgs);
  }

  function renderState(s) {
    if (!s) return;
    function set(id, v) {
      var el = $(id);
      if (el && el.textContent !== String(v)) el.textContent = v;
    }
    set("s-callerid", s.callerid || "-");
    set("s-callid", s.callid || "CQ");
    set("s-freq", fmtFreq(s.freq_hz));
    set("s-mode", String(s.mode || "").toUpperCase());
    set("s-channel", s.channel > 0 ? s.channel : "—");

    // Auswahlkaesten aus dem Zustand zurueckschreiben. Ohne das zeigt der
    // Mode-Kasten nach einer abgelehnten Umstellung weiterhin den neuen
    // Wert, obwohl sich am Geraet nichts geaendert hat.
    var sel = $("selmode");
    if (sel && sel.value !== (s.mode || "")) sel.value = s.mode || "";
    var dsel = $("selduplex");
    if (dsel && dsel.value !== (s.duplex || "")) dsel.value = s.duplex || "";
    set("s-rx", s.rx_count || 0);
    set("s-tx", s.tx_count || 0);
    set("s-signal", (s.rx_db != null ? s.rx_db : "-") + " dBm");

    var d = $("s-duplex");
    if (d) {
      var full = s.duplex === "full";
      d.textContent = full ? "FULL-DUPLEX" : "HALB-DUPLEX";
      d.className = "chip " + (full ? "is-duplex-full" : "is-duplex-half");
    }

    var l = $("s-link");
    if (l) {
      l.textContent = s.link_ok ? (s.device || "verbunden") : "getrennt";
      l.className = "chip " + (s.link_ok ? "is-link-ok" : "is-link-bad");
    }

    var note = $("duplexnote");
    if (note) {
      note.textContent = s.duplex === "full"
        ? "Vollduplex — der Empfang läuft während des Sendens weiter."
        : "Halbduplex — während des Sendens wird nicht empfangen.";
      note.className = "note " + (s.duplex === "full" ? "note-ok" : "note-warn");
    }
  }

  function fmtFreq(hz) {
    if (!hz) return "—";
    return (hz / 1000000).toFixed(3) + " MHz";
  }

  /* ----------------------------------------------------------------------
     Aktionen
     ---------------------------------------------------------------------- */
    /* Die aktive Station bestimmt, mit WELCHER Hardware gearbeitet wird.
     Jeder Reiter steht fuer eine vollstaendige Station. */
  function withStation(data) {
    if (S.station) data.station = S.station;
    /* CSRF-Token mitschicken. Ohne das schlagen alle fetch-Aktionen
     * bei angemeldeten Nutzern mit "Token ungueltig" fehl. */
    var meta = document.querySelector('meta[name="csrf"]');
    if (meta) data.csrf = meta.getAttribute("content");
    return data;
  }

  function post(data, done) {
    data = withStation(data);
    if (S.busy) return;
    S.busy = true;
    var body = [];
    for (var k in data) {
      if (Object.prototype.hasOwnProperty.call(data, k)) {
        body.push(encodeURIComponent(k) + "=" + encodeURIComponent(data[k]));
      }
    }
    fetch("", {
      method: "POST",
      headers: {
        "Content-Type": "application/x-www-form-urlencoded",
        "X-Requested-With": "PRTERM"
      },
      body: body.join("&"),
      credentials: "same-origin"
    })
      .then(function (r) { return r.json().catch(function () { return null; }); })
      .then(function (j) { S.busy = false; if (done) done(j); })
      .catch(function () { S.busy = false; if (done) done(null); });
  }

  function sendText() {
    var input = $("txtext");
    if (!input) return;
    var text = input.value;
    if (!text.trim()) return;

    /* Anzurufende Station - bleibt leer fuer Rundruf (CQ). */
    var call = $("callto");
    var to = call ? call.value.trim().toUpperCase() : "";

    post({ action: "tx", text: text, to: to }, function (j) {
      if (j && j.ok === false) flash(j.error || "Senden fehlgeschlagen", "err");
      input.value = "";
      input.focus();
      refresh(true);
    });
  }

  /*
   * Pruef-Trager im Adminbereich - ein Ger\u00e4tetest, kein Betrieb.
   *
   * Auch ein leerer Traeger ist eine Funksendung. Darum zuerst ank\u00fcndigen,
   * dann den Rueckz\u00e4hler zeigen und erst nach der Bestaetigung senden.
   * Der Abbruch ist jederzeit moeglich.
   */
  var pttBusy = false;

  function pttTest() {
    if (pttBusy) return;
    var out = $("pttstate");

    /* Stufe 1: ank\u00fcndigen. Sendet noch nichts. */
    pttBusy = true;
    if (out) out.textContent = "Pr\u00fcfe \u2026";
    post({ action: "ptt", run: "0", station: S.station }, function (j) {
      if (!j || j.ok !== true) {
        if (out) out.textContent = "";
        flash((j && j.error) || "Test abgelehnt", "err");
        pttBusy = false;
        return;
      }
      var wait = j.wait || 3;
      if (out) out.textContent = j.announce || ("TX in " + wait + " Sekunden");

      /* Countdown - Abbruch bleibt moeglich. */
      var left = wait;
      var tick = setInterval(function () {
        left--;
        if (left <= 0) {
          clearInterval(tick);
          if (out) out.textContent = "Sende \u2026";
          /* Stufe 2: erst jetzt geht etwas auf die Luft. */
          post({ action: "ptt", run: "1", station: S.station }, function (k) {
            pttBusy = false;
            if (out) out.textContent = k && k.ok === true
              ? "Test beendet." : "";
            if (k && k.ok === false) flash(k.error || "Test fehlgeschlagen", "err");
            refresh(true);
          });
        } else if (out) {
          out.textContent = (j.announce || "TX") + " \u00b7 noch " + left + " s";
        }
      }, 1000);
    });
  }

  function flash(msg, kind) {
    // Zuerst im Login-Dialog zeigen, falls der offen ist - dort war die
    // Meldung bisher unsichtbar und die Anmeldung wirkte "ohne Meldung".
    var dlg = $("logindlg");
    if (dlg && dlg.open) {
      var lm = $("loginmsg");
      if (lm) {
        lm.textContent = msg;
        lm.className = "note note-" + (kind || "warn");
        lm.hidden = false;
      }
      return;
    }
    var el = $("flash");
    if (!el) return;
    el.textContent = msg;
    el.className = "note note-" + (kind || "warn");
    el.hidden = false;
    clearTimeout(flash._t);
    flash._t = setTimeout(function () { el.hidden = true; }, 5000);
  }

  /* ----------------------------------------------------------------------
     Aktualisierung
     ---------------------------------------------------------------------- */
  function refresh(force) {
    var q = "?action=state&rows=" + S.rows + "&cols=" + S.cols +
      (S.station ? "&station=" + encodeURIComponent(S.station) : "") +
      (S.since ? "&since=" + S.since : "");
    fetch(q, { credentials: "same-origin" })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (!j) return;
        S.loggedIn = !!j.logged_in;
        renderState(j);
        if (j.callerid !== undefined) S.callerid = j.callerid;
        if (j.messages && j.messages.length) {
          /* Merken, was wir kennen - sonst haengt der Client dieselbe
           * Meldung bei jedem Abruf erneut an. */
          for (var i = 0; i < j.messages.length; i++) {
            S.msgs.push(j.messages[i]);
            var ts = j.messages[i].ts || 0;
            if (ts > S.since) S.since = ts;
          }
          /* Bestand begrenzen */
          while (S.msgs.length > 900) S.msgs.shift();
          renderLog(j.messages);
        }
        if (j.error) flash(j.error, "warn");
      })
      .catch(function () { /* naechster Durchlauf */ });

    if (force) {
      fetch("?action=log", { credentials: "same-origin" })
        .then(function (r) { return r.json(); })
        .then(function (j) {
          var t = $("term");
          if (t && j && j.messages) {
            t.innerHTML = "";
            renderLog(j.messages);
          }
        })
        .catch(function () {});
    }
  }

  /* ----------------------------------------------------------------------
     Dialog / Anmeldung
     ---------------------------------------------------------------------- */
  function openLogin() {
    var dlg = $("logindlg");
    if (!dlg) return;
    var lm = $("loginmsg");
    if (lm) lm.hidden = true;      // alte Meldung entfernen
    if (typeof dlg.showModal === "function") dlg.showModal();
    else dlg.hidden = false;
    var u = $("loginuser");
    if (u) u.focus();
  }
  function closeLogin() {
    var dlg = $("logindlg");
    if (dlg && typeof dlg.close === "function") dlg.close();
    else if (dlg) dlg.hidden = true;
  }

  /* ----------------------------------------------------------------------
     Start
     ---------------------------------------------------------------------- */
  function boot() {
    /* Ansichtsschalter */
    qsa("[data-goto]").forEach(function (b) {
      b.addEventListener("click", function () {
        switchView(b.getAttribute("data-goto"));
      });
    });

    /* Sendeleiste */
    var form = $("txform");
    if (form) {
      form.addEventListener("submit", function (e) {
        e.preventDefault();
        sendText();
      });
    }
    var ptt = $("ptttest");
    if (ptt) ptt.addEventListener("click", pttTest);

    /* Stationsreiter - jeder steht fuer eine eigene Hardware */
    qsa("[data-station]").forEach(function (b) {
      b.addEventListener("click", function () {
        /* dieselbe Markierung wie Terminal / Administration rechts */
        qsa("[data-station]").forEach(function (x) { x.classList.remove("is-active"); });
        b.classList.add("is-active");
        S.station = b.getAttribute("data-station") || "";
        rerenderLog();
      });
    });

    /* Kanalraster */
    qsa(".ch").forEach(function (el) {
      el.addEventListener("click", function () {
        var n = el.getAttribute("data-ch");
        post({ action: "set", channel: n }, function () {
          qsa(".ch").forEach(function (x) { x.classList.remove("is-current"); });
          el.classList.add("is-current");
          refresh(true);
        });
      });
    });

    /* Betriebsart / Duplex */
    var mode = $("selmode");
    if (mode) {
      mode.addEventListener("change", function () {
        var want = mode.value;
        post({ action: "set", mode: want }, function (j) {
          if (j && j.ok === false) {
            flash(j.error || "Umschalten nicht moeglich", "err");
          }
          // Zustand neu lesen - setzt den Kasten auf den echten Wert
          refresh(true);
        });
      });
    }
    var duplex = $("selduplex");
    if (duplex) {
      duplex.addEventListener("change", function () {
        post({ action: "set", duplex: duplex.value }, function () {
          refresh(true);
          location.reload();
        });
      });
    }

    /* Anmeldung */
    var loginForm = $("loginform");
    if (loginForm) {
      loginForm.addEventListener("submit", function (e) {
        e.preventDefault();
        var uEl = $("loginuser"), pEl = $("loginpass");
        if (!uEl || !pEl) {
          flash("Formular unvollständig - bitte neu laden", "err");
          return;
        }
        var u = uEl.value, p = pEl.value;
        if (!u || !p) {
          flash("Benutzer und Passwort eingeben", "warn");
          return;
        }
        post({ action: "login", user: u, pass: p }, function (j) {
          if (j && j.ok) {
            closeLogin();
            location.reload();
          } else {
            flash((j && j.error) || "Anmeldung fehlgeschlagen", "err");
          }
        });
      });
    }
    qsa("[data-open-login]").forEach(function (b) {
      b.addEventListener("click", function (e) {
        e.preventDefault();
        openLogin();
      });
    });
    qsa("[data-close-login]").forEach(function (b) {
      b.addEventListener("click", closeLogin);
    });

    /* Tastatur */
    document.addEventListener("keydown", function (e) {
      if (e.target && /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return;
      if (e.key === "/") { e.preventDefault(); var t = $("txtext"); if (t) t.focus(); }
    });

    /* Raster bei Groessenaenderung neu ausrechnen */
    var term = $("term");
    if (term && typeof ResizeObserver !== "undefined") {
      new ResizeObserver(function () {
        measureGrid();
      }).observe(term);
    }
    window.addEventListener("resize", measureGrid);
    if (document.fonts && document.fonts.ready) {
      document.fonts.ready.then(measureGrid);
    }

    measureGrid();
    switchView("terminal");
    refresh(true);
    S.timer = setInterval(function () { refresh(false); }, S.pollMs);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
