/*
 * PRTERM - CB & Amateur Radio Terminal
 * lang.c - UI languages: English, German, Spanish, Portuguese, French.
 *
 * English is the SOURCE language and the key of every entry - a text
 * without an entry simply stays English, so the catalog can grow
 * without ever breaking the interface.
 *
 * What is translated is what the operator READS on screen: labels,
 * headings, buttons, hints, notes and the messages of the browser
 * script. Technical terms stay as they are (CALL:, RX/TX:, CQ:, QRG,
 * FULL-DUPLEX in the code, MHz, Baud, FM/AM/SSB) - they are the same
 * on the air in every language.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "lang.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ======================================================================= */
/* The shipped languages                                                   */
/* ======================================================================= */

typedef struct pr_lang {
    const char *code;    /* [site] language                  */
    const char *name;    /* endonym, for the selection       */
} pr_lang;

static const pr_lang langs[] = {
    { "en", "English"    },
    { "de", "Deutsch"    },
    { "es", "Español" },
    { "pt", "Portugu\u00eas" },
    { "fr", "Fran\u00e7ais"  },
};
#define LANG_COUNT (sizeof langs / sizeof langs[0])

/* ======================================================================= */
/* Catalog                                                                 */
/* ======================================================================= */

/*
 * One entry per text: English (the key), then the same text in the
 * order of langs[] - de, es, pt, fr.
 */
typedef struct pr_lang_entry {
    const char *col[LANG_COUNT];
} pr_lang_entry;

#define E(en, de, es, pt, fr)  { { en, de, es, pt, fr } }

