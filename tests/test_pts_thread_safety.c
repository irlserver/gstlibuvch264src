/* Concurrency tests for the clock + PTS critical section in frame_callback().
 *
 * Like test_device_select.c, this statically links the element translation
 * units, the libuvc mock, and the driver into ONE executable and registers the
 * element type directly - so the racing set_clock() vmethod and the mock feeder
 * thread that drives frame_callback() run in the SAME instrumented binary, which
 * is what ThreadSanitizer needs to observe both sides of the race. (It also
 * avoids the shared mock-backed plugin .so used by the other harnesses.)
 *
 *   test_pts_clock_race   Hammer gst_element_set_clock() from the main thread
 *                         (toggling a real clock and NULL) while the mock feeder
 *                         delivers frames on the libuvc callback thread. The
 *                         clock pointer and the PTS baseline (base_time,
 *                         prev_pts, pts_offset_sum, pts_stretch) are shared
 *                         between the two threads; without the in-callback
 *                         object lock this is a data race (and a clock
 *                         use-after-free on the NULL transition). Surfaced as
 *                         the ctest entry "pts_thread_safety"; the TSan variant
 *                         is the gate.
 *
 *   test_frame_throughput The same locking must not cost frames: stream a fixed
 *                         number of buffers end to end and assert every one is
 *                         delivered (no drops versus the pre-lock baseline).
 *                         Surfaced as the ctest entry "frame_throughput".
 *
 * GST_CHECKS selects a single test per ctest invocation (see tests/CMakeLists.txt).
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

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

/* Atomic so ThreadSanitizer sees the cross-thread handshake without a GMutex
 * (whose happens-before lives inside uninstrumented GLib and is invisible to
 * TSan under ignore_noninstrumented_modules). */
static gint buffer_seen;
static gint buffer_count;

static GstPadProbeReturn
first_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  (void) pad;
  (void) user_data;
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER)
    g_atomic_int_set (&buffer_seen, 1);
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

GST_START_TEST (test_pts_clock_race)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();            /* VALID mode, frames until stop */
  g_atomic_int_set (&buffer_seen, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src, first_buffer_probe);

  fail_unless (gst_element_set_state (pipeline, GST_STATE_PLAYING)
      != GST_STATE_CHANGE_FAILURE, "could not set pipeline to PLAYING");

  /* Wait until frames are actually flowing, so frame_callback() is touching the
   * clock + PTS state and the hammer below truly races it (not an idle source). */
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (!g_atomic_int_get (&buffer_seen)
      && g_get_monotonic_time () < deadline) {
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  fail_unless (g_atomic_int_get (&buffer_seen),
      "no frames flowed; cannot exercise the clock/PTS race");

  /* Drive the set_clock() vmethod hard from this thread while frame_callback()
   * runs on the mock feeder thread. Each set_clock(clock) resets the PTS
   * baseline and each set_clock(NULL) drops the clock - both write the exact
   * fields frame_callback() reads. Without the in-callback object lock TSan
   * reports the race here; with it the run is clean. */
  GstClock *clock = gst_system_clock_obtain ();
  gint64 race_until = g_get_monotonic_time () + 1500 * G_TIME_SPAN_MILLISECOND;
  while (g_get_monotonic_time () < race_until) {
    gst_element_set_clock (GST_ELEMENT (src), clock);
    gst_element_set_clock (GST_ELEMENT (src), NULL);
  }
  gst_object_unref (clock);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
}

GST_END_TEST;

#define THROUGHPUT_BUFFERS 120

GST_START_TEST (test_frame_throughput)
{
  load_core_elements ();
  register_element ();
  mock_uvc_reset ();
  g_atomic_int_set (&buffer_count, 0);

  GstElement *src = NULL;
  GstElement *pipeline = build_pipeline (&src, count_buffers_probe);
  /* num-buffers bounds the run: the base class emits EOS after N buffers. */
  g_object_set (src, "num-buffers", (gint) THROUGHPUT_BUFFERS, NULL);

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

  /* The narrowed lock must not drop frames: every requested buffer arrives. */
  fail_unless (g_atomic_int_get (&buffer_count) == THROUGHPUT_BUFFERS,
      "expected %d buffers, got %d (frames dropped under the PTS lock)",
      THROUGHPUT_BUFFERS, g_atomic_int_get (&buffer_count));
}

GST_END_TEST;

static Suite *
pts_thread_safety_suite (void)
{
  Suite *s = suite_create ("libuvch264src-pts-thread-safety");

  TCase *tc_race = tcase_create ("pts_clock_race");
  tcase_set_timeout (tc_race, 60);
  tcase_add_test (tc_race, test_pts_clock_race);
  suite_add_tcase (s, tc_race);

  TCase *tc_thru = tcase_create ("frame_throughput");
  tcase_set_timeout (tc_thru, 60);
  tcase_add_test (tc_thru, test_frame_throughput);
  suite_add_tcase (s, tc_thru);

  return s;
}

GST_CHECK_MAIN (pts_thread_safety);
