/*
 * Tests for gst_libuvc_h264_src_v4l2_probe().
 *
 * Two ctest suites dispatched by argv[1]:
 *
 *   probe    (v4l2_probe)          probe runs without crashing on any index
 *   nonfatal (v4l2_probe_nonfatal) absent /dev/video node is non-fatal
 */

#include <stdio.h>
#include <string.h>
#include <gst/gst.h>
#include "../libuvch264src/src/uvc_device.h"

/* uvc_device.c uses GST_DEBUG_OBJECT via GST_CAT_DEFAULT which resolves to
 * gst_libuvc_h264_src_debug. Normally defined in gstlibuvch264src.c; define
 * it here so this standalone test links without pulling in the full element. */
GST_DEBUG_CATEGORY(gst_libuvc_h264_src_debug);

static int g_failures;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("  ok   - %s\n", msg);                                      \
        } else {                                                               \
            printf("  FAIL - %s\n", msg);                                      \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static int run_probe(void) {
    GstElement *pipeline = gst_pipeline_new(NULL);

    gst_libuvc_h264_src_v4l2_probe(pipeline, 0);
    CHECK(1, "probe index 0 returns without crash");

    gst_libuvc_h264_src_v4l2_probe(pipeline, 999);
    CHECK(1, "probe index 999 returns without crash");

    gst_object_unref(pipeline);
    return g_failures;
}

static int run_nonfatal(void) {
    GstElement *pipeline = gst_pipeline_new(NULL);

    gst_libuvc_h264_src_v4l2_probe(pipeline, 9999);
    CHECK(1, "absent V4L2 node is non-fatal");

    gst_object_unref(pipeline);
    return g_failures;
}

int main(int argc, char **argv) {
    gst_init(NULL, NULL);
    GST_DEBUG_CATEGORY_INIT(gst_libuvc_h264_src_debug, "libuvch264src", 0,
                            "libuvch264src element");

    if (argc < 2) {
        fprintf(stderr, "usage: %s <probe|nonfatal>\n", argv[0]);
        gst_deinit();
        return 2;
    }

    int failures;
    if (strcmp(argv[1], "probe") == 0) {
        printf("v4l2_probe:\n");
        failures = run_probe();
    } else if (strcmp(argv[1], "nonfatal") == 0) {
        printf("v4l2_probe_nonfatal:\n");
        failures = run_nonfatal();
    } else {
        fprintf(stderr, "unknown suite: %s\n", argv[1]);
        gst_deinit();
        return 2;
    }

    printf("%s: %d failure(s)\n", argv[1], failures);
    gst_deinit();
    return failures == 0 ? 0 : 1;
}
