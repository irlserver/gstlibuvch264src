#ifndef GST_LIBUVC_H264_SRC_SPSPPS_PATH_H
#define GST_LIBUVC_H264_SRC_SPSPPS_PATH_H

/*
 * Pure, dependency-free SPS/PPS cache path builder.
 *
 * Kept free of GLib/GStreamer so it can be unit-tested in isolation
 * (tests/test_cache.c) without constructing a GObject. spspps_cache.c wraps
 * this with the element's logging and instance state.
 *
 * Two hardening properties live here:
 *
 *   M8 (path traversal): the device `index` is never interpolated verbatim. It
 *   is parsed with strtol and only the resulting non-negative integer reaches
 *   the path, so a hostile index such as "../.." or "/etc/passwd" collapses to
 *   a safe numeric component and can never escape ~/.spspps.
 *
 *   L5 (resolution key): the negotiated codec and WxH are folded into the file
 *   name, so a cache entry written for one resolution can never be loaded for a
 *   different one.
 */

#include <stdio.h>
#include <stdlib.h>

/*
 * Build the cache path into `out` (capacity `outlen`).
 *
 *   index == NULL -> the cache directory itself ("<home>/.spspps").
 *   index != NULL -> a resolution-keyed file
 *                    ("<home>/.spspps/<idx>_<codec>_<width>x<height>").
 *
 * Returns the number of characters written (excluding the NUL) on success, or
 * -1 if `out` is unusable or the path would be truncated. On -1 the caller MUST
 * skip the cache rather than touch the filesystem.
 */
static inline int spspps_build_path(char *out, size_t outlen,
                                    const char *home_dir, const char *index,
                                    int is_h265, int width, int height) {
    if (out == NULL || outlen == 0) {
        return -1;
    }
    if (home_dir == NULL) {
        home_dir = "";
    }

    int ret;
    if (index == NULL) {
        ret = snprintf(out, outlen, "%s/.spspps", home_dir);
    } else {
        /* M8: parse, do not interpolate. A non-numeric or negative index can
         * not introduce path separators or ".." once collapsed to a long. */
        char *end = NULL;
        long idx = strtol(index, &end, 10);
        if (end == index || idx < 0) {
            idx = 0;
        }

        /* L5: codec + resolution are part of the key. */
        const char *codec = is_h265 ? "h265" : "h264";
        ret = snprintf(out, outlen, "%s/.spspps/%ld_%s_%dx%d",
                       home_dir, idx, codec, width, height);
    }

    if (ret < 0 || (size_t)ret >= outlen) {
        return -1;
    }
    return ret;
}

#endif /* GST_LIBUVC_H264_SRC_SPSPPS_PATH_H */
