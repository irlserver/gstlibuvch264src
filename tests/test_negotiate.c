/* negotiate() regression tests for the libuvch264src element.
 *
 * Like test_device_select.c, the element TUs, the libuvc mock, and the driver
 * are linked into ONE statically-registered executable so the mock's format
 * descriptor can be shaped in-process via mock_uvc_set_format_mode(). Each
 * gst-check test is surfaced as its own ctest entry through GST_CHECKS.
 *
 * Covered (Task 13):
 *   test_negotiate_leak            N renegotiations leak no GstCaps (LSAN). The
 *                                  working caps always leaked, and the chosen
 *                                  caps leaked on success too (set_caps takes its
 *                                  own ref) - the M3 cleanup path frees both.
 *   test_negotiate_zero_format     a device with no H264/H265 descriptor posts a
 *                                  RESOURCE bus error instead of streaming.
 *   test_negotiate_framerate_zero  a 0 fps descriptor must not SIGFPE on the
 *                                  1e9 / framerate division (H4).
 *   test_negotiate_zero_interval   a zero device frame interval must not SIGFPE
 *                                  on the 1e7 / interval division (H4).
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

/* LeakSanitizer is only present in the -fsanitize=address build; the recoverable
 * leak check is what gives the leak test its teeth there. In the plain build the
 * same test still drives the renegotiation path, just without the leak assertion. */
#if defined(__SANITIZE_ADDRESS__)
#  include <sanitizer/lsan_interface.h>
#  define NEGOTIATE_HAVE_LSAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    include <sanitizer/lsan_interface.h>
#    define NEGOTIATE_HAVE_LSAN 1
#  endif
#endif

#define WARMUP_CYCLES 3
#define MEASURED_CYCLES 30

static gint g_buffers_seen;

static GstPadProbeReturn
count_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_inc (&g_buffers_seen);
  return GST_PAD_PROBE_OK;
}

static void
setup (void)
{
  /* fakesink lives in coreelements; the harness blanks the system plugin path
   * for isolation, so load just that one plugin explicitly. */
  const gchar *core_plugin = g_getenv ("GST_COREELEMENTS_PLUGIN");
  if (core_plugin != NULL && *core_plugin != '\0') {
    GError *lerr = NULL;
    GstPlugin *p = gst_plugin_load_file (core_plugin, &lerr);
    fail_unless (p != NULL, "could not load core-elements plugin '%s': %s",
        core_plugin, lerr ? lerr->message : "(unknown)");
    gst_object_unref (p);
  }

  static gboolean registered = FALSE;
  if (!registered) {
    fail_unless (gst_element_register (NULL, "libuvch264src", GST_RANK_NONE,
            GST_TYPE_LIBUVC_H264_SRC), "failed to register libuvch264src");
    registered = TRUE;
  }

  mock_uvc_reset ();
  g_atomic_int_set (&g_buffers_seen, 0);
}

static GstElement *
build_pipeline (void)
{
  GstElement *pipeline = gst_pipeline_new ("negotiate-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");

  fail_unless (pipeline != NULL && src != NULL && sink != NULL,
      "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  g_object_set (src, "index", "0", NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");
  return pipeline;
}

/* One PLAYING -> (wait for a buffer) -> NULL round trip. Each PLAYING re-runs
 * start()+negotiate(), so a cycle is one full renegotiation. */
static void
renegotiate_cycle (GstElement * pipeline)
{
  int before = g_atomic_int_get (&g_buffers_seen);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING) !=
      GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
  while (g_atomic_int_get (&g_buffers_seen) <= before
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }

  gst_element_set_state (pipeline, GST_STATE_NULL);
}

GST_START_TEST (test_negotiate_leak)
{
  /* Pipeline construction and the warm-up cycles run inside the LSAN-disabled
   * window so GStreamer's one-time global allocations (element class init, caps
   * system, type classes, clocks) are excluded from the measurement; only the
   * per-renegotiation caps leak accumulates after __lsan_enable(). */
#ifdef NEGOTIATE_HAVE_LSAN
  __lsan_disable ();
#endif

  GstElement *pipeline = build_pipeline ();

  GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer_probe, NULL,
      NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);

  for (int i = 0; i < WARMUP_CYCLES; i++)
    renegotiate_cycle (pipeline);

