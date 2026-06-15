/*
 * Unit tests for the SPS/PPS cache path builder (libuvch264src/src/spspps_path.h).
 *
 * These run against the pure spspps_build_path() helper, so no GObject /
 * GStreamer instance is needed. Two ctest suites are dispatched by argv[1]:
 *
 *   path  (cache_path_safety)   - M7 NULL-path safety + M8 traversal blocking
 *   key   (cache_resolution_key)- L5 codec + resolution keyed file names
 */

#include <stdio.h>
#include <string.h>

#include "spspps_path.h"

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

#define HOME "/home/tester"
#define DIR HOME "/.spspps"

static int run_path_safety(void) {
    char buf[4096];
    int ret;

    /* M8: a "../.." index must not escape ~/.spspps. */
    ret = spspps_build_path(buf, sizeof(buf), HOME, "../..", 0, 1920, 1080);
    CHECK(ret > 0, "traversal index builds a path");
    CHECK(strstr(buf, "..") == NULL, "traversal index contains no '..'");
    CHECK(strncmp(buf, DIR "/", strlen(DIR "/")) == 0,
          "traversal index stays under ~/.spspps");
    CHECK(strcmp(buf, DIR "/0_h264_1920x1080") == 0,
          "non-numeric index collapses to safe '0'");

    /* An absolute-looking index must not inject a new root. */
    ret = spspps_build_path(buf, sizeof(buf), HOME, "/etc/passwd", 0, 1280, 720);
    CHECK(ret > 0 && strstr(buf, "/etc/passwd") == NULL,
          "absolute-path index does not reach /etc/passwd");
    CHECK(strcmp(buf, DIR "/0_h264_1280x720") == 0,
          "absolute-path index collapses to safe '0'");

    /* A leading digit followed by traversal keeps only the parsed integer. */
    ret = spspps_build_path(buf, sizeof(buf), HOME, "5/../../etc", 0, 640, 480);
    CHECK(ret > 0 && strstr(buf, "..") == NULL,
          "digit+traversal index drops the traversal");
    CHECK(strcmp(buf, DIR "/5_h264_640x480") == 0,
          "digit+traversal index keeps only the parsed integer");

    /* A negative index is rejected to the safe fallback. */
    ret = spspps_build_path(buf, sizeof(buf), HOME, "-1", 0, 320, 240);
    CHECK(strcmp(buf, DIR "/0_h264_320x240") == 0,
          "negative index collapses to safe '0'");

    /* A normal numeric index passes through. */
    ret = spspps_build_path(buf, sizeof(buf), HOME, "2", 0, 1920, 1080);
    CHECK(strcmp(buf, DIR "/2_h264_1920x1080") == 0,
          "numeric index is preserved");

    /* index == NULL yields the directory itself, no key, no traversal. */
    ret = spspps_build_path(buf, sizeof(buf), HOME, NULL, 0, 1920, 1080);
    CHECK(strcmp(buf, DIR) == 0, "NULL index builds the cache directory");

    /* M7: an unusable output buffer returns -1 so callers skip the cache. */
    CHECK(spspps_build_path(NULL, sizeof(buf), HOME, "0", 0, 1920, 1080) < 0,
          "NULL output buffer returns -1 (cache skipped)");
    CHECK(spspps_build_path(buf, 0, HOME, "0", 0, 1920, 1080) < 0,
          "zero-length buffer returns -1 (cache skipped)");

    /* Truncation must fail rather than emit a partial path. */
    char tiny[8];
    CHECK(spspps_build_path(tiny, sizeof(tiny), HOME, "0", 0, 1920, 1080) < 0,
          "truncated path returns -1 (cache skipped)");

    /* A NULL home does not crash and stays relative to ".spspps". */
    ret = spspps_build_path(buf, sizeof(buf), NULL, "0", 0, 1920, 1080);
    CHECK(ret > 0 && strstr(buf, "..") == NULL,
          "NULL home is handled without traversal");

    return g_failures;
}

static int run_resolution_key(void) {
    char a[4096];
    char b[4096];

    /* L5: same index, different resolution -> different cache files. */
    spspps_build_path(a, sizeof(a), HOME, "0", 0, 1920, 1080);
    spspps_build_path(b, sizeof(b), HOME, "0", 0, 1280, 720);
    CHECK(strcmp(a, b) != 0, "different resolution yields different file");
    CHECK(strcmp(a, DIR "/0_h264_1920x1080") == 0, "1080p key is correct");
    CHECK(strcmp(b, DIR "/0_h264_1280x720") == 0, "720p key is correct");

    /* L5: same index/resolution, different codec -> different cache files. */
    spspps_build_path(a, sizeof(a), HOME, "0", 0, 1920, 1080);
    spspps_build_path(b, sizeof(b), HOME, "0", 1, 1920, 1080);
    CHECK(strcmp(a, b) != 0, "different codec yields different file");
    CHECK(strstr(a, "h264") != NULL, "H264 key carries codec tag");
    CHECK(strstr(b, "h265") != NULL, "H265 key carries codec tag");

    /* The key embeds the resolution verbatim. */
    spspps_build_path(a, sizeof(a), HOME, "1", 1, 3840, 2160);
    CHECK(strstr(a, "3840x2160") != NULL, "key embeds WxH resolution");
    CHECK(strcmp(a, DIR "/1_h265_3840x2160") == 0, "full 4K H265 key is correct");

    /* Deterministic: identical inputs produce identical keys. */
    spspps_build_path(a, sizeof(a), HOME, "7", 0, 1920, 1080);
    spspps_build_path(b, sizeof(b), HOME, "7", 0, 1920, 1080);
    CHECK(strcmp(a, b) == 0, "identical inputs produce identical key");

    return g_failures;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path|key>\n", argv[0]);
        return 2;
    }

    int failures;
    if (strcmp(argv[1], "path") == 0) {
        printf("cache_path_safety:\n");
        failures = run_path_safety();
    } else if (strcmp(argv[1], "key") == 0) {
        printf("cache_resolution_key:\n");
        failures = run_resolution_key();
    } else {
        fprintf(stderr, "unknown suite: %s\n", argv[1]);
        return 2;
    }

    printf("%s: %d failure(s)\n", argv[1], failures);
    return failures == 0 ? 0 : 1;
}
