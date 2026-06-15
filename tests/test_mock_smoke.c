/* Hardware-independent streaming smoke test for the libuvch264src element.
 *
 * Unlike test_plugin_load.c (which only touches registration/introspection),
 * this drives the element through its full READY->PAUSED->PLAYING path against
 * the mock libuvc (tests/mock_libuvc.c) linked into a mock-backed copy of the
 * plugin. The mock's feeder thread crafts H.264 access units (SPS + PPS + IDR
 * with 4-byte start codes); the element parses them, assembles GstBuffers, and
 * pushes them downstream.
 *
 * The element's GstBaseSrc "num-buffers" property bounds the run: after 10
 * buffers the base class emits EOS, which is how the test terminates. A sink-pad
 * probe counts buffers so we can assert the mock actually fed the element and
 * the element actually popped and pushed them.
 *
 * No UVC hardware is touched: every libuvc call resolves to the mock.
 */

#include <gst/check/gstcheck.h>

#define N_BUFFERS 10

static gint buffer_count;

static GstPadProbeReturn
count_buffers_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_inc (&buffer_count);
  return GST_PAD_PROBE_OK;
}

GST_START_TEST (test_mock_feeds_frames_element_pops_them)
{
  g_atomic_int_set (&buffer_count, 0);

  /* The harness runs with a blanked GST_PLUGIN_SYSTEM_PATH so the broad system
   * scan never pulls in unrelated third-party plugins (which trip the
   * sanitizers). Load just the core-elements plugin explicitly so fakesink is
   * available without scanning everything. */
  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }

  GError *err = NULL;
  GstElement *pipeline = gst_parse_launch (
      "libuvch264src num-buffers=" G_STRINGIFY (N_BUFFERS)
      " ! fakesink sync=false name=sink", &err);
  fail_unless (err == NULL, "pipeline parse failed: %s",
      err ? err->message : "(unknown)");
  fail_unless (pipeline != NULL, "no pipeline produced");

  GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  fail_unless (sink != NULL, "fakesink not found in pipeline");
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL, "fakesink has no sink pad");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffers_probe, NULL,
      NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, 10 * GST_SECOND,
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

  fail_unless (g_atomic_int_get (&buffer_count) == N_BUFFERS,
      "expected %d buffers from the mock feeder, got %d", N_BUFFERS,
      g_atomic_int_get (&buffer_count));
}

GST_END_TEST;

static Suite *
mock_smoke_suite (void)
{
  Suite *s = suite_create ("libuvch264src-mock-smoke");
  TCase *tc = tcase_create ("streaming");

  /* Generous wall-clock bound; the feeder delivers ~500 fps so EOS lands fast,
   * but sanitizer-instrumented builds run much slower. */
  tcase_set_timeout (tc, 60);
  suite_add_tcase (s, tc);
  tcase_add_test (tc, test_mock_feeds_frames_element_pops_them);

  return s;
}

GST_CHECK_MAIN (mock_smoke);
