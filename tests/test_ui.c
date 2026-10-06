/*
 * PRTERM - Test: page composition (the terminal UI)
 *
 * Locks the layout of the terminal against regressions - rendered
 * exactly as the CGI renders it, no browser needed:
 *
 *   - top bar: CALL: and the RX/TX: menu, under "All" the CQ: menu
 *   - device entries in the form <devicename>@<freq>@<baud>Baud
 *   - the send bar carries message line, Send and the character grid -
 *     and nothing that lives elsewhere
 *   - FM/AM/SSB and duplex are administration settings
 *   - the Mailbox tab exists only when MailboxD is enabled
 *   - the interface speaks the big five (en, de, es, pt, fr) and an
 *     unknown language falls back to English
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "config.h"
#include "html.h"
#include "lang.h"
#include "pages.h"
#include "radio.h"
#include "session.h"
#include "testutil.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TEST_DIR "test-ui.runtime"
#define TEST_INI TEST_DIR "/prterm.ini"

static bool write_ini(void)
{
    FILE *f = fopen(TEST_INI, "w");
    if (f == NULL)
        return false;

    fprintf(f,
        "[site]\n"
        "name = Test\n\n"
        "[radio]\n"
        "duplex = full\n"
        "driver = tnc2\n"
        "baud = 19200\n"
        "radio_baud = 2400\n"
        "line = 8n1\n"
        "kiss_init = esc\n"
        "freq_hz = 27235000\n"
        "mode = am\n\n"
        "[station:tnc2c]\n"
        "driver = tnc2\n"
        "baud = 19200\n"
        "radio_baud = 2400\n"
        "line = 8n1\n"
        "callerid = TEST-1\n"
        "enabled = true\n\n"
        "[station:pktn2c]\n"
        "driver = tnc2\n"
        "baud = 9600\n"
        "radio_baud = 1200\n"
        "line = 8n1\n"
        "callerid = TEST-2\n"
        "enabled = true\n\n"
        "[station:off1]\n"
        "driver = tnc2\n"
        "baud = 9600\n"
        "radio_baud = 1200\n"
        "callerid = TEST-3\n"
        "enabled = false\n\n"
        "[paths]\n"
        "runtime_dir = %s\n",
        TEST_DIR);

    fclose(f);
    return true;
}

/*
 * Render the page in one language and look for two markers: the label
 * of the Send button and the heading of the administration. That is
 * enough to tell the five languages - and English - apart.
 */
static void check_language(pr_config *cfg, const pr_session *sess,
                           const pr_rig_state *st, const char *code,
                           const char *send_word, const char *admin_word)
{
    pr_strlcpy(cfg->language, code, sizeof cfg->language);

    pr_buf out;
    pr_buf_init(&out);
    page_render(&out, cfg, sess, st, NULL, 0, NULL, NULL);
    const char *html = out.data != NULL ? out.data : "";

    char send_btn[80], admin_h2[80];
    snprintf(send_btn, sizeof send_btn, ">%s</button>", send_word);
    snprintf(admin_h2, sizeof admin_h2, "<h2 class=\"grad\">%s</h2>", admin_word);

    pr_test_checks++;
    bool ok = strstr(html, send_btn) != NULL && strstr(html, admin_h2) != NULL;
    printf("  %s language %s: %s / %s (%s:%d)\n",
           ok ? "ok  " : "FAIL", code, send_word, admin_word,
           __FILE__, __LINE__);
    if (!ok) {
        pr_test_fails++;
        printf("       want \"%s\" and \"%s\"\n", send_btn, admin_h2);
    }
    pr_buf_free(&out);
}

