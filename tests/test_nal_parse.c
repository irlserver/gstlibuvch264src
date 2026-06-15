/*
 * Unit tests for the Annex-B NAL parser in frame_pipeline.c.
 *
 * The parser (find_nal_unit / count_nal_units / parse_nal_units) is exercised
 * directly against crafted byte buffers - no UVC device, no GStreamer runtime,
 * no mock feeder. frame_pipeline.c also defines frame_callback(), whose only
 * element-internal references are the debug category and store_spspps(); both
 * are stubbed below so the parser TU links standalone (the parser functions
 * under test touch neither). Three argv-selected suites:
 *
 *   multislice  >10 slice NALs in one frame are ALL parsed, none dropped (L2)
 *   startcode   3-byte (00 00 01) and offset-shifted frames parse (L3)
 *   bounds      truncated / oversized / merged buffers stay in bounds (ASAN)
 *
 * Buffers are allocated to their EXACT used length with g_malloc so the ASAN
 * redzone sits immediately after the last valid byte - that is what gives the
 * bounds suite teeth against an over-read.
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <libuvc/libuvc.h>

#include "frame_pipeline.h"

/* frame_pipeline.c pulls these in through frame_callback(), which this test
   never calls; trivial definitions satisfy the linker without dragging in the
   element or the SPS/PPS cache TU. */
GST_DEBUG_CATEGORY(gst_libuvc_h264_src_debug);
void store_spspps(GstLibuvcH264Src *self) { (void)self; }

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

/* H.264 NAL header bytes: forbidden_zero_bit 0, nal_ref_idc, nal_unit_type. */
#define NH_SPS    0x67  /* type 7 */
#define NH_PPS    0x68  /* type 8 */
#define NH_IDR    0x65  /* type 5 */
#define NH_NONIDR 0x61  /* type 1 */

/* Append one Annex-B NAL: a sc_len-byte start code, a header byte, then `pay`
   bytes of 0xAB (non-zero payload can never form a 00 00 01 sequence, so the
   only start codes in the buffer are the ones placed here). Returns new pos. */
static gsize append_nal(unsigned char *buf, gsize pos, int sc_len,
                        unsigned char header, gsize pay) {
    if (sc_len == 4) buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;
    buf[pos++] = header;
    for (gsize k = 0; k < pay; k++) buf[pos++] = 0xAB;
    return pos;
}

static int run_multislice(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H264;

    /* One access unit: SPS + PPS + IDR + 12 non-IDR slices = 15 NAL units. The
       retired fixed cap was 10, so >10 is the case that used to drop slices. */
    gsize size = 0;
    size += 4 + 1 + 8;            /* SPS */
    size += 4 + 1 + 4;            /* PPS */
    size += 4 + 1 + 32;           /* IDR */
    size += 12 * (4 + 1 + 16);    /* 12 non-IDR slices */
    unsigned char *buf = g_malloc(size);

    gsize pos = 0;
    pos = append_nal(buf, pos, 4, NH_SPS, 8);
    pos = append_nal(buf, pos, 4, NH_PPS, 4);
    pos = append_nal(buf, pos, 4, NH_IDR, 32);
    for (int s = 0; s < 12; s++)
        pos = append_nal(buf, pos, 4, NH_NONIDR, 16);
    CHECK(pos == size, "multislice buffer filled to its exact length");

    gsize n = count_nal_units(fmt, buf, size);
    CHECK(n == 15, "count_nal_units returns all 15 units (not capped at 10)");

    nal_unit_t *units = g_new(nal_unit_t, n ? n : 1);
    gsize c = parse_nal_units(fmt, units, n, buf, size);
    CHECK(c == 15, "parse_nal_units delivers all 15 units when sized to the count");
    CHECK(c >= 3 && units[0].type == UNIT_SPS, "unit 0 is SPS");
    CHECK(c >= 3 && units[1].type == UNIT_PPS, "unit 1 is PPS");
    CHECK(c >= 3 && units[2].type == UNIT_FRAME_IDR, "unit 2 is IDR");

    int all_nonidr = (c == 15);
    for (gsize i = 3; i < c; i++)
        if (units[i].type != UNIT_FRAME_NON_IDR) all_nonidr = 0;
    CHECK(all_nonidr, "units 3..14 are all non-IDR slices (none dropped past the cap)");

    /* The unit spans must tile the whole buffer with no gaps or overruns. */
    gsize sum = 0;
    for (gsize i = 0; i < c; i++) sum += units[i].len;
    CHECK(sum == size, "unit lengths tile the whole buffer exactly");
    CHECK(c > 0 && units[c - 1].ptr + units[c - 1].len == buf + size,
          "last unit reaches the end of the buffer");

    /* A caller-supplied small max still caps (the cap is now the caller's
       choice, not a hidden constant) - proving the count is the real fix. */
    nal_unit_t capped[10];
    gsize cc = parse_nal_units(fmt, capped, 10, buf, size);
    CHECK(cc == 10, "a small max caps at 10, yet count_nal_units reported 15");

    g_free(units);
    g_free(buf);
    return g_failures;
}

