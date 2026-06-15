/* Consolidated functional suite for the libuvch264src element, run end to end
 * against the mock-backed plugin (tests/mock_libuvc.c) so no UVC hardware is
 * touched. It fills the gaps left by the per-task suites rather than repeating
 * them: the per-task tests already cover device-open failure (device_zero),
 * unlock/flush (unlock_shutdown), stop/restart (mutex_restart, usb_teardown),
 * device selection (device_selector), SPS/PPS overflow (sps_bounds_asan),
 * thread-safety (pts_thread_safety_tsan), and negotiate() error edges
 * (negotiate_*). What was missing - and what this file adds - is the POSITIVE
 * caps-negotiation matrix (an H.264 device negotiates video/x-h264, an H.265
 * device negotiates video/x-h265, frames flow in both) and frame delivery under
 * downstream BACKPRESSURE (a slow consumer must not drop frames or deadlock).
 *
 * The cases share the mock-backed plugin and isolated env of test_mock_smoke;
 * GST_CHECKS selects one per ctest invocation so each gets its own mock config
 * (the H.265 case sets MOCK_UVC_FRAME_FORMAT=H265 in its ctest environment).
 *
 * No UVC hardware is touched: every libuvc call resolves to the mock.
 */

#include <gst/check/gstcheck.h>

static gint buffer_count;
static gchar *negotiated_caps_name; /* media type seen on the first CAPS event */

/* Capture the media type the element negotiated downstream (video/x-h264 vs
 * video/x-h265) the first time a CAPS event crosses the sink pad. */
static GstPadProbeReturn
caps_event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
  if (GST_EVENT_TYPE (ev) == GST_EVENT_CAPS && negotiated_caps_name == NULL) {
    GstCaps *caps = NULL;
    gst_event_parse_caps (ev, &caps);
    if (caps != NULL && gst_caps_get_size (caps) > 0) {
      GstStructure *s = gst_caps_get_structure (caps, 0);
      negotiated_caps_name = g_strdup (gst_structure_get_name (s));
    }
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
count_buffers_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_inc (&buffer_count);
  return GST_PAD_PROBE_OK;
}

/* A slow consumer: sleep on every buffer so the streaming thread blocks in its
 * downstream push and backpressure propagates up into the element. */
static GstPadProbeReturn
slow_sink_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    g_atomic_int_inc (&buffer_count);
    g_usleep (8 * G_TIME_SPAN_MILLISECOND);
  }
  return GST_PAD_PROBE_OK;
}

/* The harness blanks GST_PLUGIN_SYSTEM_PATH; load just core-elements so fakesink
 * is available without scanning unrelated plugins (which trip the sanitizers). */
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

/* Drive `num_buffers` of streaming through `libuvch264src ! fakesink`, attaching
 * `buf_probe` (and always the caps probe) to the sink pad, and run to EOS.
 * Returns the buffers counted; the negotiated media type lands in
 * negotiated_caps_name. Fails the test on parse/state/timeout/error. */
static gint
run_streaming (gint num_buffers, GstPadProbeCallback buf_probe)
{
  load_core_elements ();

  g_atomic_int_set (&buffer_count, 0);
  g_clear_pointer (&negotiated_caps_name, g_free);

  gchar *desc = g_strdup_printf (
      "libuvch264src num-buffers=%d ! fakesink sync=false name=sink",
      num_buffers);
  GError *err = NULL;
  GstElement *pipeline = gst_parse_launch (desc, &err);
  g_free (desc);
  fail_unless (err == NULL, "pipeline parse failed: %s",
      err ? err->message : "(unknown)");
  fail_unless (pipeline != NULL, "no pipeline produced");

  GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  fail_unless (sink != NULL, "fakesink not found in pipeline");
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL, "fakesink has no sink pad");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, caps_event_probe,
      NULL, NULL);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buf_probe, NULL, NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);

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
  fail_unless (msg != NULL, "timed out waiting for EOS");
  fail_unless (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS,
      "expected EOS, got %s", GST_MESSAGE_TYPE_NAME (msg));
  gst_message_unref (msg);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  return g_atomic_int_get (&buffer_count);
}

/* An H.264 device must negotiate video/x-h264 and deliver every frame. */
GST_START_TEST (test_caps_h264)
{
  gint got = run_streaming (10, count_buffers_probe);

  fail_unless (negotiated_caps_name != NULL, "no CAPS event reached the sink");
  fail_unless_equals_string (negotiated_caps_name, "video/x-h264");
  fail_unless (got == 10, "expected 10 buffers, got %d", got);

  g_clear_pointer (&negotiated_caps_name, g_free);
}

GST_END_TEST;

/* The same path against an H.265 device (set via MOCK_UVC_FRAME_FORMAT=H265 in
 * the ctest env) must negotiate video/x-h265 - the dual-codec branch the H.264
 * smoke test never reaches. */
GST_START_TEST (test_caps_h265)
{
  gint got = run_streaming (10, count_buffers_probe);

  fail_unless (negotiated_caps_name != NULL, "no CAPS event reached the sink");
  fail_unless_equals_string (negotiated_caps_name, "video/x-h265");
  fail_unless (got == 10, "expected 10 buffers, got %d", got);

  g_clear_pointer (&negotiated_caps_name, g_free);
}

GST_END_TEST;

/* A slow downstream consumer applies backpressure: the element's push blocks,
 * which must stall - not drop - the upstream feed. Every requested buffer must
 * still arrive and the pipeline must reach EOS without deadlocking. */
GST_START_TEST (test_backpressure)
{
  const gint n = 30;
  gint got = run_streaming (n, slow_sink_probe);

  fail_unless (got == n,
      "backpressure dropped frames: expected %d buffers, got %d", n, got);

  g_clear_pointer (&negotiated_caps_name, g_free);
}

GST_END_TEST;

static Suite *
functional_suite (void)
{
  Suite *s = suite_create ("libuvch264src-functional");

  TCase *tc_h264 = tcase_create ("caps_h264");
  tcase_set_timeout (tc_h264, 60);
  tcase_add_test (tc_h264, test_caps_h264);
  suite_add_tcase (s, tc_h264);

  TCase *tc_h265 = tcase_create ("caps_h265");
  tcase_set_timeout (tc_h265, 60);
  tcase_add_test (tc_h265, test_caps_h265);
  suite_add_tcase (s, tc_h265);

  TCase *tc_bp = tcase_create ("backpressure");
  tcase_set_timeout (tc_bp, 60);
  tcase_add_test (tc_bp, test_backpressure);
  suite_add_tcase (s, tc_bp);

  return s;
}

GST_CHECK_MAIN (functional);
