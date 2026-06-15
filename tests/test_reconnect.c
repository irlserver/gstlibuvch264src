/* Disconnect-detection and opt-in reconnect tests for the libuvch264src element
 * (Task 18). Like test_pts_monotonic.c, this statically links the element
 * translation units, the libuvc mock, and the driver into ONE executable and
 * registers the element type directly, so the mock feeder drives the real
 * create() disconnect path in the test process and the mock's open counter is
 * observable in-process (mock_uvc_open_count).
 *
 *   test_disconnect_error   With reconnect off (the default), a device that goes
 *                           silent mid-stream must surface as a RESOURCE/READ bus
 *                           error. The mock's DISCONNECT mode delivers one frame
 *                           then stops feeding (real libuvc passes no NULL frame
 *                           on unplug in callback mode), so create() infers the
 *                           disconnect from sustained silence and errors out.
 *
 *   test_reconnect_resume   With reconnect=TRUE, the same silent-source scenario
 *                           must NOT error: the element tears the dead handle
 *                           down and reopens it. The test simulates a successful
 *                           replug by switching the mock to a healthy feed once
 *                           the first frame has been seen, then asserts the
 *                           device was reopened (open count grew) and frames
 *                           resumed flowing.
 *
 * GST_CHECKS selects a single test per ctest invocation (see tests/CMakeLists.txt).
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

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

static gint buffers_seen;       /* atomic: buffers that reached the sink */

static GstPadProbeReturn
count_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_inc (&buffers_seen);
  return GST_PAD_PROBE_OK;
}

static GstElement *
build_pipeline (GstElement ** src_out)
{
  GstElement *pipeline = gst_pipeline_new ("reconnect-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");

  fail_unless (pipeline != NULL && src != NULL && sink != NULL,
      "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL, "fakesink has no sink pad");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer_probe, NULL,
      NULL);
  gst_object_unref (pad);

  /* The bin owns src; the caller borrows the pointer while the pipeline lives. */
  if (src_out != NULL)
    *src_out = src;
  return pipeline;
}

/* ------------------------------------------------------------------------- */
/* test_disconnect_error                                                     */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_disconnect_error)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
  /* One frame, then the feed goes silent - the mock's stand-in for an unplug. */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_DISCONNECT);
  mock_uvc_set_max_frames (1);

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  /* reconnect defaults to FALSE, so the disconnect must surface as an error. */

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* DISCONNECT_TIMEOUT_COUNT (5) consecutive 1 s pop timeouts must elapse before
   * create() declares the disconnect, so allow generous headroom past ~5 s. */
  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 12 * GST_SECOND,
      GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  gboolean is_disconnect_error = FALSE;
  if (msg != NULL && GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *gerr = NULL;
    gchar *dbg = NULL;
    gst_message_parse_error (msg, &gerr, &dbg);
    /* The element posts its RESOURCE/READ disconnect error before returning
     * GST_FLOW_ERROR, so it is the first ERROR on the bus. */
    is_disconnect_error =
        g_error_matches (gerr, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ);
    g_clear_error (&gerr);
    g_free (dbg);
  }
  if (msg != NULL)
    gst_message_unref (msg);
  gst_object_unref (bus);

  /* Tear down before asserting: a failed fail_unless() longjmps past teardown
   * under CK_FORK=no, leaving the streaming task alive until the ctest timeout. */
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (msg != NULL,
      "expected a disconnect error within 12 s, got none "
      "(create() never detected the silent source)");
  fail_unless (is_disconnect_error,
      "expected GST_RESOURCE_ERROR_READ on disconnect; got a different message");
}

GST_END_TEST;

/* ------------------------------------------------------------------------- */
/* test_reconnect_resume                                                     */
/* ------------------------------------------------------------------------- */

GST_START_TEST (test_reconnect_resume)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
  /* Start in DISCONNECT mode (one frame, then silence) so the first feeder goes
   * quiet and create() detects a disconnect. */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_DISCONNECT);
  mock_uvc_set_max_frames (1);

  g_atomic_int_set (&buffers_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src);
  g_object_set (src, "reconnect", TRUE, NULL);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* Wait for the first (pre-disconnect) frame. Its arrival proves the first
   * feeder already ran in DISCONNECT mode and has now gone silent, so switching
   * the mock to a healthy feed below only affects the reopened stream. */
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (g_atomic_int_get (&buffers_seen) < 1
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  fail_unless (g_atomic_int_get (&buffers_seen) >= 1,
      "the initial stream never delivered a frame");

  /* Simulate a successful replug: the reopened device feeds healthy frames
   * continuously. The reconnect feeder reads this when uvc_start_streaming
   * spawns it, so the resumed stream runs without going silent again. */
  mock_uvc_set_frame_mode (MOCK_UVC_FRAME_VALID);
  mock_uvc_set_max_frames (0);

  gint baseline = g_atomic_int_get (&buffers_seen);

  /* The element should detect the disconnect (~5 s), back off (~1 s), reopen,
   * and resume. Wait for frames to flow well past the single pre-disconnect one
   * AND for a second uvc_open() to confirm a real reopen happened. Fail fast if
   * an error reaches the bus instead (reconnect should suppress it). */
  GstBus *bus = gst_element_get_bus (pipeline);
  gboolean resumed = FALSE;
  gboolean errored = FALSE;
  deadline = g_get_monotonic_time () + 40 * G_TIME_SPAN_SECOND;
  while (g_get_monotonic_time () < deadline) {
    GstMessage *msg =
        gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (msg != NULL) {
      errored = (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
      gst_message_unref (msg);
      break;
    }
    if (g_atomic_int_get (&buffers_seen) >= baseline + 5
        && mock_uvc_open_count () >= 2) {
      resumed = TRUE;
      break;
    }
    g_usleep (20 * G_TIME_SPAN_MILLISECOND);
  }
  gint final_buffers = g_atomic_int_get (&buffers_seen);
  gint open_count = mock_uvc_open_count ();
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (!errored,
      "reconnect=TRUE should suppress the disconnect error, but the pipeline "
      "errored out");
  fail_unless (open_count >= 2,
      "expected the device to be reopened (open count >= 2), got %d", open_count);
  fail_unless (resumed && final_buffers >= baseline + 5,
      "stream did not resume after reconnect: %d buffers (baseline %d), "
      "open count %d", final_buffers, baseline, open_count);
}

GST_END_TEST;

static Suite *
reconnect_suite (void)
{
  Suite *s = suite_create ("libuvch264src-reconnect");

  TCase *tc_disc = tcase_create ("disconnect_error");
  tcase_set_timeout (tc_disc, 30);
  tcase_add_test (tc_disc, test_disconnect_error);
  suite_add_tcase (s, tc_disc);

  TCase *tc_recon = tcase_create ("reconnect_resume");
  tcase_set_timeout (tc_recon, 60);
  tcase_add_test (tc_recon, test_reconnect_resume);
  suite_add_tcase (s, tc_recon);

  return s;
}

GST_CHECK_MAIN (reconnect);