static int run_startcode(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H264;

    /* (a) 3-byte start codes only must parse just like 4-byte ones. */
    {
        gsize size = (3 + 1 + 8) + (3 + 1 + 4) + (3 + 1 + 32);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        pos = append_nal(buf, pos, 3, NH_SPS, 8);
        pos = append_nal(buf, pos, 3, NH_PPS, 4);
        pos = append_nal(buf, pos, 3, NH_IDR, 32);
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 3, "3-byte start codes: all 3 units found");
        nal_unit_t u[3];
        gsize c = parse_nal_units(fmt, u, 3, buf, size);
        CHECK(c == 3 && u[0].type == UNIT_SPS && u[1].type == UNIT_PPS &&
              u[2].type == UNIT_FRAME_IDR,
              "3-byte start codes: types parsed correctly");
        g_free(buf);
    }

    /* (b) A frame that does not begin at offset 0 (leading junk before the
       first start code) must be found, not dropped - this is the search=1 fix. */
    {
        gsize junk = 6;
        gsize size = junk + (4 + 1 + 8) + (4 + 1 + 16);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        for (gsize k = 0; k < junk; k++) buf[pos++] = 0xFF;  /* no 00 00 01 here */
        gsize first_sc = pos;
        pos = append_nal(buf, pos, 4, NH_SPS, 8);
        pos = append_nal(buf, pos, 4, NH_IDR, 16);
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 2, "offset-shifted frame: units found despite leading junk");
        nal_unit_t u[2];
        gsize c = parse_nal_units(fmt, u, 2, buf, size);
        CHECK(c == 2, "offset-shifted frame: parsed, not dropped");
        CHECK(c == 2 && u[0].ptr == buf + first_sc,
              "first unit begins at the start code, not at offset 0");
        CHECK(c == 2 && u[0].type == UNIT_SPS && u[1].type == UNIT_FRAME_IDR,
              "offset-shifted frame: types parsed correctly");
        g_free(buf);
    }

    /* (c) A 3-byte start code sitting immediately after a 4-byte one must not be
       skipped (advance-by-start-code-length, not a fixed +5). */
    {
        gsize size = (4 + 1 + 8) + (3 + 1 + 4) + (4 + 1 + 16);
        unsigned char *buf = g_malloc(size);
        gsize pos = 0;
        pos = append_nal(buf, pos, 4, NH_SPS, 8);
        pos = append_nal(buf, pos, 3, NH_PPS, 4);
        pos = append_nal(buf, pos, 4, NH_IDR, 16);
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 3, "mixed 3+4-byte start codes: all 3 units found");
        nal_unit_t u[3];
        gsize c = parse_nal_units(fmt, u, 3, buf, size);
        CHECK(c == 3 && u[0].type == UNIT_SPS && u[1].type == UNIT_PPS &&
              u[2].type == UNIT_FRAME_IDR,
              "mixed start codes: types parsed correctly");
        g_free(buf);
    }

    /* (d) find_nal_unit reports the start-code length for both forms. */
    {
        unsigned char b4[8] = {0, 0, 0, 1, NH_IDR, 0xAB, 0xAB, 0xAB};
        unsigned char b3[8] = {0, 0, 1, NH_SPS, 0xAB, 0xAB, 0xAB, 0xAB};
        gsize off = 99, sc = 0;
        int t = find_nal_unit(fmt, b4, sizeof(b4), 0, 1, &off, &sc);
        CHECK(t == UNIT_FRAME_IDR && off == 0 && sc == 4,
              "find_nal_unit reports a 4-byte start code (sc_len == 4)");
        off = 99;
        sc = 0;
        t = find_nal_unit(fmt, b3, sizeof(b3), 0, 1, &off, &sc);
        CHECK(t == UNIT_SPS && off == 0 && sc == 3,
              "find_nal_unit reports a 3-byte start code (sc_len == 3)");
    }

    return g_failures;
}

