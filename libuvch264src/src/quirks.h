#ifndef GST_LIBUVC_H264_SRC_QUIRKS_H
#define GST_LIBUVC_H264_SRC_QUIRKS_H

/*
 * Per-device (vid:pid) quirk seam for the libuvch264src element.
 *
 * Some UVC cameras need a device-specific workaround that has nothing to do with
 * the format they advertise. Rather than scatter `if (vid == ... && pid == ...)`
 * checks through negotiate(), a single flat table maps a USB vendor:product ID
 * to a bitmask of quirk flags, and negotiate() branches on those flags.
 *
 * The design mirrors the Linux uvcvideo quirk table
 * (drivers/media/usb/uvc/uvc_driver.c) in shape only: a flat array of
 * {vid, pid, flags} rows and a pure lookup. No kernel code is copied.
 *
 * The PRODUCTION table (uvc_quirks_lookup) ships EMPTY: no device currently
 * needs a quirk, so with all cameras the lookup returns 0 and behavior is
 * byte-for-byte unchanged. A workaround is enabled by adding one row to the
 * static table in quirks.c (with a comment citing the device and the upstream
 * issue) - never by editing the lookup logic.
 *
 * quirks_lookup_in() is a PURE function over a caller-supplied table so it can
 * be unit-tested in isolation with a test-local table (no global state). The
 * production uvc_quirks_lookup() is a thin wrapper over the static empty table.
 */

#include <glib.h>

G_BEGIN_DECLS

/* Quirk flags. A row's `flags` is the OR of the workarounds that device needs.
 *
 *   QUIRK_DOUBLE_PROBE  Issue uvc_get_stream_ctrl_format_size() TWICE, discarding
 *                       the first result. Works around cameras that return a
 *                       stale/rejected stream control on the first probe
 *                       (libuvc issue #242). */
#define QUIRK_DOUBLE_PROBE (1u << 0)

/* One quirk table row: a USB vendor:product ID mapped to its quirk flags. */
typedef struct {
    guint16 vid;
    guint16 pid;
    guint32 flags;
} uvc_quirk_entry_t;

/* Pure lookup over a caller-supplied table (n rows). Returns the `flags` of the
 * first row whose (vid, pid) matches, or 0 if none matches (including a NULL or
 * empty table). No global state - unit-testable with a test-local table. */
guint32 quirks_lookup_in(const uvc_quirk_entry_t *table, gsize n,
                         guint16 vid, guint16 pid);

/* Production lookup over the static (empty) quirk table. Returns the quirk flags
 * for (vid, pid), or 0 when no quirk applies. Logs GST_INFO on a match. */
guint32 uvc_quirks_lookup(guint16 vid, guint16 pid);

#ifdef LIBUVCH264SRC_TESTING
/* Test-only seam (A14): override the table uvc_quirks_lookup() consults so a
 * static-registration test can key a quirk against the mock device's vid:pid
 * without shipping a populated production entry. Passing (NULL, 0) restores the
 * production empty table. This symbol is compiled ONLY into the test targets
 * that define LIBUVCH264SRC_TESTING (see tests/CMakeLists.txt); the production
 * plugin never sees it. */
void uvc_quirks_set_test_table(const uvc_quirk_entry_t *table, gsize n);
#endif

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_QUIRKS_H */
