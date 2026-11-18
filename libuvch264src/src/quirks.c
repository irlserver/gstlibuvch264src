#include "quirks.h"
#include "gstlibuvch264src_internal.h"

static const uvc_quirk_entry_t *quirks_find_row(const uvc_quirk_entry_t *table,
                                                gsize n, guint16 vid,
                                                guint16 pid) {
    if (table == NULL) {
        return NULL;
    }
    for (gsize i = 0; i < n; i++) {
        if (table[i].vid == vid && table[i].pid == pid) {
            return &table[i];
        }
    }
    return NULL;
}

guint32 quirks_lookup_in(const uvc_quirk_entry_t *table, gsize n,
                         guint16 vid, guint16 pid) {
    const uvc_quirk_entry_t *row = quirks_find_row(table, n, vid, pid);
    return row != NULL ? row->flags : 0;
}

void quirks_limits_in(const uvc_quirk_entry_t *table, gsize n,
                      guint16 vid, guint16 pid, uvc_quirk_limits_t *out) {
    g_return_if_fail(out != NULL);

    out->flags = 0;
    out->max_pixel_rate = 0;

    const uvc_quirk_entry_t *row = quirks_find_row(table, n, vid, pid);
    if (row == NULL) {
        return;
    }

    out->flags = row->flags;
    /* Resolving the flag HERE is what lets every caller test max_pixel_rate
     * alone; a row that carries a rate without the flag imposes no cap. */
    if (row->flags & QUIRK_MAX_PIXEL_RATE) {
        out->max_pixel_rate = row->max_pixel_rate;
    }
}

gboolean uvc_quirk_mode_selectable(const uvc_quirk_limits_t *limits,
                                   guint width, guint height, guint fps) {
    if (limits == NULL || limits->max_pixel_rate == 0) {
        return TRUE;
    }
    /* 64-bit product: 4K@60 alone is 497 664 000, and a future 8K row would
     * overflow 32 bits outright. */
    guint64 rate = (guint64) width * (guint64) height * (guint64) fps;
    return rate <= limits->max_pixel_rate;
}

guint uvc_quirk_max_fps(const uvc_quirk_limits_t *limits,
                        guint width, guint height) {
    if (limits == NULL || limits->max_pixel_rate == 0) {
        return G_MAXUINT;
    }
    guint64 pixels = (guint64) width * (guint64) height;
    if (pixels == 0) {
        return G_MAXUINT;
    }
    guint64 max_fps = limits->max_pixel_rate / pixels;
    return max_fps > G_MAXUINT ? G_MAXUINT : (guint) max_fps;
}

