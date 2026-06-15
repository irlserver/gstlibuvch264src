/* PTS clamp (M4) and restart-state reset (M5) tests for frame_callback().
 *
 * Like test_pts_thread_safety.c, this statically links the element translation
 * units, the libuvc mock, and the driver into ONE executable and registers the
 * element type directly, so the mock feeder thread drives the real
 * frame_callback() PTS arithmetic in the test process.
 *
 *   test_pts_monotonic  The mock feeds access units far faster (~2 ms apart)
 *                       than the negotiated 30 fps frame interval (~33 ms), so
 *                       the running PTS outruns the wall clock and the resync
 *                       offset goes strongly negative once the estimator kicks
 *                       in. Without the M4 clamp the PTS jumps backwards (and a
 *                       deeper underflow would wrap the unsigned PTS, producing
 *                       a huge DURATION). Assert every buffer PTS is strictly
 *                       increasing and every DURATION is non-zero and sane.
 *
 *   test_restart_idr    The mock leads each stream with a few bare non-IDR
 *                       slices before the first IDR. Stream once to latch
 *                       had_idr, restart (NULL -> PLAYING), and assert the first
 *                       buffer after the restart is never a forwarded non-IDR
 *                       slice. Without the M5 reset in start(), the stale
 *                       had_idr forwards that slice before a fresh IDR.
 *
 * GST_CHECKS selects a single test per ctest invocation (see tests/CMakeLists.txt).
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

/* H.264 NAL unit types (the mock streams H.264 by default). */
#define NAL_NON_IDR 1
#define NAL_IDR     5
#define NAL_SPS     7

/* The harness blanks GST_PLUGIN_SYSTEM_PATH; load just core-elements so fakesink
 * is available without scanning unrelated plugins. */
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

/* The element is linked in, not loaded from a plugin .so; register its type once
 * so gst_element_factory_make() finds it. */
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

static GstElement *
build_pipeline (GstElement ** src_out, GstPadProbeCallback probe)
{
  GstElement *pipeline = gst_pipeline_new ("pts-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");

  fail_unless (pipeline != NULL && src != NULL && sink != NULL,
      "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL, "fakesink has no sink pad");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, probe, NULL, NULL);
  gst_object_unref (pad);

  /* The bin owns src; the caller borrows the pointer while the pipeline lives. */
  if (src_out != NULL)
    *src_out = src;
  return pipeline;
}

/* ------------------------------------------------------------------------- */
/* test_pts_monotonic                                                        */
/* ------------------------------------------------------------------------- */

/* A single frame is at most ~33 ms; any DURATION near a second means the
 * unsigned PTS subtraction wrapped (an underflow the M4 clamp must prevent). */
#define DURATION_SANE_MAX GST_SECOND
#define MONOTONIC_BUFFERS 200

/* Read on the streaming (sink chain) thread only; the test thread reads them
 * after the state change to NULL, which provides the memory barrier. */
static gint pts_violation;       /* atomic: a PTS failed to strictly increase */
static gint duration_violation;  /* atomic: a DURATION was zero or wrapped huge */
static gint pts_checked;         /* atomic: buffers with a valid PTS examined */
static GstClockTime pts_prev;    /* streaming-thread local */
static gboolean pts_have_prev;   /* streaming-thread local */

static GstPadProbeReturn
pts_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (!(GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER))
    return GST_PAD_PROBE_OK;

  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  GstClockTime pts = GST_BUFFER_PTS (buf);
  GstClockTime dur = GST_BUFFER_DURATION (buf);

  if (GST_CLOCK_TIME_IS_VALID (pts)) {
    if (pts_have_prev && pts <= pts_prev)
      g_atomic_int_set (&pts_violation, 1);
    pts_prev = pts;
    pts_have_prev = TRUE;
    g_atomic_int_inc (&pts_checked);
  }
  if (GST_CLOCK_TIME_IS_VALID (dur) && (dur == 0 || dur > DURATION_SANE_MAX))
    g_atomic_int_set (&duration_violation, 1);

  return GST_PAD_PROBE_OK;
}

