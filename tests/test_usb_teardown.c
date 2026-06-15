/* USB teardown (H1) + interface-count (L8) regression for the libuvch264src
 * element. The element TUs, the libuvc mock, and a dedicated libusb mock (no
 * real libusb) are linked into one executable with MOCK_LIBUSB_TEARDOWN defined,
 * so force_usb_release() actually drives a mock libusb handle and uvc_close()
 * models libuvc closing that same handle.
 *
 * The old force_usb_release() closed the handle and stop() then called
 * uvc_close() on it again - a double-close. Driven 10 times, the ASAN variant
 * aborts on that double-free; both variants also fail the balance assertions
 * below. The fixed teardown leaves closing to uvc_close(), so each open is
 * matched by exactly one close, and the interface count is read once per
 * teardown from the active config descriptor (L8) instead of a fixed 0..7 range.
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"
#include "mock_libusb.h"

#define TEARDOWN_CYCLES 10

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
  mock_libusb_reset ();
  g_atomic_int_set (&g_buffers_seen, 0);
}

GST_START_TEST (test_usb_teardown_cycles)
{
  GstElement *pipeline = gst_pipeline_new ("teardown-pipeline");
  GstElement *src = gst_element_factory_make ("libuvch264src", "src");
  GstElement *sink = gst_element_factory_make ("fakesink", "sink");
  fail_unless (pipeline && src && sink, "failed to create test elements");
  g_object_set (sink, "sync", FALSE, NULL);
  g_object_set (src, "index", "0", NULL);
  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
  fail_unless (gst_element_link (src, sink), "failed to link src ! sink");

  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL, "fakesink has no sink pad");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer_probe, NULL,
      NULL);
  gst_object_unref (pad);

  gboolean cycle_failed = FALSE;

  for (int i = 0; i < TEARDOWN_CYCLES; i++) {
    if (gst_element_set_state (pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
      cycle_failed = TRUE;
      break;
    }

    /* Wait until streaming has delivered a frame this cycle, so stop() tears
     * down a live handle (uvc_stop_streaming + force_usb_release + uvc_close).
     * Bounded: a missing frame still exercises the open/close teardown. */
    int before = g_atomic_int_get (&g_buffers_seen);
    gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
    while (g_atomic_int_get (&g_buffers_seen) <= before
        && g_get_monotonic_time () < deadline) {
      g_usleep (2 * G_TIME_SPAN_MILLISECOND);
    }

    if (gst_element_set_state (pipeline, GST_STATE_NULL) ==
        GST_STATE_CHANGE_FAILURE) {
      cycle_failed = TRUE;
      break;
    }
  }

  /* Snapshot before teardown: a failed fail_unless longjmps past the unref, and
   * a live control thread would keep the non-forking process alive until the
   * ctest timeout. Assert last. */
  int uvc_opens = mock_uvc_open_count ();
  int uvc_closes = mock_uvc_close_count ();
  int usb_opens = mock_libusb_open_count ();
  int usb_closes = mock_libusb_close_count ();
  int cfg_queries = mock_libusb_config_query_count ();

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  fail_if (cycle_failed, "a start/stop cycle failed the state change");

  fail_unless (uvc_opens == TEARDOWN_CYCLES,
      "expected %d uvc_open() across the run, got %d", TEARDOWN_CYCLES,
      uvc_opens);
  fail_unless (uvc_closes == uvc_opens,
      "uvc teardown unbalanced: %d opens, %d closes", uvc_opens, uvc_closes);

  /* H1: each open's libusb handle is closed exactly once. The old code closed it
   * in force_usb_release() too, which would double this (and ASAN would already
   * have aborted on the double-free before we ever got here). */
  fail_unless (usb_opens == TEARDOWN_CYCLES,
      "expected %d libusb handles opened, got %d", TEARDOWN_CYCLES, usb_opens);
  fail_unless (usb_closes == usb_opens,
      "libusb close/open unbalanced: %d opens, %d closes (double-close is H1)",
      usb_opens, usb_closes);

  /* L8: the release loop sized itself from the active config descriptor (queried
   * once per teardown), not a hardcoded 0..7 range. */
  fail_unless (cfg_queries == TEARDOWN_CYCLES,
      "expected %d active-config queries (L8), got %d", TEARDOWN_CYCLES,
      cfg_queries);
}

GST_END_TEST;

static Suite *
usb_teardown_suite (void)
{
  Suite *s = suite_create ("libuvch264src-usb-teardown");
  TCase *tc = tcase_create ("usb-teardown");

  tcase_set_timeout (tc, 90);
  tcase_add_checked_fixture (tc, setup, NULL);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_usb_teardown_cycles);

  return s;
}

GST_CHECK_MAIN (usb_teardown);