static const pr_lang_entry catalog[] = {

    /* ---- Top bar ---------------------------------------------------- */
    E("Station to call",
      "Anzurufende Station",
      "Estación a llamar",
      "Estação a chamar",
      "Station à appeler"),
    E("All devices or exactly one - reception filter and transmitting device in one menu",
      "Alle Geräte oder genau eines - Empfangsfilter und sendendes Gerät in einem Menü",
      "Todos los dispositivos o solo uno - filtro de recepción y dispositivo emisor en un menú",
      "Todos os dispositivos ou apenas um - filtro de receção e dispositivo emissor num menu",
      "Tous les appareils ou un seul - filtre de réception et appareil émetteur dans un menu"),
    E("Device",
      "Gerät", "Dispositivo", "Dispositivo", "Appareil"),
    E("All",
      "Alle", "Todos", "Todos", "Tous"),
    E("CQ / broadcast: which device transmits",
      "CQ / Rundruf: welches Gerät sendet",
      "CQ / difusión: qué dispositivo transmite",
      "CQ / emissão: que dispositivo transmite",
      "CQ / diffusion : quel appareil émet"),
    E("Device for CQ/broadcast",
      "Gerät für CQ/Rundruf",
      "Dispositivo para CQ/difusión",
      "Dispositivo para CQ/emissão",
      "Appareil pour CQ/diffusion"),
    E("Channel",
      "Kanal", "Canal", "Canal", "Canal"),
    E("Mode",
      "Betriebsart", "Modo", "Modo", "Mode"),
    E("Terminal",
      "Terminal", "Terminal", "Terminal", "Terminal"),
    E("Mailbox",
      "Mailbox", "Buzón", "Correio", "Messagerie"),
    E("Administration",
      "Verwaltung", "Administración", "Administração", "Administration"),

    /* ---- Terminal --------------------------------------------------- */
    E("Full duplex — reception continues while transmitting.",
      "Vollduplex — der Empfang läuft während des Sendens weiter.",
      "Dúplex completo — la recepción continúa mientras se transmite.",
      "Duplexo completo — a receção continua durante a transmissão.",
      "Duplex intégral — la réception continue pendant l'émission."),
    E("Half duplex — no reception while transmitting.",
      "Halbduplex — während des Sendens wird nicht empfangen.",
      "Semidúplex — no se recibe mientras se transmite.",
      "Semiduplexo — não há receção durante a transmissão.",
      "Semi-duplex — pas de réception pendant l'émission."),
    E("Enter message &#8230;  [Enter] to send",
      "Nachricht eingeben &#8230;  [Enter] zum Senden",
      "Escribir mensaje &#8230;  [Enter] para enviar",
      "Escrever mensagem &#8230;  [Enter] para enviar",
      "Saisir un message &#8230;  [Entrée] pour envoyer"),
    E("Send",
      "Senden", "Enviar", "Enviar", "Envoyer"),

    /* ---- Mailbox view (PRTERM's own chrome around MailboxD) --------- */
    E("local mailbox",
      "lokale Mailbox", "buzón local", "correio local", "messagerie locale"),
    E("Login",
      "Anmeldung", "Acceso", "Entrada", "Connexion"),
    E("User:",
      "Benutzer:", "Usuario:", "Utilizador:", "Utilisateur :"),
    E("Password:",
      "Passwort:", "Contraseña:", "Palavra-passe:", "Mot de passe :"),
    E("Log in",
      "Anmelden", "Iniciar sesión", "Entrar", "Se connecter"),
    E("mailbox command",
      "Mailbox-Befehl", "comando del buzón", "comando de correio", "commande messagerie"),

    /* ---- Administration --------------------------------------------- */
    E("The admin area is locked.",
      "Der Administrationsbereich ist gesperrt.",
      "El área de administración está bloqueada.",
      "A área de administração está bloqueada.",
      "La zone d'administration est verrouillée."),
    E("Log in directly here in the terminal.",
      "Hier im Terminal direkt anmelden.",
      "Inicie sesión directamente aquí en el terminal.",
      "Inicie sessão diretamente aqui no terminal.",
      "Connectez-vous directement ici dans le terminal."),
    E("General",
      "Allgemein", "General", "Geral", "Général"),
    E("Name",
      "Name", "Nombre", "Nome", "Nom"),
    E("Subtitle",
      "Untertitel", "Subtítulo", "Subtítulo", "Sous-titre"),
    E("Language",
      "Sprache", "Idioma", "Idioma", "Langue"),
    E("Interface language",
      "Sprache der Oberfläche", "Idioma de la interfaz",
      "Idioma da interface", "Langue de l'interface"),
    E("Save",
      "Speichern", "Guardar", "Guardar", "Enregistrer"),
    E("Station",
      "Station", "Estación", "Estação", "Station"),
    E("base max. 6 characters + SSID \"-<digit>\", total max. 8",
      "Basis max. 6 Zeichen + SSID \"-<Ziffer>\", gesamt max. 8",
      "base máx. 6 caracteres + SSID \"-<dígito>\", total máx. 8",
      "base máx. 6 caracteres + SSID \"-<dígito>\", total máx. 8",
      "base 6 caractères max. + SSID \"-<chiffre>\", 8 au total max."),
    E("Locator",
      "Locator", "Localizador", "Locator", "Locator"),
    E("Radio",
      "Funk", "Radio", "Rádio", "Radio"),
    E("Driver",
      "Treiber", "Controlador", "Controlador", "Pilote"),
    E("Serial interface",
      "Serielle Schnittstelle", "Interfaz serie", "Interface série", "Interface série"),
    E("Baud rate",
      "Baudrate", "Velocidad (baudios)", "Velocidade (baudios)", "Débit (bauds)"),
    E("Full duplex",
      "Vollduplex", "Dúplex completo", "Duplexo completo", "Duplex intégral"),
    E("Half duplex",
      "Halbduplex", "Semidúplex", "Semiduplexo", "Semi-duplex"),
    E("Duplex",
      "Duplex", "Dúplex", "Duplexo", "Duplex"),
    E("FM/AM/SSB - checked against the channel",
      "FM/AM/SSB - wird gegen den Kanal geprüft",
      "FM/AM/SSB - se comprueba con el canal",
      "FM/AM/SSB - verificado com o canal",
      "FM/AM/SSB - vérifié par rapport au canal"),
    E("Frequency (Hz)",
      "Frequenz (Hz)", "Frecuencia (Hz)", "Frequência (Hz)", "Fréquence (Hz)"),
    E("TX power (mW)",
      "Sendeleistung (mW)", "Potencia de TX (mW)",
      "Potência de TX (mW)", "Puissance d'émission (mW)"),
    E("checked against the allocation",
      "wird gegen die Zuteilung geprüft", "se comprueba con la asignación",
      "verificado com a alocação", "vérifié par rapport à l'attribution"),
    E("Callsign",
      "Rufzeichen", "Indicativo", "Indicativo", "Indicatif"),
    E("CALLID max. length",
      "CALLID max. Länge", "CALLID longitud máx.",
      "CALLID comp. máx.", "CALLID longueur max."),
    E("CALLERID base",
      "CALLERID-Basis", "CALLERID base", "CALLERID base", "CALLERID base"),
    E("CALLERID total",
      "CALLERID gesamt", "CALLERID total", "CALLERID total", "CALLERID total"),
    E("6 + 2 = 8 as usual in radio",
      "6 + 2 = 8 wie üblich im Funk", "6 + 2 = 8 como es habitual en radio",
      "6 + 2 = 8 como é habitual no rádio", "6 + 2 = 8 comme en usage radio"),
    E("SSID digits",
      "SSID-Stellen", "Dígitos SSID", "Dígitos SSID", "Chiffres SSID"),
    E("allow SSID",
      "SSID erlauben", "permitir SSID", "permitir SSID", "autoriser SSID"),
    E("Channel selection",
      "Kanalauswahl", "Selección de canal", "Seleção de canal", "Choix du canal"),
    E("Click a channel to switch.",
      "Kanal anklicken zum Umschalten.", "Haga clic en un canal para cambiar.",
      "Clique num canal para mudar.", "Cliquez sur un canal pour changer."),
    E("Gateway",
      "Gateway", "Pasarela", "Gateway", "Passerelle"),
    E("data",
      "Daten", "datos", "dados", "données"),
    E("Device test",
      "Gerätetest", "Prueba de dispositivo", "Teste de dispositivo", "Test d'appareil"),
    E("Sends <b>an empty test carrier for 3 seconds</b> - no content, only to check antenna and TX LED.",
      "Sendet <b>eine leere Testträgerwelle für 3 Sekunden</b> - ohne Inhalt, nur um Antenne und TX-LED zu prüfen.",
      "Envía <b>una portadora de prueba vacía durante 3 segundos</b> - sin contenido, solo para comprobar la antena y el LED de TX.",
      "Envia <b>uma portadora de teste vazia durante 3 segundos</b> - sem conteúdo, apenas para verificar a antena e o LED de TX.",
      "Émet <b>une porteuse de test vide pendant 3 secondes</b> - sans contenu, juste pour vérifier l'antenne et la LED d'émission."),
    E("This is a radio transmission: it is announced first and only sent after confirmation. The transmit rules check beforehand whether the channel is clear.",
      "Dies ist eine Funksendung: sie wird zuerst angekündigt und erst nach Bestätigung gesendet. Die Senderegeln prüfen vorher, ob der Kanal frei ist.",
      "Esta es una transmisión de radio: se anuncia primero y solo se envía después de confirmar. Las reglas de transmisión comprueban antes si el canal está libre.",
      "Esta é uma transmissão de rádio: é anunciada primeiro e só enviada depois da confirmação. As regras de transmissão verificam antes se o canal está livre.",
      "Il s'agit d'une émission radio : elle est annoncée d'abord et émise seulement après confirmation. Les règles d'émission vérifient au préalable que le canal est libre."),
    E("3-second test",
      "3-Sekunden-Test", "Prueba de 3 segundos", "Teste de 3 segundos", "Test de 3 secondes"),
    E("Blocked stations",
      "Gesperrte Stationen", "Estaciones bloqueadas", "Estações bloqueadas", "Stations bloquées"),
    E("Pattern",
      "Muster", "Patrón", "Padrão", "Motif"),
    E("Reason",
      "Grund", "Motivo", "Motivo", "Raison"),
    E("Remove",
      "Entfernen", "Quitar", "Remover", "Retirer"),
    E("no blocks",
      "keine Sperren", "sin bloqueos", "sem bloqueios", "aucun blocage"),
    E("wildcards * and ?",
      "Platzhalter * und ?", "comodines * y ?", "curingas * e ?", "jokers * et ?"),
    E("Block",
      "Sperren", "Bloquear", "Bloquear", "Bloquer"),
    E("Font &amp; display",
      "Schrift &amp; Darstellung", "Fuente &amp; pantalla",
      "Letra &amp; ecrã", "Police &amp; affichage"),
    E("Font file",
      "Schriftdatei", "Archivo de fuente", "Ficheiro de letra", "Fichier de police"),
    E(".otf or .ttf, relative to prterm.ini",
      ".otf oder .ttf, relativ zu prterm.ini", ".otf o .ttf, relativo a prterm.ini",
      ".otf ou .ttf, relativo a prterm.ini", ".otf ou .ttf, relatif à prterm.ini"),
    E("Font size (px)",
      "Schriftgröße (px)", "Tamaño de fuente (px)",
      "Tamanho da letra (px)", "Taille de police (px)"),
    E("Line height (%)",
      "Zeilenhöhe (%)", "Altura de línea (%)", "Altura da linha (%)", "Hauteur de ligne (%)"),
    E("120 = 1.2",
      "120 = 1.2", "120 = 1.2", "120 = 1.2", "120 = 1.2"),
    E("Density",
      "Dichte", "Densidad", "Densidade", "Densité"),
    E("Compact (maximum text space)",
      "Kompakt (maximaler Textplatz)", "Compacto (máximo espacio de texto)",
      "Compacto (máximo espaço de texto)", "Compact (maximum d'espace texte)"),
    E("Normal",
      "Normal", "Normal", "Normal", "Normal"),
    E("Silver (default)",
      "Silber (Standard)", "Plata (predeterminado)", "Prata (predefinido)", "Argent (par défaut)"),
    E("Dark",
      "Dunkel", "Oscuro", "Escuro", "Sombre"),
    E("Color scheme",
      "Farbschema", "Esquema de colores", "Esquema de cores", "Jeu de couleurs"),
    E("Security",
      "Sicherheit", "Seguridad", "Segurança", "Sécurité"),
    E("Old password",
      "Altes Passwort", "Contraseña anterior", "Palavra-passe antiga", "Ancien mot de passe"),
    E("New password",
      "Neues Passwort", "Contraseña nueva", "Nova palavra-passe", "Nouveau mot de passe"),
    E("Repeat",
      "Wiederholen", "Repetir", "Repetir", "Répéter"),
    E("allow transmitting without login",
      "Senden ohne Anmeldung erlauben", "permitir transmitir sin iniciar sesión",
      "permitir transmitir sem iniciar sessão", "autoriser l'émission sans connexion"),
    E("Change password",
      "Passwort ändern", "Cambiar contraseña", "Alterar palavra-passe", "Changer le mot de passe"),
    E("Configuration (prterm.ini)",
      "Konfiguration (prterm.ini)", "Configuración (prterm.ini)",
      "Configuração (prterm.ini)", "Configuration (prterm.ini)"),
    E("Session",
      "Sitzung", "Sesión", "Sessão", "Session"),
    E("Logged in as",
      "Angemeldet als", "Conectado como", "Sessão iniciada como", "Connecté en tant que"),
    E("Log out",
      "Abmelden", "Cerrar sesión", "Terminar sessão", "Se déconnecter"),

    /* ---- Login dialog ----------------------------------------------- */
    E("User",
      "Benutzer", "Usuario", "Utilizador", "Utilisateur"),
    E("Password",
      "Passwort", "Contraseña", "Palavra-passe", "Mot de passe"),
    E("Cancel",
      "Abbrechen", "Cancelar", "Cancelar", "Annuler"),

    /* ---- Browser script --------------------------------------------- */
    E("FULL-DUPLEX",
      "VOLL-DUPLEX", "DÚPLEX COMPLETO", "DUPLEXO COMPLETO", "DUPLEX INTÉGRAL"),
    E("HALF-DUPLEX",
      "HALB-DUPLEX", "SEMIDÚPLEX", "SEMIDUPLEXO", "SEMI-DUPLEX"),
    E("MailboxD is not connected — the daemon is not linked yet.",
      "MailboxD ist nicht verbunden — der Daemon ist noch nicht angebunden.",
      "MailboxD no está conectado — el demonio aún no está enlazado.",
      "MailboxD não está ligado — o daemon ainda não está ligado.",
      "MailboxD n'est pas connecté — le démon n'est pas encore relié."),
    E("Please address a station — broadcast only under \"All\".",
      "Bitte eine Station ansprechen — Rundruf nur unter \"Alle\".",
      "Indique una estación — la difusión solo bajo \"Todos\".",
      "Indique uma estação — emissão apenas em \"Todos\".",
      "Veuillez adresser une station — diffusion seulement sous \"Tous\"."),
    E("sending failed",
      "Senden fehlgeschlagen", "error al enviar", "falha ao enviar", "échec de l'envoi"),
    E("Checking …",
      "Prüfe …", "Comprobando …", "A verificar …", "Vérification …"),
    E("Sending …",
      "Sende …", "Enviando …", "A enviar …", "Émission …"),
    E("test rejected",
      "Test abgelehnt", "prueba rechazada", "teste rejeitado", "test refusé"),
    E("TX in %s seconds",
      "TX in %s Sekunden", "TX en %s segundos", "TX em %s segundos", "Émission dans %s secondes"),
    E("Test finished.",
      "Test beendet.", "Prueba terminada.", "Teste concluído.", "Test terminé."),
    E("test failed",
      "Test fehlgeschlagen", "prueba fallida", "teste falhado", "test échoué"),
    E("enter user and password",
      "Benutzer und Passwort eingeben", "introducir usuario y contraseña",
      "introduzir utilizador e palavra-passe", "saisir utilisateur et mot de passe"),
    E("form incomplete — please reload",
      "Formular unvollständig — bitte neu laden", "formulario incompleto — recargue por favor",
      "formulário incompleto — recarregue por favor", "formulaire incomplet — veuillez recharger"),
    E("login failed",
      "Anmeldung fehlgeschlagen", "error de inicio de sesión",
      "falha ao iniciar sessão", "échec de la connexion"),
    E("connected",
      "verbunden", "conectado", "ligado", "connecté"),
    E("disconnected",
      "getrennt", "desconectado", "desligado", "déconnecté"),

    /* ---- Messages of the request layer (flash / json error) -------- */
    E("no configuration loaded",
      "keine Konfiguration geladen", "no se ha cargado la configuración",
      "nenhuma configuração carregada", "aucune configuration chargée"),
    E("CALLERID is invalid (base max. 6 characters, SSID \"-<digit>\", total max. 8)",
      "CALLERID ist ungültig (Basis max. 6 Zeichen, SSID \"-<Ziffer>\", gesamt max. 8)",
      "CALLERID no es válido (base máx. 6 caracteres, SSID \"-<dígito>\", total máx. 8)",
      "CALLERID inválido (base máx. 6 caracteres, SSID \"-<dígito>\", total máx. 8)",
      "CALLERID invalide (base 6 caractères max., SSID \"-<chiffre>\", 8 au total max.)"),
    E("unknown rig driver",
      "unbekannter Rig-Treiber", "controlador de radio desconocido",
      "controlador de rádio desconhecido", "pilote radio inconnu"),
    E("duplex must be \"full\" or \"half\"",
      "duplex muss \"full\" oder \"half\" sein", "duplex debe ser \"full\" o \"half\"",
      "duplex deve ser \"full\" ou \"half\"", "duplex doit être \"full\" ou \"half\""),
    E("mode must be \"fm\", \"am\" or \"ssb\"",
      "mode muss \"fm\", \"am\" oder \"ssb\" sein", "mode debe ser \"fm\", \"am\" o \"ssb\"",
      "mode deve ser \"fm\", \"am\" ou \"ssb\"", "mode doit être \"fm\", \"am\" ou \"ssb\""),
    E("callsign rules outside the allowed limits",
      "Rufzeichenregeln außerhalb der erlaubten Grenzen",
      "las reglas de indicativo superan los límites permitidos",
      "as regras de indicativo estão fora dos limites permitidos",
      "les règles d'indicatif dépassent les limites autorisées"),
    E("total length must not be smaller than the base",
      "die Gesamtlänge darf nicht kleiner als die Basis sein",
      "la longitud total no puede ser menor que la base",
      "o comprimento total não pode ser menor que a base",
      "la longueur totale ne peut pas être inférieure à la base"),
    E("the new rule makes your own CALLERID invalid",
      "die neue Regel macht das eigene CALLERID ungültig",
      "la nueva regla hace inválido su propio CALLERID",
      "a nova regra torna o seu CALLERID inválido",
      "la nouvelle règle rend votre propre CALLERID invalide"),
    E("font file must be .ttf, .otf, .woff or .woff2",
      "die Schriftdatei muss .ttf, .otf, .woff oder .woff2 sein",
      "el archivo de fuente debe ser .ttf, .otf, .woff o .woff2",
      "o ficheiro de letra deve ser .ttf, .otf, .woff ou .woff2",
      "le fichier de police doit être .ttf, .otf, .woff ou .woff2"),
    E("font size must be between 6 and 96",
      "die Schriftgröße muss zwischen 6 und 96 liegen",
      "el tamaño de fuente debe estar entre 6 y 96",
      "o tamanho da letra deve estar entre 6 e 96",
      "la taille de police doit être comprise entre 6 et 96"),
    E("line height must be between 100% and 300%",
      "die Zeilenhöhe muss zwischen 100% und 300% liegen",
      "la altura de línea debe estar entre 100% y 300%",
      "a altura da linha deve estar entre 100% e 300%",
      "la hauteur de ligne doit être comprise entre 100% et 300%"),
    E("density must be \"compact\" or \"normal\"",
      "density muss \"compact\" oder \"normal\" sein",
      "density debe ser \"compact\" o \"normal\"",
      "density deve ser \"compact\" ou \"normal\"",
      "density doit être \"compact\" ou \"normal\""),
    E("theme must be \"silver\" or \"dark\"",
      "theme muss \"silver\" oder \"dark\" sein",
      "theme debe ser \"silver\" o \"dark\"",
      "theme deve ser \"silver\" ou \"dark\"",
      "theme doit être \"silver\" ou \"dark\""),
    E("pattern must not be empty",
      "das Muster darf nicht leer sein", "el patrón no puede estar vacío",
      "o padrão não pode estar vazio", "le motif ne peut pas être vide"),
    E("pattern too long",
      "Muster zu lang", "patrón demasiado largo",
      "padrão demasiado comprido", "motif trop long"),
    E("no such ban entry",
      "kein solcher Sperr-Eintrag", "no existe esa entrada de bloqueo",
      "não existe essa entrada de bloqueio", "aucune telle entrée de blocage"),
    E("old password is wrong",
      "das alte Passwort ist falsch", "la contraseña anterior es incorrecta",
      "a palavra-passe antiga está errada", "l'ancien mot de passe est incorrect"),
    E("new password must have at least 8 characters",
      "das neue Passwort muss mindestens 8 Zeichen haben",
      "la contraseña nueva debe tener al menos 8 caracteres",
      "a nova palavra-passe deve ter pelo menos 8 caracteres",
      "le nouveau mot de passe doit comporter au moins 8 caractères"),
    E("password repetition does not match",
      "die Passwortwiederholung stimmt nicht überein",
      "la repetición de la contraseña no coincide",
      "a repetição da palavra-passe não coincide",
      "la répétition du mot de passe ne correspond pas"),
    E("hash could not be created",
      "der Hash konnte nicht erzeugt werden", "no se pudo crear el hash",
      "não foi possível criar o hash", "impossible de créer l'empreinte"),
    E("no action",
      "keine Aktion", "ninguna acción", "nenhuma ação", "aucune action"),
    E("unknown action",
      "unbekannte Aktion", "acción desconocida", "ação desconhecida", "action inconnue"),
    E("session expired or invalid token",
      "Sitzung abgelaufen oder ungültiges Token", "sesión caducada o token no válido",
      "sessão expirada ou token inválido", "session expirée ou jeton invalide"),
    E("broadcast is only allowed under \"All\" - with a single device please address a station",
      "Rundruf ist nur unter \"Alle\" erlaubt - bei einem einzelnen Gerät bitte eine Station ansprechen",
      "la difusión solo se permite bajo \"Todos\" - con un solo dispositivo indique una estación",
      "a emissão só é permitida em \"Todos\" - com um único dispositivo indique uma estação",
      "la diffusion n'est autorisée que sous \"Tous\" - avec un seul appareil, veuillez adresser une station"),
    E("this driver does not support a test carrier",
      "dieser Treiber unterstützt keinen Testträger", "este controlador no admite portadora de prueba",
      "este controlador não suporta portadora de teste", "ce pilote ne prend pas en charge la porteuse de test"),
    E("too many failed attempts - please try again later",
      "zu viele Fehlversuche - bitte später erneut versuchen",
      "demasiados intentos fallidos - inténtelo de nuevo más tarde",
      "demasiadas tentativas falhadas - tente novamente mais tarde",
      "trop de tentatives échouées - veuillez réessayer plus tard"),
    E("login required",
      "Anmeldung erforderlich", "se requiere iniciar sesión",
      "é necessário iniciar sessão", "connexion requise"),
    E("no rig connected",
      "kein Gerät verbunden", "ningún equipo conectado",
      "nenhum equipamento ligado", "aucun équipement connecté"),
    E("monitor mode: transmitting is locked",
      "Monitorbetrieb: Senden ist gesperrt", "modo monitor: la transmisión está bloqueada",
      "modo monitor: a transmissão está bloqueada", "mode monitor : l'émission est bloquée"),
    E("CALLERID is banned",
      "CALLERID ist gesperrt", "CALLERID está bloqueado",
      "CALLERID está bloqueado", "CALLERID est bloqué"),
    E("transmitting requires login",
      "zum Senden ist eine Anmeldung nötig", "para transmitir se requiere iniciar sesión",
      "para transmitir é necessário iniciar sessão", "l'émission nécessite une connexion"),

    /* ---- Band plan and device drivers ------------------------------- */
    E("no band plan active",
      "kein Bandplan aktiv", "ningún plan de banda activo",
      "nenhum plano de banda ativo", "aucun plan de bandes actif"),
    E("invalid frequency",
      "ungültige Frequenz", "frecuencia no válida",
      "frequência inválida", "fréquence invalide"),
    E("frequency %ld Hz is not on an allocated channel (%s)",
      "Frequenz %ld Hz liegt nicht auf einem zugewiesenen Kanal (%s)",
      "la frecuencia %ld Hz no está en un canal asignado (%s)",
      "a frequência %ld Hz não está num canal atribuído (%s)",
      "la fréquence %ld Hz n'est pas sur un canal attribué (%s)"),
    E("channel %d (%ld Hz): only FM/PM allowed (national extension range), not %s",
      "Kanal %d (%ld Hz): nur FM/PM erlaubt (nationale Erweiterung), nicht %s",
      "canal %d (%ld Hz): solo se permite FM/PM (extensión nacional), no %s",
      "canal %d (%ld Hz): apenas FM/PM permitido (extensão nacional), não %s",
      "canal %d (%ld Hz) : seul FM/PM est autorisé (extension nationale), pas %s"),
    E("channel %d (%ld Hz): %s is not allowed, allowed are %s",
      "Kanal %d (%ld Hz): %s ist nicht erlaubt, erlaubt sind %s",
      "canal %d (%ld Hz): %s no está permitido, permitidos son %s",
      "canal %d (%ld Hz): %s não é permitido, permitidos são %s",
      "canal %d (%ld Hz) : %s n'est pas autorisé, autorisés : %s"),
    E("channel not found",
      "Kanal nicht gefunden", "canal no encontrado",
      "canal não encontrado", "canal introuvable"),
    E("no power limit for channel %d / %s",
      "keine Leistungsgrenze für Kanal %d / %s", "sin límite de potencia para el canal %d / %s",
      "sem limite de potência para o canal %d / %s", "aucune limite de puissance pour le canal %d / %s"),
    E("power %ld mW exceeds %ld mW (channel %d, %s)",
      "Leistung %ld mW überschreitet %ld mW (Kanal %d, %s)",
      "la potencia %ld mW supera %ld mW (canal %d, %s)",
      "a potência %ld mW excede %ld mW (canal %d, %s)",
      "la puissance %ld mW dépasse %ld mW (canal %d, %s)"),
    E("TNC not connected",
      "TNC nicht verbunden", "TNC no conectado", "TNC não ligado", "TNC non connecté"),
    E("empty message",
      "leere Nachricht", "mensaje vacío", "mensagem vazia", "message vide"),
    E("frame could not be built",
      "Rahmen konnte nicht gebaut werden", "no se pudo construir la trama",
      "não foi possível construir a trama", "impossible de construire la trame"),
    E("KISS frame too large",
      "KISS-Rahmen zu groß", "trama KISS demasiado grande",
      "trama KISS demasiado grande", "trame KISS trop grande"),
    E("message too long",
      "Nachricht zu lang", "mensaje demasiado largo",
      "mensagem demasiado comprida", "message trop long"),
    E("test frame could not be built",
      "Testrahmen konnte nicht gebaut werden", "no se pudo construir la trama de prueba",
      "não foi possível construir a trama de teste", "impossible de construire la trame de test"),
    E("duration must be between 1 and 10 seconds",
      "die Dauer muss zwischen 1 und 10 Sekunden liegen",
      "la duración debe estar entre 1 y 10 segundos",
      "a duração deve estar entre 1 e 10 segundos",
      "la durée doit être comprise entre 1 et 10 secondes"),
    E("line format \"%s\" is invalid (e.g. 8n1)",
      "Zeilenformat \"%s\" ist ungültig (z. B. 8n1)",
      "el formato de línea \"%s\" no es válido (p. ej. 8n1)",
      "o formato de linha \"%s\" não é válido (p. ex. 8n1)",
      "le format de ligne \"%s\" n'est pas valide (p. ex. 8n1)"),
    E("simulation not connected",
      "Simulation nicht verbunden", "simulación no conectada",
      "simulação não ligada", "simulation non connectée"),
    E("simulation not open",
      "Simulation nicht offen", "simulación no abierta",
      "simulação não aberta", "simulation non ouverte"),
    E("TX in %d seconds - empty test carrier",
      "TX in %d Sekunden - leere Testträgerwelle", "TX en %d segundos - portadora de prueba vacía",
      "TX em %d segundos - portadora de teste vazia", "Émission dans %d secondes - porteuse de test vide"),
    E("unknown channel %s",
      "unbekannter Kanal %s", "canal desconocido %s",
      "canal desconhecido %s", "canal inconnu %s"),
    E("unknown mode",
      "unbekannte Betriebsart", "modo desconocido",
      "modo desconhecido", "mode inconnu"),
    E("%.3f MHz is not on an allocated channel",
      "%.3f MHz liegt nicht auf einem zugewiesenen Kanal", "%.3f MHz no está en un canal asignado",
      "%.3f MHz não está num canal atribuído", "%.3f MHz n'est pas sur un canal attribué"),
    E("Simulation (no hardware)",
      "Simulation (ohne Hardware)", "Simulación (sin hardware)",
      "Simulação (sem hardware)", "Simulation (sans matériel)"),
    E("TNC2 class (Landolt TNC2C, PK-TNC2) via KISS",
      "TNC2-Klasse (Landolt TNC2C, PK-TNC2) über KISS", "Clase TNC2 (Landolt TNC2C, PK-TNC2) por KISS",
      "Classe TNC2 (Landolt TNC2C, PK-TNC2) por KISS", "Classe TNC2 (Landolt TNC2C, PK-TNC2) via KISS"),
    E("simulation started (full duplex: reception continues while transmitting)",
      "Simulation gestartet (Vollduplex: der Empfang läuft während des Sendens weiter)",
      "simulación iniciada (dúplex completo: la recepción continúa mientras se transmite)",
      "simulação iniciada (duplexo completo: a receção continua durante a transmissão)",
      "simulation démarrée (duplex intégral : la réception continue pendant l'émission)"),
    E("simulation started (half duplex: no reception while transmitting)",
      "Simulation gestartet (Halbduplex: während des Sendens wird nicht empfangen)",
      "simulación iniciada (semidúplex: no se recibe mientras se transmite)",
      "simulação iniciada (semiduplexo: não há receção durante a transmissão)",
      "simulation démarrée (semi-duplex : pas de réception pendant l'émission)"),
    E("[empty test carrier]",
      "[leere Testträgerwelle]", "[portadora de prueba vacía]",
      "[portadora de teste vazia]", "[porteuse de test vide]"),
    E("the admin area is locked",
      "der Administrationsbereich ist gesperrt", "el área de administración está bloqueada",
      "a área de administração está bloqueada", "la zone d'administration est verrouillée"),
    E("user or password is wrong",
      "Benutzer oder Passwort ist falsch", "el usuario o la contraseña son incorrectos",
      "o utilizador ou a palavra-passe estão errados", "l'utilisateur ou le mot de passe est incorrect"),
    E("out of memory",
      "Speicher voll", "memoria agotada", "memória esgotada", "mémoire insuffisante"),
    E("Set a password",
      "Passwort festlegen", "Establecer una contraseña", "Definir uma palavra-passe", "Définir un mot de passe"),
    E("The built-in password is active - the administration is open to everyone who knows it.",
      "Das eingebaute Passwort ist aktiv - die Verwaltung ist für alle offen, die es kennen.",
      "La contraseña incorporada está activa: la administración está abierta para quien la conozca.",
      "A palavra-passe incorporada está ativa: a administração está aberta a quem a conhecer.",
      "Le mot de passe intégré est actif : l'administration est ouverte à tous ceux qui le connaissent."),
    E("Set your own password under Security - the built-in one is only for the first installation.",
      "Unter Sicherheit das eigene Passwort festlegen - das eingebaute gilt nur für die erste Installation.",
      "Establezca su propia contraseña en Seguridad: la incorporada solo sirve para la primera instalación.",
      "Defina a sua própria palavra-passe em Segurança: a incorporada serve apenas para a primeira instalação.",
      "Définissez votre propre mot de passe sous Sécurité — celui intégré n'est que pour la première installation."),
};
#define CATALOG_COUNT (sizeof catalog / sizeof catalog[0])

