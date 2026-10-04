# PRTERM — Mehrere Stationen

> **Vorgabe:** *„Es müssen mehrere gleichzeitig funktionieren auf dem
> selben Kanal.“*
>
> **Klärung:** *„Beide haben ein eigenes Funkgerät und eigene Antenne.“*

---

## 1. Das Modell

Es sind **keine** zwei TNCs an einem Funkgerät, sondern **zwei
vollständige Stationen**:

```
Station A:  TNC2C   ──►  Funkgerät A  ──►  Antenne A
Station B:  PK-TNC2 ──►  Funkgerät B  ──►  Antenne B
                                │
                          beide auf Kanal 24 (27.235 MHz)
```

Jede Station ist **in sich geschlossen**: eigener TNC, eigenes Funkgerät,
eigene Antenne, eigener serieller Anschluss, eigene Identität.

---

## 2. Warum verschiedene Funk-Baudraten kein Problem sind

| Station  | Funk-Baudrate | Vermögen |
| -------- | ------------- | ------- |
| TNC2C    | **2400**      | fest verdrahtet (Modem TCM3105) |
| PK-TNC2  | **1200**      | fest verdrahtet |

> *„Die weder aufwärts noch abwärts schalten können.“*

Die Funk-Baudrate ist **Hardware**, keine Einstellung. PRTERM darf sie
kennen, dokumentieren und anzeigen — aber **niemals versuchen zu ändern**.

Dass die beiden sich auf demselben Kanal nicht gegenseitig verstehen, ist
damit **erwartet und gewollt**: es sind unabhängige Stationen.

---

## 3. Konfiguration

```ini
[radio]
freq_hz   = 27235000     ; Kanal 24 - gemeinsame Frequenz
mode      = fm

[station:tnc2c]
driver     = tnc2
port       = /dev/serial/by-id/usb-FTDI_USB_Serial_Converter_FTC7OKUL-if00-port0
baud       = 19200        ; seriell zum TNC
radio_baud = 2400         ; FEST - Hardware, nicht aenderbar
modem      = tcm3105
line       = 8n1
callerid   = DL1ABC-1
antenne    = Vertikal

[station:pktn2c]
driver     = tnc2
port       = /dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller-if00-port0
baud       = 9600         ; seriell zum TNC
radio_baud = 1200         ; FEST - Hardware, nicht aenderbar
line       = 8n1
callerid   = DL1ABC-2
antenne    = Richtantenne
```

`radio_baud` wird bewusst **nicht** an das Gerät gesendet — es ist eine
Eigenschaft, keine Anweisung.

---

## 4. Senderegelung

Auch mit getrennten Funkgeräten gilt: **auf derselben Frequenz sendet
immer nur eine Station.** Sonst stören sich die Signale über die Luft,
unabhängig von der Antenne.

Die Regelung liegt in `src/arbiter.c`:

```
Sendewunsch  ──►  Sperre je Frequenz  ──►  senden  ──►  freigeben
```

* eine Sperre **je Frequenz** — Stationen auf verschiedenen Kanälen
  behindern sich nicht
* über `fcntl(F_SETLK)` — wirkt auch über mehrere CGI-Prozesse
* fällt automatisch, wenn ein Prozess endet — kein hängender Kanal
* zeigt bei Ablehnung an, **wer** gerade sendet

Empfang ist unkritisch: beide Stationen hören gleichzeitig und tragen in
ein gemeinsames Log ein.

---

## 5. Stand

|                                     |                                     |
| ----------------------------------- | ----------------------------------- |
| Mehrere Stationen konfigurieren     | in Arbeit                           |
| Senderegelung (Arbiter)             | **gebaut** (`src/arbiter.c`)        |
| Gemeinsames Log                     | Grundlage vorhanden                 |
| Eigene Identität je Station         | vorbereitet                         |
| Anzeige beider im Webinterface      | offen                               |
| Funk-Baudrate als Hardware-Eigenschaft | **gebaut**                      |

---

## 6. Warum das so und nicht anders

Die beiden Stationen könnten völlig getrennte Programme sein. PRTERM
führt sie trotzdem zusammen, weil:

* es **einen** Ort für Empfangslog und Bedienung geben soll
* die **Senderegelung** nur funktioniert, wenn sie alle Stationen sieht
* die **Compliance** (Bandplan, Leistung) für alle gleich gelten muss
