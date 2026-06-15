/* ASAN regression test for the SPS/PPS/VPS bounds clamp (C1, heap overflow).
 *
 * The mock feeder (MOCK_UVC_FRAME_MODE=oversized_sps) delivers access units
 * whose SPS NAL is far larger than the element's fixed 1024 B SPS buffer - large
 * enough to run off the END of the whole GObject instance allocation. Before the
 * clamp, frame_callback() memcpy'd it straight into self->sps[SPSPPSBUFSZ],
 * smashing the heap; under ASan (with G_SLICE=always-malloc, set by the ctest
 * env, so each instance is a plain malloc with a trailing redzone) that aborts
 * the process and this test goes RED.
 *
 * With the clamp the oversized SPS is dropped with a GST_WARNING and the stream
 * keeps flowing: every access unit still yields its IDR buffer, the element
 * reaches num-buffers and emits EOS, and ASan stays clean. The assertions below
 * therefore prove three things at once - no overflow (ASan silent), frames still
 * delivered (drop was non-fatal), and the drop took the guarded warning path.
 *
 * Hardware-independent: every libuvc call resolves to tests/mock_libuvc.c.
 */

#include <gst/check/gstcheck.h>
#include <string.h>

#define N_BUFFERS 10

static gint buffer_count;
static gint oversized_warning_seen;

static void
catch_drop_warning (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer user_data)
{
  (void) category;
  (void) file;
  (void) function;
  (void) line;
  (void) object;
  (void) user_data;
  if (level > GST_LEVEL_WARNING)
    return;
  const gchar *text = gst_debug_message_get (message);
  if (text != NULL && strstr (text, "oversized") != NULL)
    g_atomic_int_set (&oversized_warning_seen, 1);
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

GST_START_TEST (test_oversized_sps_is_clamped_no_overflow)
{
  g_atomic_int_set (&buffer_count, 0);
  g_atomic_int_set (&oversized_warning_seen, 0);

  /* Route the element's WARNING through our sink so we can prove the oversized
   * NAL was dropped via the guarded path, not merely tolerated. */
  gst_debug_set_active (TRUE);
  gst_debug_set_default_threshold (GST_LEVEL_WARNING);
  gst_debug_set_threshold_for_name ("libuvch264src", GST_LEVEL_WARNING);
  gst_debug_add_log_function (catch_drop_warning, NULL, NULL);

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
      "timed out waiting for EOS - the oversized SPS path likely crashed");
  fail_unless (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS,
      "expected EOS, got %s", GST_MESSAGE_TYPE_NAME (msg));
  gst_message_unref (msg);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (g_atomic_int_get (&buffer_count) == N_BUFFERS,
      "expected %d buffers after dropping the oversized SPS, got %d",
      N_BUFFERS, g_atomic_int_get (&buffer_count));
  fail_unless (g_atomic_int_get (&oversized_warning_seen) == 1,
      "expected a GST_WARNING that the oversized SPS NAL was dropped");
}

GST_END_TEST;

static Suite *
sps_bounds_suite (void)
{
  Suite *s = suite_create ("libuvch264src-sps-bounds");
  TCase *tc = tcase_create ("bounds");

  tcase_set_timeout (tc, 60);
  suite_add_tcase (s, tc);
  tcase_add_test (tc, test_oversized_sps_is_clamped_no_overflow);

  return s;
}

GST_CHECK_MAIN (sps_bounds);
