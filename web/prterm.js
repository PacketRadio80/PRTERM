/* ==========================================================================
   PRTERM - CB & Amateur Radio Terminal
   prterm.js

   One URL, several views. No framework, no build step.
   ========================================================================== */
(function () {
  "use strict";

  /* ----------------------------------------------------------------------
     State
     ---------------------------------------------------------------------- */
  var S = {
    view: "terminal",
    /*
     * One menu for reception and transmission (RX/TX):
     *   rxtx === ""   "All" - hear every device, CQ/broadcast goes
     *                 out over the device chosen in "cqdev"
     *   rxtx !== ""   exactly this device: hear it, transmit with it
     */
    rxtx: "",
    cqdev: "",      /* device for CQ/broadcast, only under "All"    */
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

  /*
   * Text in the language of the installation. The page carries the
   * translations with it (PRTERM_L), English is the key and the
   * fallback - exactly like pr_tr() on the server.
   */
  function L(s) {
    return (window.PRTERM_L && window.PRTERM_L[s]) || s;
  }

  /* ----------------------------------------------------------------------
     Character grid: as many characters as font size and resolution allow
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
    probe.textContent = "MMMMMMMMMM"; /* 10 chars   */
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
     Switch views (no URL, no reload)
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
     Mailbox (MailboxD) — local mailbox and BBS, rendered inverted.

     MailboxD is a separate daemon. This is only PRTERM's view onto it: the
     tab strip, the local mailbox login and the administration button. The
     link to the daemon itself is a separate step.
     ---------------------------------------------------------------------- */
  function mboxPanel(name) {
    qsa("[data-mbox]").forEach(function (x) {
      x.classList.toggle("is-active", x.getAttribute("data-mbox") === name);
    });
    var login = $("mbox-loginform");
    if (login) login.hidden = (name !== "login");
  }

  function mboxFlash(msg, kind) {
    var el = $("mbox-flash");
    if (!el) return;
    el.textContent = msg || "";
    el.className = "note" + (kind ? " " + kind : "");
    el.hidden = !msg;
  }

  function mboxNotConnected() {
    mboxFlash(L("MailboxD is not connected — the daemon is not linked yet."), "warn");
  }

  function sendMailboxCmd() {
    var i = $("mbox-cmd");
    if (!i) return;
    if (!i.value.trim()) return;
    mboxNotConnected();
    i.value = "";
  }

  function mboxLoginSubmit() {
    mboxNotConnected();
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
   * Two views on the same messages:
   *
   *   All          everything falling on the channel - every
   *                broadcast, every foreign conversation. Banned
   *                stations stay outside (the server filters them).
   *   <device>     only what THIS device picked up. 1200 and 2400
   *                baud are different modems - what one hears is not
   *                what the other hears.
   *
   * In "All" every message names the device that picked it up -
   * otherwise several TNCs on one channel could not be told apart.
   */
  function msgVisible(m) {
    if (!S.rxtx) return true;              /* All: everything        */
    /* System messages and own transmissions carry the device too -
     * those without one are shown everywhere. */
    if (m.kind !== "R") return !m.station || m.station === S.rxtx;
    return m.station === S.rxtx;
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
        /* Which device picked it up - only needed in "All".     */
        (m.station && !S.rxtx
          ? '<span class="dev">@' + esc(m.station) + "</span> " : "") +
        (m.from ? '<span class="who">' + esc(m.from) + "</span> " : "") +
        '<span class="tx">' + esc(m.text) + "</span>" +
        (m.db && m.db !== 0 ? ' <span class="db">' + m.db + " dB</span>" : "") +
        "</span>";
    }
    if (html) {
      t.insertAdjacentHTML("beforeend", html);
      /* limit so the DOM does not grow         */
      while (t.childNodes.length > 900) t.removeChild(t.firstChild);
      if (stick) scrollTerm();
    }
  }

  /* Rebuild the terminal completely from the stock - needed when
   * switching between "All" and a station. */
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

    set("s-rx", s.rx_count || 0);
    set("s-tx", s.tx_count || 0);
    set("s-signal", (s.rx_db != null ? s.rx_db : "-") + " dBm");

    var d = $("s-duplex");
    if (d) {
      var full = s.duplex === "full";
      d.textContent = L(full ? "FULL-DUPLEX" : "HALF-DUPLEX");
      d.className = "chip " + (full ? "is-duplex-full" : "is-duplex-half");
    }

    var l = $("s-link");
    if (l) {
      l.textContent = s.link_ok ? (s.device || L("connected")) : L("disconnected");
      l.className = "chip " + (s.link_ok ? "is-link-ok" : "is-link-bad");
    }

    var note = $("duplexnote");
    if (note) {
      note.textContent = L(s.duplex === "full"
        ? "Full duplex — reception continues while transmitting."
        : "Half duplex — no reception while transmitting.");
      note.className = "note " + (s.duplex === "full" ? "note-ok" : "note-warn");
    }
  }

  function fmtFreq(hz) {
    if (!hz) return "—";
    return (hz / 1000000).toFixed(3) + " MHz";
  }

  /*
   * The CQ menu only exists under "All". With a single device the
   * device itself transmits - a second choice would contradict it.
   */
  function showCqDev() {
    var lbl = $("cqlbl"), dev = $("cqdev");
    if (lbl) lbl.hidden = !!S.rxtx;
    if (dev) dev.hidden = !!S.rxtx;
  }

  /* ----------------------------------------------------------------------
     Actions
     ---------------------------------------------------------------------- */
  function withStation(data) {
    /* The device decides WHICH hardware is used - under "All" the one
     * chosen for CQ/broadcast, otherwise the selected device itself.
     * Reception filtering happens client-side, see msgVisible(). */
    data.station = S.rxtx || S.cqdev;
    /* Send the CSRF token along. Without it all fetch actions fail
     * with "invalid token" for logged-in users. */
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

    /* Station to call - stays empty for broadcast (CQ).    */
    var call = $("callto");
    var to = call ? call.value.trim().toUpperCase() : "";

    /*
     * Broadcast (CQ) only under "All" - there the CQ menu decides
     * which device transmits. With a single device one talks to ONE
     * partner, so a destination must be given. The server checks
     * this too.
     */
    var isAll = !S.rxtx;
    if (!isAll && (!to || to === "CQ")) {
      flash(L("Please address a station — broadcast only under \"All\"."), "warn");
      if (call) call.focus();
      return;
    }

    var payload = { action: "tx", text: text, to: to };
    if (isAll) payload.bcast = "1";

    post(payload, function (j) {
      if (j && j.ok === false) flash(j.error || L("sending failed"), "err");
      input.value = "";
      input.focus();
      refresh(true);
    });
  }

  /*
   * Test carrier in the admin area - a device test, not operation.
   *
   * Even an empty carrier is a transmission. So announce first, then
   * show the countdown and only send after confirmation. Abort is
   * possible at any time.
   */
  var pttBusy = false;

  function pttTest() {
    if (pttBusy) return;
    var out = $("pttstate");

    /* Stage 1: announce. Sends nothing yet.         */
    pttBusy = true;
    if (out) out.textContent = L("Checking …");
    post({ action: "ptt", run: "0" }, function (j) {
      if (!j || j.ok !== true) {
        if (out) out.textContent = "";
        flash((j && j.error) || L("test rejected"), "err");
        pttBusy = false;
        return;
      }
      var wait = j.wait || 3;
      if (out) out.textContent = j.announce || L("TX in %s seconds").replace("%s", wait);

      /* Countdown - abort stays possible.    */
      var left = wait;
      var tick = setInterval(function () {
        left--;
        if (left <= 0) {
          clearInterval(tick);
          if (out) out.textContent = L("Sending …");
          /* Stage 2: only now something goes on the air. */
          post({ action: "ptt", run: "1" }, function (k) {
            pttBusy = false;
            if (out) out.textContent = k && k.ok === true
              ? L("Test finished.") : "";
            if (k && k.ok === false) flash(k.error || L("test failed"), "err");
            refresh(true);
          });
        } else if (out) {
          out.textContent = (j.announce || "TX") + " \u00b7 " + left + " s left";
        }
      }, 1000);
    });
  }

  function flash(msg, kind) {
    // Show it first in the login dialog, if that is open - there the
    // message used to be invisible and login appeared "without message".
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
     Update
     ---------------------------------------------------------------------- */
  function refresh(force) {
    var q = "?action=state&rows=" + S.rows + "&cols=" + S.cols +
      (S.rxtx || S.cqdev
        ? "&station=" + encodeURIComponent(S.rxtx || S.cqdev) : "") +
      (S.since ? "&since=" + S.since : "");
    fetch(q, { credentials: "same-origin" })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (!j) return;
        S.loggedIn = !!j.logged_in;
        renderState(j);
        if (j.callerid !== undefined) S.callerid = j.callerid;
        if (j.messages && j.messages.length) {
          /* Remember what we know - otherwise the client appends the same
           * message again on every poll. */
          for (var i = 0; i < j.messages.length; i++) {
            S.msgs.push(j.messages[i]);
            var ts = j.messages[i].ts || 0;
            if (ts > S.since) S.since = ts;
          }
          /* Limit the stock   */
          while (S.msgs.length > 900) S.msgs.shift();
          renderLog(j.messages);
        }
        if (j.error) flash(j.error, "warn");
      })
      .catch(function () { /* next run            */ });

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
     Dialog / login
     ---------------------------------------------------------------------- */
  function openLogin() {
    var dlg = $("logindlg");
    if (!dlg) return;
    var lm = $("loginmsg");
    if (lm) lm.hidden = true;      // remove old message
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
    /* View switcher    */
    qsa("[data-goto]").forEach(function (b) {
      b.addEventListener("click", function () {
        switchView(b.getAttribute("data-goto"));
      });
    });

    /* Mailbox view (MailboxD) — tab strip and administration button */
    qsa("[data-mbox]").forEach(function (b) {
      b.addEventListener("click", function () {
        mboxPanel(b.getAttribute("data-mbox"));
      });
    });

    var mboxForm = $("mbox-form");
    if (mboxForm) {
      mboxForm.addEventListener("submit", function (e) {
        e.preventDefault();
        sendMailboxCmd();
      });
    }

    var mboxLogin = $("mbox-loginform");
    if (mboxLogin) {
      mboxLogin.addEventListener("submit", function (e) {
        e.preventDefault();
        mboxLoginSubmit();
      });
    }

    /* Send bar    */
    var form = $("txform");
    if (form) {
      form.addEventListener("submit", function (e) {
        e.preventDefault();
        sendText();
      });
    }
    /*
     * RX/TX menu - reception filter and transmitting device in one.
     * Under "All" the CQ menu decides which device transmits; with a
     * single device the device itself is the sender.
     */
    var rxtx = $("rxtx");
    if (rxtx) {
      S.rxtx = rxtx.value || "";
      rxtx.addEventListener("change", function () {
        S.rxtx = rxtx.value || "";
        showCqDev();
        rerenderLog();
      });
    }
    var cqdev = $("cqdev");
    if (cqdev) {
      S.cqdev = cqdev.value || "";
      cqdev.addEventListener("change", function () {
        S.cqdev = cqdev.value || "";
      });
    }
    showCqDev();

    /* Enter in CALL: continues in the message line       */
    var callto = $("callto");
    if (callto) {
      callto.addEventListener("keydown", function (e) {
        if (e.key === "Enter") {
          e.preventDefault();
          var t = $("txtext");
          if (t) t.focus();
        }
      });
    }

    var ptt = $("ptttest");
    if (ptt) ptt.addEventListener("click", pttTest);

    /* Channel grid */
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

    /* Mode FM/AM/SSB and duplex are station settings - they are set
     * in the administration (Radio card) and no longer in the send
     * bar. Nothing to bind here. */

    /* Login     */
    var loginForm = $("loginform");
    if (loginForm) {
      loginForm.addEventListener("submit", function (e) {
        e.preventDefault();
        var uEl = $("loginuser"), pEl = $("loginpass");
        if (!uEl || !pEl) {
          flash(L("form incomplete — please reload"), "err");
          return;
        }
        var u = uEl.value, p = pEl.value;
        if (!u || !p) {
          flash(L("enter user and password"), "warn");
          return;
        }
        post({ action: "login", user: u, pass: p }, function (j) {
          if (j && j.ok) {
            closeLogin();
            location.reload();
          } else {
            flash((j && j.error) || L("login failed"), "err");
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

    /* Keyboard */
    document.addEventListener("keydown", function (e) {
      if (e.target && /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return;
      if (e.key === "/") { e.preventDefault(); var t = $("txtext"); if (t) t.focus(); }
    });

    /* Recompute the grid on resize                */
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