int main(void)
{
    char err[256];

    printf("== Page composition ==\n");

    (void)mkdir(TEST_DIR, 0777);
    CHECK(write_ini());

    pr_config cfg;
    CHECK(pr_config_load(&cfg, TEST_INI, err, sizeof err) == 0);

    pr_rig_state st;
    memset(&st, 0, sizeof st);
    st.freq_hz = cfg.freq_hz;
    st.mode    = cfg.mode;
    st.duplex  = cfg.duplex;
    st.link_ok = true;
    pr_strlcpy(st.status, "running", sizeof st.status);

    pr_session guest;
    memset(&guest, 0, sizeof guest);

    pr_buf out;
    pr_buf_init(&out);
    page_render(&out, &cfg, &guest, &st, NULL, 0, NULL, NULL);
    const char *html = out.data != NULL ? out.data : "";
    CHECK(out.data != NULL);

    /* ---- Top bar: CALL: and RX/TX: --------------------------------- */
    CHECK(strstr(html, ">CALL:</label>") != NULL);
    CHECK(strstr(html, "for=\"callto\"") != NULL);
    CHECK(strstr(html, "id=\"callto\"") != NULL);
    CHECK(strstr(html, ">RX/TX:</label>") != NULL);
    CHECK(strstr(html, "for=\"rxtx\"") != NULL);
    CHECK(strstr(html, "id=\"rxtx\"") != NULL);

    /* ---- CQ menu: only the devices, same format -------------------- */
    CHECK(strstr(html, ">CQ:</label>") != NULL);
    CHECK(strstr(html, "id=\"cqlbl\"") != NULL);
    CHECK(strstr(html, "id=\"cqdev\"") != NULL);

    /* ---- Entries: <devicename>@<freq>@<baud>Baud -------------------- */
    CHECK(strstr(html, "<option value=\"\" selected>All</option>") != NULL);
    CHECK(strstr(html, "<option value=\"tnc2c\">tnc2c@27.235@2400Baud</option>")
          != NULL);
    CHECK(strstr(html, "<option value=\"pktn2c\">pktn2c@27.235@1200Baud</option>")
          != NULL);
    /* switched-off stations are not offered */
    CHECK(strstr(html, "off1@") == NULL);

    /* ---- Send bar: message line, Send, character grid --------------- */
    CHECK(strstr(html, "id=\"txtext\"") != NULL);
    CHECK(strstr(html, ">Send</button>") != NULL);
    CHECK(strstr(html, "id=\"gridinfo\"") != NULL);

    /* ---- Navigation on the right ----------------------------------- */
    CHECK(strstr(html, "data-goto=\"terminal\"") != NULL);
    CHECK(strstr(html, "data-goto=\"admin\"") != NULL);
    /* the Mailbox tab only exists when MailboxD is enabled             */
    CHECK(strstr(html, "data-goto=\"mailbox\"") == NULL);

    /* ---- Gone for good --------------------------------------------- */
    CHECK(strstr(html, "data-station=") == NULL);   /* station tabs     */
    CHECK(strstr(html, "id=\"txdev\"") == NULL);    /* second TX menu   */
    CHECK(strstr(html, "id=\"selmode\"") == NULL);  /* FM/AM/SSB        */
    CHECK(strstr(html, "id=\"selduplex\"") == NULL);/* duplex choice    */

    /* ---- With MailboxD enabled ------------------------------------- */
    cfg.mailboxd_enabled = true;
    pr_buf_free(&out);
    pr_buf_init(&out);
    page_render(&out, &cfg, &guest, &st, NULL, 0, NULL, NULL);
    html = out.data != NULL ? out.data : "";
    CHECK(strstr(html, "data-goto=\"mailbox\"") != NULL);
    CHECK(strstr(html, "data-view=\"mailbox\"") != NULL);
    cfg.mailboxd_enabled = false;

    /* ---- Administration: the settings that left the terminal ------- */
    pr_session admin;
    memset(&admin, 0, sizeof admin);
    admin.valid = true;
    pr_strlcpy(admin.user, "admin", sizeof admin.user);
    pr_strlcpy(admin.csrf, "0123456789abcdef", sizeof admin.csrf);

    pr_buf_free(&out);
    pr_buf_init(&out);
    page_render(&out, &cfg, &admin, &st, NULL, 0, NULL, NULL);
    html = out.data != NULL ? out.data : "";

    CHECK(strstr(html, "<h2 class=\"grad\">Radio</h2>") != NULL);
    CHECK(strstr(html, "<select name=\"duplex\">") != NULL);
    CHECK(strstr(html, "<select name=\"mode\">") != NULL);
    CHECK(strstr(html, "value=\"fm\"") != NULL);
    CHECK(strstr(html, "value=\"am\"") != NULL);
    CHECK(strstr(html, "value=\"ssb\"") != NULL);
    /* but still not in the send bar                                  */
    CHECK(strstr(html, "id=\"selmode\"") == NULL);
    CHECK(strstr(html, "id=\"selduplex\"") == NULL);

    /* locked without a session                                      */
    pr_buf_free(&out);
    pr_buf_init(&out);
    page_render(&out, &cfg, &guest, &st, NULL, 0, NULL, NULL);
    html = out.data != NULL ? out.data : "";
    CHECK(strstr(html, "admin area is locked") != NULL);
    CHECK(strstr(html, "<select name=\"mode\">") == NULL);

    /* ---- Languages: the big five ----------------------------------- */
    printf("\n== Languages ==\n");
    CHECK_INT(pr_lang_count(), 5);
    CHECK(pr_lang_supported("en") && pr_lang_supported("DE") &&
          pr_lang_supported("Es") && pr_lang_supported("pt") &&
          pr_lang_supported("fr"));
    CHECK(!pr_lang_supported("xx") && !pr_lang_supported(NULL));
    CHECK_STR(pr_tr("de", "Send"), "Senden");
    CHECK_STR(pr_tr("es", "Send"), "Enviar");
    CHECK_STR(pr_tr("pt", "Send"), "Enviar");
    CHECK_STR(pr_tr("fr", "Send"), "Envoyer");
    CHECK_STR(pr_tr("xx", "Send"), "Send");       /* unknown: English  */
    CHECK_STR(pr_tr("de", "not in the catalog"), "not in the catalog");

    /* and the messages of the request layer too                     */
    CHECK_STR(pr_tr("de", "no rig connected"), "kein Gerät verbunden");
    CHECK_STR(pr_tr("fr", "login required"), "connexion requise");
    CHECK_STR(pr_tr("es", "CALLERID is banned"), "CALLERID está bloqueado");
    CHECK_STR(pr_tr("pt", "transmitting requires login"),
              "para transmitir é necessário iniciar sessão");

    /* second marker: the heading "General" of the administration     */
    check_language(&cfg, &admin, &st, "de", "Senden", "Allgemein");
    check_language(&cfg, &admin, &st, "es", "Enviar", "General");
    check_language(&cfg, &admin, &st, "pt", "Enviar", "Geral");
    check_language(&cfg, &admin, &st, "fr", "Envoyer", "Général");
    check_language(&cfg, &admin, &st, "en", "Send", "General");
    check_language(&cfg, &admin, &st, "xx", "Send", "General");

    /* the selection offers exactly the languages PRTERM ships, and the
     * translations travel with the page for the browser script */
    {
        static const char *const codes[] = { "en", "de", "es", "pt", "fr" };

        pr_strlcpy(cfg.language, "en", sizeof cfg.language);
        pr_buf_free(&out);
        pr_buf_init(&out);
        page_render(&out, &cfg, &admin, &st, NULL, 0, NULL, NULL);
        html = out.data != NULL ? out.data : "";
        CHECK(strstr(html, "<select name=\"language\">") != NULL);
        for (size_t i = 0; i < 5; i++) {
            char opt[64];
            snprintf(opt, sizeof opt, "<option value=\"%s\"", codes[i]);
            CHECK(strstr(html, opt) != NULL);
        }
        CHECK(strstr(html, "var PRTERM_L={") != NULL);

        pr_strlcpy(cfg.language, "de", sizeof cfg.language);
        pr_buf_free(&out);
        pr_buf_init(&out);
        page_render(&out, &cfg, &admin, &st, NULL, 0, NULL, NULL);
        html = out.data != NULL ? out.data : "";
        CHECK(strstr(html, "Senden fehlgeschlagen") != NULL);   /* JS map */
    }

    pr_buf_free(&out);
    pr_config_free(&cfg);
    TEST_SUMMARY("ui");
}