/* ======================================================================= */
/* Lookup                                                                  */
/* ======================================================================= */

/* Column of a language code, English for everything unknown. */
static size_t lang_col(const char *lang)
{
    if (lang != NULL) {
        for (size_t i = 0; i < LANG_COUNT; i++) {
            if (pr_str_eq_ci(lang, langs[i].code))
                return i;
        }
    }
    return 0;
}

const char *pr_tr(const char *lang, const char *text)
{
    if (text == NULL)
        return "";

    size_t col = lang_col(lang);
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (strcmp(catalog[i].col[0], text) != 0)
            continue;

        const char *t = catalog[i].col[col];
        if (t != NULL && t[0] != '\0')
            return t;
        return text;            /* empty entry: stay with the key  */
    }
    return text;                /* unknown text: stay English      */
}

bool pr_lang_supported(const char *lang)
{
    if (lang == NULL)
        return false;
    for (size_t i = 0; i < LANG_COUNT; i++) {
        if (pr_str_eq_ci(lang, langs[i].code))
            return true;
    }
    return false;
}

/* ======================================================================= */
/* Language of the current request                                         */
/* ======================================================================= */

/*
 * Layers like the band plan or the device drivers produce messages
 * without a configuration at hand. The language of the request is
 * therefore set once per request and used by pr_trs()/pr_trf().
 */
static char g_lang[8] = "en";

void pr_lang_set(const char *lang)
{
    if (pr_lang_supported(lang))
        pr_strlcpy(g_lang, lang, sizeof g_lang);
    else
        pr_strlcpy(g_lang, "en", sizeof g_lang);
}

const char *pr_trs(const char *text)
{
    return pr_tr(g_lang, text);
}

int pr_trf(char *dst, size_t dstlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    /*
     * The format string is a CATALOG entry, not attacker input - the
     * translations are written together with the code and kept in the
     * same order of placeholders. The non-literal format check cannot
     * see that, so it is switched off exactly here and nowhere else.
     */
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int n = vsnprintf(dst, dstlen, pr_tr(g_lang, fmt), ap);
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

    va_end(ap);
    return n;
}

size_t pr_lang_count(void)
{
    return LANG_COUNT;
}

const char *pr_lang_code(size_t idx)
{
    return idx < LANG_COUNT ? langs[idx].code : "en";
}

const char *pr_lang_name(size_t idx)
{
    return idx < LANG_COUNT ? langs[idx].name : "English";
}
