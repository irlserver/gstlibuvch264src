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
 * {vid, pid, flags, ...} rows and a pure lookup. No kernel code is copied.
 *
 * A workaround is enabled by adding one row to the static table in quirks.c
 * (with a comment citing the device and the evidence) - never by editing the
 * lookup logic. Every camera with no row keeps the original behavior exactly:
 * the lookup returns 0 and imposes no limits.
 *
 * quirks_lookup_in() and quirks_limits_in() are PURE functions over a
 * caller-supplied table so they can be unit-tested in isolation with a
 * test-local table (no global state). The production uvc_quirks_lookup() and
 * uvc_quirks_limits() are thin wrappers over the static table.
 */

#include <glib.h>

G_BEGIN_DECLS

/* Quirk flags. A row's `flags` is the OR of the workarounds that device needs.
 *
 *   QUIRK_DOUBLE_PROBE  Issue uvc_get_stream_ctrl_format_size() TWICE, discarding
 *                       the first result. Works around cameras that return a
 *                       stale/rejected stream control on the first probe
 *                       (libuvc issue #242).
 *
 *   QUIRK_MAX_PIXEL_RATE  The device advertises frame intervals it cannot
 *                       actually deliver, so descriptor-truth != device-truth.
 *                       The row's `max_pixel_rate` is the highest
 *                       width x height x fps that has been PROVEN to stream;
 *                       negotiation drops every advertised rate above it. */
#define QUIRK_DOUBLE_PROBE   (1u << 0)
#define QUIRK_MAX_PIXEL_RATE (1u << 1)

/* One quirk table row: a USB vendor:product ID mapped to its quirk flags, plus
 * the parameters those flags need.
 *
 * `max_pixel_rate` is read ONLY when QUIRK_MAX_PIXEL_RATE is set, so a row that
 * does not use the flag simply leaves it out (C zero-initialises the tail). */
typedef struct {
    guint16 vid;
    guint16 pid;
    guint32 flags;
    guint64 max_pixel_rate;
} uvc_quirk_entry_t;

/* The mode-selection limits a matched quirk row imposes, resolved from its flags.
 * An all-zero value is what every unquirked device gets: no limit at all.
 *
 * `max_pixel_rate` is 0 unless the row set QUIRK_MAX_PIXEL_RATE, so callers test
 * this one field and never have to re-check the flag. */
typedef struct {
    guint32 flags;
    guint64 max_pixel_rate; /* 0 = unlimited */
} uvc_quirk_limits_t;

/* Pure lookup over a caller-supplied table (n rows). Returns the `flags` of the
 * first row whose (vid, pid) matches, or 0 if none matches (including a NULL or
 * empty table). No global state - unit-testable with a test-local table. */
guint32 quirks_lookup_in(const uvc_quirk_entry_t *table, gsize n,
                         guint16 vid, guint16 pid);

/* Production lookup over the static quirk table. Returns the quirk flags for
 * (vid, pid), or 0 when no quirk applies. Logs GST_INFO on a match. */
guint32 uvc_quirks_lookup(guint16 vid, guint16 pid);

/* Pure limits resolution over a caller-supplied table. Fills *out from the first
 * matching row, or with all zeroes when nothing matches. */
void quirks_limits_in(const uvc_quirk_entry_t *table, gsize n,
                      guint16 vid, guint16 pid, uvc_quirk_limits_t *out);

/* Production limits resolution over the same table uvc_quirks_lookup() consults.
 * negotiate() calls this once and carries the result through mode selection. */
void uvc_quirks_limits(guint16 vid, guint16 pid, uvc_quirk_limits_t *out);

/* May negotiation select `width` x `height` at `fps` under these limits? TRUE for
 * every mode when `limits` is NULL or imposes no cap. */
gboolean uvc_quirk_mode_selectable(const uvc_quirk_limits_t *limits,
                                   guint width, guint height, guint fps);

/* Highest fps `limits` allows at `width` x `height`, or G_MAXUINT when uncapped.
 * For the continuous-frame-interval descriptors, where a whole fps RANGE has to
 * be clamped rather than a discrete list filtered. */
guint uvc_quirk_max_fps(const uvc_quirk_limits_t *limits,
                        guint width, guint height);

#ifdef LIBUVCH264SRC_TESTING
/* Test-only seam (A14): override the table the production lookups consult so a
 * static-registration test can key a quirk against the mock device's vid:pid
 * without adding a production row. Passing (NULL, 0) restores the shipped
 * table. This symbol is compiled ONLY into the test targets
 * that define LIBUVCH264SRC_TESTING (see tests/CMakeLists.txt); the production
 * plugin never sees it. */
void uvc_quirks_set_test_table(const uvc_quirk_entry_t *table, gsize n);
#endif

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_QUIRKS_H */
