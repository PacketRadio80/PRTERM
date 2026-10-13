/*
 * PRTERM Alter — progressive enhancement JavaScript.
 * Not required. The form works without it via POST + page reload.
 * Adds: auto-refresh polling, keyboard shortcuts, Enter-to-send,
 * typewriter effect (character-by-character like telnet).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
(function(){
    "use strict";

    /* ---- Typewriter effect ---- */
    var twQueue = [];
    var twBusy = false;
    var twDelay = 40;  /* default: 2400 baud */

    function typewrite(el, text, cb) {
        if (!el || text == null) { if (cb) cb(); return; }
        if (text === "") {
            el.textContent = "\n";
            if (cb) cb();
            return;
        }
        el.classList.add("tw-cursor");
        var i = 0;
        var term = document.getElementById("term");
        var iv = setInterval(function() {
            el.textContent = text.substring(0, ++i);
            if (term) term.scrollTop = term.scrollHeight;
            if (i >= text.length) {
                clearInterval(iv);
                el.classList.remove("tw-cursor");
                if (cb) cb();
            }
        }, twDelay);
    }

    function typewriteLine(el, text) {
        twQueue.push({ el: el, text: text });
        if (!twBusy) twProcess();
    }

    function twProcess() {
        if (!twQueue.length) { twBusy = false; return; }
        twBusy = true;
        var item = twQueue.shift();
        typewrite(item.el, item.text, twProcess);
    }

    /* ---- Polling ---- */
    var pollTimer = null;
    var term = document.getElementById("term");
    var lastTs = 0;
    if (term && term.getAttribute("data-last-ts"))
        lastTs = parseInt(term.getAttribute("data-last-ts"), 10) || 0;

    function poll() {
        var xhr = new XMLHttpRequest();
        xhr.open("GET", "prterm-alter.cgi?action=state&since=" + lastTs, true);
        xhr.onload = function() {
            if (xhr.status !== 200) return;
            try {
                var data = JSON.parse(xhr.responseText);
                if (!data.ok) return;
                if (data.typewriter_ms) twDelay = data.typewriter_ms;
                var term = document.getElementById("term");
                if (!term || !data.messages) return;
                var stick = term.scrollHeight - term.scrollTop - term.clientHeight < 24;
                for (var i = 0; i < data.messages.length; i++) {
                    var m = data.messages[i];
                    if (m.ts > lastTs) lastTs = m.ts;
                    var cls = "ln-" + (m.kind === "R" ? "rx" :
                                       m.kind === "T" ? "tx" :
                                       m.kind === "W" ? "warn" :
                                       m.kind === "E" ? "err" : "sys");
                    var t = new Date(m.ts * 1000);
                    var hh = ("0"+t.getHours()).slice(-2);
                    var mm = ("0"+t.getMinutes()).slice(-2);
                    var ss = ("0"+t.getSeconds()).slice(-2);
                    var prefix = hh+":"+mm+":"+ss;
                    if (m.from) prefix += " " + m.from;
                    if (m.to) prefix += " > " + m.to;

                    /* Last message: typewriter effect */
                    if (i === data.messages.length - 1) {
                        var txSpan = document.createElement("span");
                        txSpan.className = "tx";
                        term.appendChild(txSpan);
                        typewriteLine(txSpan, prefix + "  " + m.text);
                    } else {
                        term.appendChild(document.createTextNode(prefix + "  " + m.text + "\n"));
                    }
                }
                while (term.childNodes.length > 500) term.removeChild(term.firstChild);
                term.scrollTop = term.scrollHeight;

                /* Update status chips */
                if (data.freq_hz) {
                    var el = document.getElementById("s-freq");
                    if (el) el.textContent = (data.freq_hz / 1000000).toFixed(3) + " MHz";
                }
                if (data.mode) {
                    var el2 = document.getElementById("s-mode");
                    if (el2) el2.textContent = data.mode.toUpperCase();
                }
                if (data.device) {
                    var el3 = document.getElementById("s-device");
                    if (el3) el3.textContent = data.device;
                }
            } catch(e) {}
        };
        xhr.send();
    }

    function startPoll() {
        if (pollTimer) return;
        pollTimer = setInterval(poll, 3000);
        poll();
    }

    document.addEventListener("visibilitychange", function () {
        if (!document.hidden) poll();
    });

    /* Enter-to-send on the TX input */
    function setupTx() {
        var inp = document.getElementById("txtext");
        var frm = document.getElementById("txform");
        if (!inp || !frm) return;
        inp.addEventListener("keydown", function(e) {
            if (e.key === "Enter" && !e.shiftKey) {
                e.preventDefault();
                frm.submit();
            }
        });
    }

    /* MailboxD command form — AJAX instead of page reload.
     * Keeps focus, enables typewriter, same UX as prterm.cgi. */
    function setupMboxForm() {
        var frm = document.getElementById("mbox-form");
        var inp = document.getElementById("mbox-cmd");
        if (!frm || !inp) return;
        frm.addEventListener("submit", function(e) {
            e.preventDefault();
            var cmd = inp.value.trim();
            if (!cmd) return;
            inp.value = "";
            var term = document.getElementById("mbox-term");
            if (term) {
                var div = document.createElement("div");
                div.style.whiteSpace = "pre";
                div.style.color = "#b39ddb";
                typewriteLine(div, "> " + cmd);
                term.appendChild(div);
            }
            fetch("prterm-alter.cgi", {
                method: "POST",
                headers: { "Content-Type": "application/x-www-form-urlencoded" },
                body: "action=mbox_run&cmd=" + encodeURIComponent(cmd)
            })
            .then(function(r) { return r.json(); })
            .then(function(j) {
                if (!j) return;
                if (j.ok && j.lines) {
                    for (var i = 0; i < j.lines.length; i++) {
                        var d = document.createElement("div");
                        d.style.whiteSpace = "pre";
                        if (term) term.appendChild(d);
                        typewriteLine(d, j.lines[i]);
                    }
                } else if (j.error) {
                    var d2 = document.createElement("div");
                    d2.style.whiteSpace = "pre";
                    d2.style.color = "#f44336";
                    if (term) term.appendChild(d2);
                    typewriteLine(d2, j.error);
                }
                if (term) term.scrollTop = term.scrollHeight;
            })
            .catch(function() {});
            inp.focus();
        });
        inp.addEventListener("keydown", function(e) {
            if (e.key === "Enter") {
                e.preventDefault();
                frm.dispatchEvent(new Event("submit"));
            }
        });
    }

    /* Init on DOMContentLoaded */
    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", function() {
            setupTx();
            setupMboxForm();
            startPoll();
            typewriteServerOutput();
        });
    } else {
        setupTx();
        setupMboxForm();
        startPoll();
        typewriteServerOutput();
    }

    /* After a mbox_run POST, server renders empty <div class="mbox-line"
     * data-text="..."> elements. Type each one character by character. */
    function typewriteServerOutput() {
        var lines = document.querySelectorAll(".mbox-line[data-text]");
        if (!lines.length) return;
        var idx = 0;
        function typeNext() {
            if (idx >= lines.length) return;
            var div = lines[idx++];
            var text = div.getAttribute("data-text") || "";
            if (!text.trim()) { div.innerHTML = "&nbsp;"; typeNext(); return; }
            var ci = 0;
            div.classList.add("tw-cursor");
            var iv = setInterval(function() {
                ci++;
                div.textContent = text.substring(0, ci);
                var box = div.closest("[style*=overflow]") || div.parentNode;
                if (box) box.scrollTop = box.scrollHeight;
                if (ci >= text.length) {
                    clearInterval(iv);
                    div.classList.remove("tw-cursor");
                    typeNext();
                }
            }, twDelay);
        }
        typeNext();
    }
})();