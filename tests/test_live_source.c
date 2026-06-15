/* Live-source polish tests (Task 19).
 *
 * Statically links the element translation units, the libuvc mock, and the
 * driver into ONE executable (like test_pts_monotonic) so the tests can both
 * drive a real PLAYING pipeline against the mock feeder AND reach into the
 * instance struct / call frame_callback() directly. GST_CHECKS selects one case
 * per ctest invocation (see tests/CMakeLists.txt).
 *
 *   live_source_polish      Drive PLAYING against the mock (30 fps H.264). Assert
 *                           the LATENCY query reports a live source with a
 *                           minimum latency of exactly one frame interval (not
 *                           the GstBaseSrc default of 0), and that consecutive
 *                           buffers carry a strictly monotonic GST_BUFFER_OFFSET
 *                           with OFFSET_END == OFFSET + 1.
 *
 *   spspps_write_on_change  Call frame_callback() directly with crafted access
 *                           units. The SPS/PPS cache file must be written on the
 *                           first (changed) parameter set, NOT rewritten when an
 *                           identical set repeats, and written again only when
 *                           the SPS actually changes (L10).
 */

#include <gst/check/gstcheck.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

/* gstcheck.h already defines GST_CAT_DEFAULT (check_debug); drop it so the
 * element's internal header can install its own category without a warning. */
#undef GST_CAT_DEFAULT
#include "gstlibuvch264src_internal.h"
#include "frame_pipeline.h"
#include "spspps_path.h"
#include "mock_libuvc.h"

/* The mock advertises a single 30 fps interval (333333 * 100 ns), so negotiate()
 * computes frame_interval = 1e9 / 30. */
#define EXPECTED_FRAME_INTERVAL_NS (GST_SECOND / 30)

static void
load_core_elements (void)
{
  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }
}

static void
register_element (void)
{
  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, "libuvch264src", GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register libuvch264src");
    registered = TRUE;
  }
}

/* ------------------------------------------------------------------------- */
/* live_source_polish: LATENCY query + monotonic buffer offsets              */
/* ------------------------------------------------------------------------- */

static gint offset_violation;   /* atomic: an OFFSET broke the +1 sequence */
static gint offset_checked;     /* atomic: buffers with a valid OFFSET examined */
static gint seen_first;         /* atomic: at least one buffer has arrived */
static guint64 off_prev;        /* streaming-thread local */
static gboolean off_have_prev;  /* streaming-thread local */

static GstPadProbeReturn
offset_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (!(GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER))
    return GST_PAD_PROBE_OK;

  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  guint64 off = GST_BUFFER_OFFSET (buf);
  guint64 off_end = GST_BUFFER_OFFSET_END (buf);

  if (off != GST_BUFFER_OFFSET_NONE) {
    if (off_have_prev && off != off_prev + 1)
      g_atomic_int_set (&offset_violation, 1);
    if (off_end != off + 1)
      g_atomic_int_set (&offset_violation, 1);
    off_prev = off;
    off_have_prev = TRUE;
    g_atomic_int_inc (&offset_checked);
  } else {
    g_atomic_int_set (&offset_violation, 1);
  }

  g_atomic_int_set (&seen_first, 1);
  return GST_PAD_PROBE_OK;
}