#ifdef NEGOTIATE_HAVE_LSAN
  __lsan_enable ();
#endif

  for (int i = 0; i < MEASURED_CYCLES; i++)
    renegotiate_cycle (pipeline);

  /* Tear the pipeline down before checking so the pad's legitimate caps ref is
   * released; only orphaned (leaked) caps remain for LSAN to find. */
  gst_object_unref (pipeline);

#ifdef NEGOTIATE_HAVE_LSAN
  fail_if (__lsan_do_recoverable_leak_check (),
      "negotiate() leaked GstCaps across renegotiation");
#endif
}

GST_END_TEST;

/* Drive the pipeline to PLAYING and return the first fatal bus ERROR (negotiate
 * runs in the live source's streaming task, so the failure surfaces there).
 * Caller unrefs the message. */
static GstMessage *
play_and_wait_error (GstElement * pipeline)
{
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *msg =
      gst_bus_timed_pop_filtered (bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
  gst_object_unref (bus);
  return msg;
}

GST_START_TEST (test_negotiate_zero_format)
{
  mock_uvc_set_format_mode (MOCK_UVC_FORMAT_NO_CODEC);

  GstElement *pipeline = build_pipeline ();
  GstMessage *msg = play_and_wait_error (pipeline);

  GError *gerr = NULL;
  gboolean is_resource = FALSE;
  if (msg != NULL) {
    gchar *dbg = NULL;
    gst_message_parse_error (msg, &gerr, &dbg);
    g_free (dbg);
    is_resource = g_error_matches (gerr, GST_RESOURCE_ERROR,
        GST_RESOURCE_ERROR_SETTINGS);
    g_clear_error (&gerr);
    gst_message_unref (msg);
  }

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (msg != NULL,
      "expected a bus ERROR for a device with no H264/H265 format");
  fail_unless (is_resource,
      "expected RESOURCE/SETTINGS for the zero-format device");
}

GST_END_TEST;

GST_START_TEST (test_negotiate_framerate_zero)
{
  mock_uvc_set_format_mode (MOCK_UVC_FORMAT_ZERO_FRAMERATE);

  GstElement *pipeline = build_pipeline ();
  /* The old code divided 1e9 by a 0 fps framerate here and took SIGFPE in the
   * streaming task. Reaching the assertion at all proves the guard held. */
  GstMessage *msg = play_and_wait_error (pipeline);

  if (msg != NULL)
    gst_message_unref (msg);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (msg != NULL,
      "expected a graceful negotiate failure (no SIGFPE) for a 0 fps device");
}

GST_END_TEST;

GST_START_TEST (test_negotiate_zero_interval)
{
  mock_uvc_set_format_mode (MOCK_UVC_FORMAT_ZERO_DEVICE_INTERVAL);

  GstElement *pipeline = build_pipeline ();
  /* The old code divided 1e7 by a zero device frame interval here; the guard
   * skips the degenerate descriptor instead of taking SIGFPE. */
  GstMessage *msg = play_and_wait_error (pipeline);

  if (msg != NULL)
    gst_message_unref (msg);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_unless (msg != NULL,
      "expected a graceful negotiate failure (no SIGFPE) for a zero-interval device");
}

GST_END_TEST;

static Suite *
negotiate_suite (void)
{
  Suite *s = suite_create ("libuvch264src-negotiate");
  TCase *tc = tcase_create ("negotiate");

  tcase_set_timeout (tc, 90);
  tcase_add_checked_fixture (tc, setup, NULL);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_negotiate_leak);
  tcase_add_test (tc, test_negotiate_zero_format);
  tcase_add_test (tc, test_negotiate_framerate_zero);
  tcase_add_test (tc, test_negotiate_zero_interval);

  return s;
}

GST_CHECK_MAIN (negotiate);
