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
