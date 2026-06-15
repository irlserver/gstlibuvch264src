/* Lifecycle regression tests for the libuvch264src element, run against the
 * mock-backed plugin (tests/mock_libuvc.c) so no UVC hardware is touched.
 *
 *   test_unlock_shutdown  A create() blocked on a silent source must be
 *                         interrupted by the unlock() vmethod, so a state change
 *                         to NULL tears the element down promptly instead of
 *                         deadlocking. Driven with the mock in DISCONNECT mode
 *                         (one frame, then silence) so create() genuinely blocks.
 *
 *   test_mutex_restart    Repeated start()/stop() cycles on one element must not
 *                         corrupt control_mutex; it is initialised once in init()
 *                         and cleared only at finalize(). The bug cleared it in
 *                         stop(), leaving the next cycle's control thread bound to
 *                         a destroyed mutex.
 *
 * GST_CHECKS selects a single test per ctest invocation (see tests/CMakeLists.txt),
 * so each case gets its own mock configuration.
 */

#include <gst/check/gstcheck.h>

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

/* Set from the streaming thread once the first buffer reaches the sink. Atomic
 * so ThreadSanitizer sees the cross-thread handshake: a GMutex/GCond pair would
 * synchronise inside uninstrumented GLib and trip a false-positive race. */
static gint first_buffer_seen;

static GstPadProbeReturn
first_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_set (&first_buffer_seen, 1);
  return GST_PAD_PROBE_OK;
}

GST_START_TEST (test_unlock_shutdown)
{
  load_core_elements ();

  g_atomic_int_set (&first_buffer_seen, 0);

  GError *err = NULL;
  GstElement *pipeline = gst_parse_launch (
      "libuvch264src ! fakesink sync=false name=sink", &err);
  fail_unless (err == NULL, "pipeline parse failed: %s",
      err ? err->message : "(unknown)");
  fail_unless (pipeline != NULL, "no pipeline produced");

  GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  fail_unless (sink != NULL, "fakesink not found in pipeline");
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL, "fakesink has no sink pad");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, first_buffer_probe, NULL,
      NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* Wait for the single DISCONNECT-mode frame to be pushed: create() has then
   * looped back onto an empty queue and is genuinely blocked. Bounded so the
   * test still proceeds if no frame ever arrives - create() is blocked from the
   * start in that case too, which is equally what unlock() must interrupt. */
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
  while (!g_atomic_int_get (&first_buffer_seen)
      && g_get_monotonic_time () < deadline) {
    g_usleep (5 * G_TIME_SPAN_MILLISECOND);
  }

  /* The crux: tearing down must not block on the stalled create(). With a
   * broken unlock() this hangs until ctest's TIMEOUT kills the run; the
   * sentinel wake must return create() far faster than its 1 s pop timeout. */
  gint64 t0 = g_get_monotonic_time ();
  GstStateChangeReturn r = gst_element_set_state (pipeline, GST_STATE_NULL);
  fail_unless (r != GST_STATE_CHANGE_FAILURE, "could not set pipeline to NULL");
  gst_element_get_state (pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
  gint64 dt = g_get_monotonic_time () - t0;

  fail_unless (dt < 500 * G_TIME_SPAN_MILLISECOND,
      "blocked create() shutdown took %" G_GINT64_FORMAT " us, expected < 500 ms",
      dt);

  gst_object_unref (pipeline);
}

GST_END_TEST;

GST_START_TEST (test_mutex_restart)
{
  load_core_elements ();

  GError *err = NULL;
  GstElement *pipeline = gst_parse_launch (
      "libuvch264src ! fakesink sync=false name=sink", &err);
  fail_unless (err == NULL, "pipeline parse failed: %s",
      err ? err->message : "(unknown)");
  fail_unless (pipeline != NULL, "no pipeline produced");

  /* Drive start()/stop() repeatedly on the SAME element. Each PLAYING runs
   * start() (spawning the control thread that owns control_mutex); each NULL
   * runs stop() (joining it). The bug cleared control_mutex in stop(), so the
   * second cycle ran against a destroyed mutex. Completing every cycle cleanly,
   * especially under the sanitizers, is the assertion. */
  const int cycles = 5;
  for (int i = 0; i < cycles; i++) {
    fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE, "cycle %d: could not set PLAYING", i);
    /* Let start() bring the control thread fully up before tearing it down. */
    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
    fail_unless (gst_element_set_state (pipeline, GST_STATE_NULL)
        != GST_STATE_CHANGE_FAILURE, "cycle %d: could not set NULL", i);
  }

  gst_object_unref (pipeline);
}

GST_END_TEST;

static Suite *
lifecycle_suite (void)
{
  Suite *s = suite_create ("libuvch264src-lifecycle");

  TCase *tc_unlock = tcase_create ("unlock_shutdown");
  tcase_set_timeout (tc_unlock, 30);
  tcase_add_test (tc_unlock, test_unlock_shutdown);
  suite_add_tcase (s, tc_unlock);

  TCase *tc_mutex = tcase_create ("mutex_restart");
  tcase_set_timeout (tc_mutex, 60);
  tcase_add_test (tc_mutex, test_mutex_restart);
  suite_add_tcase (s, tc_mutex);

  return s;
}

GST_CHECK_MAIN (lifecycle);