/* Production quirk table. One row per device that needs a workaround, each citing
 * the device and the evidence. Adding a row is the ONLY way to enable a quirk -
 * negotiate() already branches on the flags, so nothing else changes, and every
 * camera without a row keeps the original behavior exactly.
 *
 * ---------------------------------------------------------------------------
 * DJI Osmo Pocket 3 (2ca3:0023) - advertises H.264 modes it cannot deliver.
 *
 * Its H.264 descriptor offers 3840x2160 at 60/50/48/30/25/24 fps. The 60/50/48
 * rates are PHANTOM: negotiation binds them (the descriptor really does carry
 * dwFrameInterval 166666/200000/208333 - verified byte-for-byte from a raw USB
 * descriptor dump on both the USB-A and USB-C ports, and independently by the
 * kernel uvcvideo parser) and then not one frame ever arrives, so the element's
 * 5 s silence watchdog synthesizes a disconnect and the stream dies with
 * RESOURCE/READ. Because negotiate() prefers the largest area at the highest
 * fps, the phantom 4K@60 was selected BY CONSTRUCTION on every permissive-caps
 * negotiation. The camera appears to advertise its RECORDING capability here;
 * the corroboration is that its MJPEG descriptor, for the same 3840x2160, lists
 * only 30/25/24 - the 60/50/48 claim exists on the H.264 descriptor alone.
 *
 * The cap is 3840x2160x30 = 248 832 000 px/s, and it is CONFIRMED on hardware,
 * not inferred from the descriptor. Board 192.168.78.131, 2026-07-30, plugin
 * .so deployed alone so the board's libuvc stayed at 4868e57 and this cap was
 * the only variable; two independent runs of
 * `num-buffers=300 ! video/x-h264,width=3840,height=2160,framerate=30/1`:
 *
 *   run 1  clean EOS, exit 0, 10.71 s, h264parse read 3840x2160 / high / 5.2
 *   run 2  exactly 300/300 access units counted at an identity probe, 10.79 s,
 *          same SPS-derived 3840x2160 / high / level 5.2 / 4:2:0
 *
 * Zero errors, zero RESOURCE/READ, no silence-watchdog disconnect on either
 * run. That is exactly the bar this table demands and nothing less: frames
 * ADVANCED for the full ~10.7 s window, at a resolution read out of the SPS
 * rather than the requested caps echoed back - a successful
 * uvc_start_streaming() proves nothing on this device.
 *
 * The value is 4K@30 EXACTLY, not the full 4K@60 descriptor range, and that is
 * what makes those runs conclusive rather than suggestive: negotiate() prefers
 * max area then max fps, so at 248 832 000 the surviving top mode is
 * 3840x2160@30 BY CONSTRUCTION and the capture cannot have silently measured
 * 4K@60 instead. Both runs logged `max pixel rate 248832000` and `quirk:
 * dropped 3 non-deliverable rate(s) at 3840x2160`, confirming 60/50/48 were
 * still excluded while the measured rate streamed.
 *
 * 4K@60/50/48 stay excluded, and this row does not speak to them. An UNCAPPED
 * build did deliver 600 access units at ~56 fps on the same date
 * (camera-compat.md S2 Step 5), so the original zero-frame observation is not
 * currently reproducible - but it was real and was never explained (one
 * candidate: the same stale-readback defect QUIRK_DOUBLE_PROBE exists for can
 * leave a bound-but-silent stream when the readback happens to compare equal).
 * So the cap still sits at the last CONFIRMED-GOOD rate rather than the last
 * known-bad one. The two failure directions are not symmetric - a cap set too
 * low costs resolution, a cap set too high costs the whole stream.
 *
 * To raise it further, measure the candidate mode the same way - advancing
 * frames, a bounded access-unit count, SPS-verified geometry, reproduced - and
 * then change this ONE number: 3840x2160@60 would be 497 664 000. The literal
 * in tests/test_quirks.c is a deliberate tripwire on this value and has to move
 * with it.
 *
 * Only pid 0023 needs a row. The camera also enumerates as 2ca3:0020, but that
 * is its RNDIS + mass-storage "connect to computer" mode with no UVC interface
 * at all, so it never reaches negotiation.
 *
 * It also needs QUIRK_DOUBLE_PROBE, for a SEPARATE defect that the pixel-rate
 * cap does not address. libuvc's uvc_probe_stream_ctrl() SET_CURs the control it
 * wants, GET_CURs it back, and rejects the mode when the readback disagrees
 * (_uvc_stream_params_negotiated, libuvc src/stream.c). The Osmo answers that
 * first GET_CUR out of the mode it had PREVIOUSLY committed, so any negotiation
 * that asks for a LARGER mode than the one currently committed is rejected with
 * UVC_ERROR_INVALID_MODE - surfacing as "Unable to get stream control: Invalid
 * mode" ~170 ms into start(), which reads to an operator as the camera not being
 * detected.
 *
 * Measured on hardware (192.168.78.131, 2026-07-30), one probe vs two, same
 * binary otherwise:
 *
 *   1280x720@30  -> 1920x1080@30   1 probe: 3/3 FAIL    2 probes: 3/3 pass
 *   1920x1080@30 -> 3840x2160@60   1 probe: 20/20 FAIL  2 probes: 4/4 pass
 *   same mode again, or SMALLER    1 probe: pass        (readback already agrees)
 *
 * The failure is deterministic and direction-specific, not flaky: 23/23 on a
 * mode increase, 0 otherwise. Note the 720p->1080p row - the element's own
 * shipping mode is affected, so this is not a 4K-only concern.
 * --------------------------------------------------------------------------- */
static const uvc_quirk_entry_t g_uvc_quirk_table[] = {
    { 0x2ca3, 0x0023, QUIRK_MAX_PIXEL_RATE | QUIRK_DOUBLE_PROBE, 248832000u },
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

static const uvc_quirk_entry_t *quirks_active_table(gsize *n) {
#ifdef LIBUVCH264SRC_TESTING
    if (g_quirk_test_table != NULL) {
        *n = g_quirk_test_table_len;
        return g_quirk_test_table;
    }
#endif
    *n = G_N_ELEMENTS(g_uvc_quirk_table);
    return g_uvc_quirk_table;
}

void uvc_quirks_limits(guint16 vid, guint16 pid, uvc_quirk_limits_t *out) {
    g_return_if_fail(out != NULL);

    gsize n = 0;
    const uvc_quirk_entry_t *table = quirks_active_table(&n);

    quirks_limits_in(table, n, vid, pid, out);
    if (out->flags != 0) {
        GST_INFO("UVC quirk match for %04x:%04x -> flags 0x%08x, "
                 "max pixel rate %" G_GUINT64_FORMAT,
                 vid, pid, out->flags, out->max_pixel_rate);
    }
}

guint32 uvc_quirks_lookup(guint16 vid, guint16 pid) {
    uvc_quirk_limits_t limits = {0};
    uvc_quirks_limits(vid, pid, &limits);
    return limits.flags;
}
