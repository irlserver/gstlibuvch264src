#include "quirks.h"
#include "gstlibuvch264src_internal.h"

guint32 quirks_lookup_in(const uvc_quirk_entry_t *table, gsize n,
                         guint16 vid, guint16 pid) {
    if (table == NULL) {
        return 0;
    }
    for (gsize i = 0; i < n; i++) {
        if (table[i].vid == vid && table[i].pid == pid) {
            return table[i].flags;
        }
    }
    return 0;
}

/* Production quirk table. SHIPS EMPTY: no device currently needs a workaround,
 * so uvc_quirks_lookup() returns 0 for every camera and negotiation is
 * byte-for-byte unchanged. To enable a quirk, add ONE row here with a comment
 * citing the device and the upstream issue, e.g.
 *
 *   { 0x2ca3, 0x001f, QUIRK_DOUBLE_PROBE },  // DJI Osmo Action, libuvc #242
 *
 * and nothing else changes - negotiate() already branches on the flags. */
static const uvc_quirk_entry_t g_uvc_quirk_table[] = {
    /* intentionally empty */
};

#ifdef LIBUVCH264SRC_TESTING
/* Test override (A14). NULL restores the production table above. Compiled only
 * into the test targets that define LIBUVCH264SRC_TESTING. */
static const uvc_quirk_entry_t *g_quirk_test_table = NULL;
static gsize g_quirk_test_table_len = 0;

void uvc_quirks_set_test_table(const uvc_quirk_entry_t *table, gsize n) {
    g_quirk_test_table = table;
    g_quirk_test_table_len = n;
}
#endif

guint32 uvc_quirks_lookup(guint16 vid, guint16 pid) {
    const uvc_quirk_entry_t *table = g_uvc_quirk_table;
    gsize n = G_N_ELEMENTS(g_uvc_quirk_table);

#ifdef LIBUVCH264SRC_TESTING
    if (g_quirk_test_table != NULL) {
        table = g_quirk_test_table;
        n = g_quirk_test_table_len;
    }
#endif

    guint32 flags = quirks_lookup_in(table, n, vid, pid);
    if (flags != 0) {
        GST_INFO("UVC quirk match for %04x:%04x -> flags 0x%08x", vid, pid, flags);
    }
    return flags;
}
