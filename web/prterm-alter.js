/*
 * PRTERM Alter — progressive enhancement JavaScript.
 * Not required. The form works without it via POST + page reload.
 * Adds: auto-refresh polling, keyboard shortcuts, Enter-to-send.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
(function(){
    "use strict";

    /* Auto-refresh: poll state every 3 seconds and update terminal */
    var pollTimer = null;
    var lastTs = 0;

    function poll() {
        var xhr = new XMLHttpRequest();
        xhr.open("GET", "prterm-alter.cgi?action=state&since=" + lastTs, true);
        xhr.onload = function() {
            if (xhr.status !== 200) return;
            try {
                var data = JSON.parse(xhr.responseText);
                if (!data.ok) return;
                var term = document.getElementById("term");
                if (!term || !data.messages) return;
                for (var i = 0; i < data.messages.length; i++) {
                    var m = data.messages[i];
                    if (m.ts > lastTs) lastTs = m.ts;
                    var cls = "ln-" + (m.kind === "R" ? "rx" :
                                       m.kind === "T" ? "tx" :
                                       m.kind === "W" ? "warn" :
                                       m.kind === "E" ? "err" : "sys");
                    var line = document.createElement("div");
                    line.className = cls;
                    var t = new Date(m.ts * 1000);
                    var hh = ("0"+t.getHours()).slice(-2);
                    var mm = ("0"+t.getMinutes()).slice(-2);
                    var ss = ("0"+t.getSeconds()).slice(-2);
                    var prefix = hh+":"+mm+":"+ss;
                    if (m.from) prefix += " " + m.from;
                    if (m.to) prefix += " > " + m.to;
                    line.textContent = prefix + "  " + m.text;
                    term.appendChild(line);
                }
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

    /* Init on DOMContentLoaded */
    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", function() {
            setupTx();
            startPoll();
        });
    } else {
        setupTx();
        startPoll();
    }
})();