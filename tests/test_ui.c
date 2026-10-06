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
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "config.h"
#include "html.h"
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

    pr_buf_free(&out);
    pr_config_free(&cfg);
    TEST_SUMMARY("ui");
}