GST_START_TEST (test_pts_monotonic)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();            /* VALID mode: every AU is SPS+PPS+IDR */

  g_atomic_int_set (&pts_violation, 0);
  g_atomic_int_set (&duration_violation, 0);
  g_atomic_int_set (&pts_checked, 0);
  pts_have_prev = FALSE;
  pts_prev = 0;

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src, pts_probe);
  /* num-buffers bounds the run and guarantees we cross the resync interval
   * (MIN_FRAMES_CALC_INTERVAL twice) where the negative offset is applied. */
  g_object_set (src, "num-buffers", (gint) MONOTONIC_BUFFERS, NULL);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 30 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  if (msg != NULL && GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *gerr = NULL;
    gchar *dbg = NULL;
    gst_message_parse_error (msg, &gerr, &dbg);
    fail ("pipeline errored instead of reaching EOS: %s (%s)",
        gerr ? gerr->message : "(none)", dbg ? dbg : "(no debug)");
    g_clear_error (&gerr);
    g_free (dbg);
  }
  fail_unless (msg != NULL,
      "timed out waiting for EOS - the mock never fed enough frames");
  fail_unless (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS,
      "expected EOS, got %s", GST_MESSAGE_TYPE_NAME (msg));
  gst_message_unref (msg);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  /* Need enough frames to cross the second resync interval, where the strongly
   * negative offset is applied; otherwise the clamp is never exercised. */
  fail_unless (g_atomic_int_get (&pts_checked) >= 2 * MIN_FRAMES_CALC_INTERVAL,
      "only %d PTS-bearing buffers; resync interval never crossed",
      g_atomic_int_get (&pts_checked));
  fail_unless (!g_atomic_int_get (&pts_violation),
      "PTS was not strictly monotonic (a resync offset drove it backwards)");
  fail_unless (!g_atomic_int_get (&duration_violation),
      "a DURATION was zero or wrapped huge (PTS underflow)");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_restart_idr                                                          */
/* ------------------------------------------------------------------------- */

/* Leading NAL type of the first buffer of a run, -1 until captured. */
static gint first_nal_type;

/* Annex-B: 00 00 00 01 then the NAL header byte (H.264 type = byte & 0x1F). */
static gint
leading_nal_type (GstBuffer * buf)
{
  GstMapInfo map;
  gint type = -1;
  if (gst_buffer_map (buf, &map, GST_MAP_READ)) {
    if (map.size >= 5 && map.data[0] == 0 && map.data[1] == 0 &&
        map.data[2] == 0 && map.data[3] == 1) {
      type = map.data[4] & 0x1F;
    }
    gst_buffer_unmap (buf, &map);
  }
  return type;
}

static GstPadProbeReturn
first_nal_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    gint t = leading_nal_type (GST_PAD_PROBE_INFO_BUFFER (info));
    /* Record only the first parseable buffer of the run. */
    g_atomic_int_compare_and_exchange (&first_nal_type, -1, t);
  }
  return GST_PAD_PROBE_OK;
}

/* Drive PLAYING and wait until the first buffer of the run has been captured. */
static gint
play_until_first_buffer (GstElement * pipeline)
{
  g_atomic_int_set (&first_nal_type, -1);
  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (g_atomic_int_get (&first_nal_type) == -1
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  return g_atomic_int_get (&first_nal_type);
}

GST_START_TEST (test_restart_idr)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
  /* Lead each stream with bare non-IDR slices before the first IDR. */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_NONIDR_LEAD);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src, first_nal_probe);

  /* Run 1 - fresh start. had_idr is FALSE from init, so the leading non-IDR
   * slices are dropped and the first emitted buffer is an SPS-prefixed IDR.
   * This also latches had_idr = TRUE for the restart below. */
  gint run1 = play_until_first_buffer (pipeline);
  fail_unless (run1 == NAL_SPS || run1 == NAL_IDR,
      "fresh start should drop non-IDR; first buffer NAL type = %d", run1);
  gst_element_set_state (pipeline, GST_STATE_NULL);

  /* Run 2 - restart. start() must reset had_idr so the leading non-IDR slices
   * are dropped again until a fresh IDR. Without the reset, the stale had_idr
   * forwards the first non-IDR slice as the first buffer. */
  gint run2 = play_until_first_buffer (pipeline);
  gst_element_set_state (pipeline, GST_STATE_NULL);

  gst_object_unref (pipeline);

  fail_unless (run2 != NAL_NON_IDR,
      "after restart a non-IDR buffer was forwarded before a fresh IDR "
      "(had_idr not reset in start())");
  fail_unless (run2 == NAL_SPS || run2 == NAL_IDR,
      "after restart the first buffer should be an IDR/SPS; NAL type = %d",
      run2);
}

GST_END_TEST;

static Suite *
pts_monotonic_suite (void)
{
  Suite *s = suite_create ("libuvch264src-pts-monotonic");

  TCase *tc_mono = tcase_create ("pts_monotonic");
  tcase_set_timeout (tc_mono, 60);
  tcase_add_test (tc_mono, test_pts_monotonic);
  suite_add_tcase (s, tc_mono);

  TCase *tc_restart = tcase_create ("restart_idr");
  tcase_set_timeout (tc_restart, 60);
  tcase_add_test (tc_restart, test_restart_idr);
  suite_add_tcase (s, tc_restart);

  return s;
}

GST_CHECK_MAIN (pts_monotonic);