static int run_bounds(void) {
    enum uvc_frame_format fmt = UVC_FRAME_FORMAT_H264;

    /* (a) A dangling 4-byte start code at the very end (no header byte) must not
       trigger a read past the buffer while looking for the NAL header. */
    {
        gsize size = (4 + 1 + 3) + 4;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_IDR, 3);
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x01;
        CHECK(pos == size, "bounds(a): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "bounds(a): dangling 4-byte start code is not a unit");
        nal_unit_t u[4];
        gsize c = parse_nal_units(fmt, u, 4, buf, size);
        CHECK(c == 1 && u[0].type == UNIT_FRAME_IDR,
              "bounds(a): the one complete unit is parsed");
        g_free(buf);
    }

    /* (b) A trailing partial start code (00 00) must not be over-read. */
    {
        gsize size = (4 + 1 + 4) + 2;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_IDR, 4);
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        CHECK(pos == size, "bounds(b): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "bounds(b): trailing partial start code is not a unit");
        g_free(buf);
    }

    /* (c) Buffers shorter than a minimal unit (and a zero-length buffer) must
       never index into the buffer at all. */
    {
        unsigned char *b = g_malloc(3);
        b[0] = 0x00; b[1] = 0x00; b[2] = 0x01;
        CHECK(count_nal_units(fmt, b, 3) == 0, "bounds(c): 3-byte buffer yields no unit");
        gsize off = 0, sc = 0;
        CHECK(find_nal_unit(fmt, b, 3, 0, 1, &off, &sc) == -1,
              "bounds(c): find_nal_unit on 3 bytes returns -1");
        g_free(b);

        unsigned char *z = g_malloc(1);
        CHECK(count_nal_units(fmt, z, 0) == 0, "bounds(c): zero-length buffer yields no unit");
        g_free(z);
    }

    /* (d) One oversized merged NAL (payload far larger than the SPS/PPS clamp
       limit). The parser must report the whole span as one unit without reading
       past the exact-size allocation; the frame_callback clamp (Task 5) is what
       later refuses to copy it, not the parser. */
    {
        gsize pay = 5000;             /* > SPSPPSBUFSZ (1024) */
        gsize size = 4 + 1 + pay;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_SPS, pay);
        CHECK(pos == size, "bounds(d): oversized buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "bounds(d): one oversized merged unit counted");
        nal_unit_t u[1];
        gsize c = parse_nal_units(fmt, u, 1, buf, size);
        CHECK(c == 1 && u[0].len == size,
              "bounds(d): oversized unit length spans the whole buffer");
        CHECK(c == 1 && u[0].len > SPSPPSBUFSZ,
              "bounds(d): oversized unit exceeds the SPS/PPS clamp limit");
        g_free(buf);
    }

    /* (e) A malformed separator (00 00 02, not a real start code) must not split
       the unit, and its bytes must not be over-read. */
    {
        gsize size = (4 + 1 + 6) + 5;
        unsigned char *buf = g_malloc(size);
        gsize pos = append_nal(buf, 0, 4, NH_IDR, 6);
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x02;
        buf[pos++] = 0xAB; buf[pos++] = 0xAB;
        CHECK(pos == size, "bounds(e): buffer filled exactly");
        gsize n = count_nal_units(fmt, buf, size);
        CHECK(n == 1, "bounds(e): malformed separator does not split the unit");
        g_free(buf);
    }

    return g_failures;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <multislice|startcode|bounds>\n", argv[0]);
        return 2;
    }

    int failures;
    if (strcmp(argv[1], "multislice") == 0) {
        printf("nal_parse multislice:\n");
        failures = run_multislice();
    } else if (strcmp(argv[1], "startcode") == 0) {
        printf("nal_parse startcode:\n");
        failures = run_startcode();
    } else if (strcmp(argv[1], "bounds") == 0) {
        printf("nal_parse bounds:\n");
        failures = run_bounds();
    } else {
        fprintf(stderr, "unknown suite: %s\n", argv[1]);
        return 2;
    }

    printf("%s: %d failure(s)\n", argv[1], failures);
    return failures == 0 ? 0 : 1;
}