GST_START_TEST (test_live_source_polish)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();            /* VALID mode, 30 fps H.264 */

  g_atomic_int_set (&offset_violation, 0);
  g_atomic_int_set (&offset_checked, 0);
  g_atomic_int_set (&seen_first, 0);
  off_have_prev = FALSE;
  off_prev = 0;

  GstElement *pipeline = gst_pipeline_new ("live-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");
  fail_unless (pipeline && src && sink, "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  g_object_set (src, "num-buffers", 60, NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  GstPad *spad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (spad, GST_PAD_PROBE_TYPE_BUFFER, offset_probe, NULL, NULL);
  gst_object_unref (spad);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* Wait until streaming so negotiate() has set frame_interval before querying. */
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (!g_atomic_int_get (&seen_first) && g_get_monotonic_time () < deadline)
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  gboolean streamed = g_atomic_int_get (&seen_first);

  /* Capture the negotiated interval and the LATENCY query result, but do not
     assert yet: a fail_unless() here would longjmp past the pipeline teardown
     below and leave the feeder thread running, hanging the test until ctest's
     timeout. Record, tear down, then assert last. */
  GstClockTime fi = (GstClockTime) GST_LIBUVC_H264_SRC (src)->frame_interval;

  gboolean queried = FALSE, live = FALSE;
  GstClockTime min_latency = GST_CLOCK_TIME_NONE, max_latency = GST_CLOCK_TIME_NONE;
  if (streamed) {
    GstQuery *q = gst_query_new_latency ();
    GstPad *srcpad = gst_element_get_static_pad (src, "src");
    queried = gst_pad_query (srcpad, q);
    gst_object_unref (srcpad);
    if (queried)
      gst_query_parse_latency (q, &live, &min_latency, &max_latency);
    gst_query_unref (q);
  }

  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 30 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  GstMessageType msg_type = msg ? GST_MESSAGE_TYPE (msg) : GST_MESSAGE_UNKNOWN;
  if (msg)
    gst_message_unref (msg);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (streamed, "no buffer arrived; mock idle");
  fail_unless (fi == EXPECTED_FRAME_INTERVAL_NS,
      "frame_interval %" G_GUINT64_FORMAT " != expected %" G_GUINT64_FORMAT,
      (guint64) fi, (guint64) EXPECTED_FRAME_INTERVAL_NS);
  fail_unless (queried, "src pad refused the LATENCY query");
  fail_unless (live, "LATENCY query did not report a live source");
  fail_unless (min_latency == fi,
      "min latency %" G_GUINT64_FORMAT " != frame interval %" G_GUINT64_FORMAT,
      (guint64) min_latency, (guint64) fi);
  fail_unless (msg_type == GST_MESSAGE_EOS,
      "expected EOS, got %s", gst_message_type_get_name (msg_type));
  fail_unless (g_atomic_int_get (&offset_checked) >= 10,
      "only %d buffers carried an OFFSET", g_atomic_int_get (&offset_checked));
  fail_unless (!g_atomic_int_get (&offset_violation),
      "GST_BUFFER_OFFSET was not a strict +1 sequence (or OFFSET_END mismatch)");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* spspps_write_on_change: cache written only when parameter sets change     */
/* ------------------------------------------------------------------------- */

/* Append an Annex-B NAL (4-byte start code + header byte + payload). */
static size_t
put_nal (uint8_t * b, uint8_t header, uint8_t fill, size_t payload_len)
{
  b[0] = 0; b[1] = 0; b[2] = 0; b[3] = 1; b[4] = header;
  memset (b + 5, fill, payload_len);
  return 5 + payload_len;
}

/* SPS(type 7) + PPS(type 8) + IDR(type 5). sps_fill selects the SPS payload so
 * two access units can carry identical or differing parameter sets. */
static size_t
craft_au (uint8_t * buf, uint8_t sps_fill)
{
  size_t n = 0;
  n += put_nal (buf + n, 0x67, sps_fill, 12);
  n += put_nal (buf + n, 0x68, 0xAB, 4);
  n += put_nal (buf + n, 0x65, 0xCD, 48);
  return n;
}

static void
drain_queue (GstLibuvcH264Src * self)
{
  GstBuffer *b;
  while ((b = g_async_queue_try_pop (self->frame_queue)) != NULL)
    gst_buffer_unref (b);
}

static gboolean
cache_exists (const char *path)
{
  struct stat st;
  return stat (path, &st) == 0;
}

static void
feed_one (GstLibuvcH264Src * self, uint8_t * buf, size_t len)
{
  uvc_frame_t frame;
  memset (&frame, 0, sizeof (frame));
  frame.data = buf;
  frame.data_bytes = len;
  frame.frame_format = UVC_FRAME_FORMAT_H264;
  frame_callback (&frame, self);
  drain_queue (self);
}

GST_START_TEST (test_spspps_write_on_change)
{
  register_element ();

  GstLibuvcH264Src *self =
      GST_LIBUVC_H264_SRC (g_object_new (GST_TYPE_LIBUVC_H264_SRC, NULL));
  self->frame_format = UVC_FRAME_FORMAT_H264;
  self->negotiated_width = 1920;
  self->negotiated_height = 1080;
  self->frame_interval = EXPECTED_FRAME_INTERVAL_NS;
  self->base_time = 0;          /* skip the first-frame base_time latch */
  self->clock = gst_system_clock_obtain ();

  char path[4096];
  fail_unless (spspps_build_path (path, sizeof (path), g_getenv ("HOME"),
          self->index, 0, 1920, 1080) > 0, "could not build cache path");
  unlink (path);

  uint8_t buf[512];

  /* First parameter set differs from the init() default, so it is written. */
  feed_one (self, buf, craft_au (buf, 0x11));
  fail_unless (cache_exists (path), "cache not written for the first SPS");

  /* Identical parameter set: delete the file and prove it is NOT rewritten. */
  unlink (path);
  feed_one (self, buf, craft_au (buf, 0x11));
  fail_if (cache_exists (path),
      "cache rewritten for an unchanged SPS (L10 regression)");

  /* Changed SPS: the cache must be written again. */
  feed_one (self, buf, craft_au (buf, 0x22));
  fail_unless (cache_exists (path), "cache not written after the SPS changed");

  unlink (path);
  gst_object_unref (self->clock);
  self->clock = NULL;
  gst_object_unref (self);
}

GST_END_TEST;

static Suite *
live_source_suite (void)
{
  Suite *s = suite_create ("libuvch264src-live-source");

  TCase *tc_polish = tcase_create ("live_source_polish");
  tcase_set_timeout (tc_polish, 60);
  tcase_add_test (tc_polish, test_live_source_polish);
  suite_add_tcase (s, tc_polish);

  TCase *tc_write = tcase_create ("spspps_write_on_change");
  tcase_set_timeout (tc_write, 60);
  tcase_add_test (tc_write, test_spspps_write_on_change);
  suite_add_tcase (s, tc_write);

  return s;
}

GST_CHECK_MAIN (live_source);
