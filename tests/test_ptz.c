/* Native PTZ property/signal tests for the libuvch264src element.
 *
 * Same harness shape as test_device_select.c: the element translation units, the
 * libuvc mock, and the driver are linked into ONE executable with the element
 * type registered statically, so the mock's last-written PTZ values live in this
 * process and are directly observable (mock_uvc_get_last_pantilt/zoom). Each
 * gst-check test is surfaced as its own ctest entry via GST_CHECKS.
 *
 * Covered (Task 12):
 *   test_ptz_properties      a PTZ-capable device: the pan/tilt/zoom properties
 *                            and the set-ptz action signal drive the device,
 *                            clamp to the probed range, and read back.
 *   test_ptz_capability_gate a device that reports no PTZ unit: properties and
 *                            the signal are no-ops, nothing reaches the device.
 *
 * Results are captured while the pipeline is live and only asserted after
 * teardown: with CK_FORK=no a failing fail_unless longjmps out and would
 * otherwise leave the control thread keeping the process alive until the ctest
 * timeout (see test_device_select.c).
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src.h"
#include "mock_libuvc.h"

static void
setup (void)
{
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
}

static GstElement *
build_pipeline (void)
{
  GstElement *pipeline = gst_pipeline_new ("test-pipeline");
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

GST_START_TEST (test_ptz_properties)
{
  mock_uvc_set_device_count (1);
  mock_uvc_set_ptz_range (-180000, 180000, -90000, 90000, 0, 100);

  GstElement *pipeline = build_pipeline ();
  GstElement *src = gst_bin_get_by_name (GST_BIN (pipeline), "src");
  fail_unless (src != NULL, "src not found in pipeline");

  /* READY->PAUSED runs start() synchronously on a live source, so the PTZ
   * capability probe has completed by the time set_state returns. */
  GstStateChangeReturn sret =
      gst_element_set_state (pipeline, GST_STATE_PAUSED);

  g_object_set (src, "pan", 1000, "tilt", 2000, "zoom", 50, NULL);
  int32_t set_pan = -1, set_tilt = -1;
  mock_uvc_get_last_pantilt (&set_pan, &set_tilt);
  uint16_t set_zoom = mock_uvc_get_last_zoom ();

  gint rpan = -1, rtilt = -1, rzoom = -1;
  g_object_get (src, "pan", &rpan, "tilt", &rtilt, "zoom", &rzoom, NULL);

  /* A request within the property's static range but past the probed device
   * range (max 180000 here) must clamp to the device maximum. */
  g_object_set (src, "pan", 500000, NULL);
  int32_t clamp_pan = -1, clamp_tilt = -1;
  mock_uvc_get_last_pantilt (&clamp_pan, &clamp_tilt);

  /* The set-ptz action signal drives every axis in one emission. */
  gboolean sig_ret = FALSE;
  g_signal_emit_by_name (src, "set-ptz", -500, -600, 25, &sig_ret);
  int32_t sig_pan = -1, sig_tilt = -1;
  mock_uvc_get_last_pantilt (&sig_pan, &sig_tilt);
  uint16_t sig_zoom = mock_uvc_get_last_zoom ();

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (src);
  gst_object_unref (pipeline);

  fail_unless (sret != GST_STATE_CHANGE_FAILURE, "start() should have succeeded");
  fail_unless (set_pan == 1000 && set_tilt == 2000,
      "pan/tilt property did not reach the device: %d/%d", set_pan, set_tilt);
  fail_unless (set_zoom == 50,
      "zoom property did not reach the device: %u", set_zoom);
  fail_unless (rpan == 1000 && rtilt == 2000 && rzoom == 50,
      "property read-back mismatch: pan=%d tilt=%d zoom=%d", rpan, rtilt, rzoom);
  fail_unless (clamp_pan == 180000,
      "pan should clamp to the device maximum: %d", clamp_pan);
  fail_unless (sig_ret, "set-ptz should report success on a PTZ-capable device");
  fail_unless (sig_pan == -500 && sig_tilt == -600 && sig_zoom == 25,
      "set-ptz did not drive the device: pan=%d tilt=%d zoom=%u",
      sig_pan, sig_tilt, sig_zoom);
}

GST_END_TEST;

GST_START_TEST (test_ptz_capability_gate)
{
  mock_uvc_set_device_count (1);
  /* Camera reports no PanTilt/Zoom unit: every uvc_*_abs() returns NOT_SUPPORTED
   * so the probe must gate all three axes off. */
  mock_uvc_set_ptz_supported (false);

  GstElement *pipeline = build_pipeline ();
  GstElement *src = gst_bin_get_by_name (GST_BIN (pipeline), "src");
  fail_unless (src != NULL, "src not found in pipeline");

  GstStateChangeReturn sret =
      gst_element_set_state (pipeline, GST_STATE_PAUSED);

  /* The mock's last-written values start at 0; an unsupported device must stay
   * undriven however the properties and the signal are poked. */
  g_object_set (src, "pan", 1234, "tilt", 5678, "zoom", 42, NULL);
  gboolean sig_ret = TRUE;
  g_signal_emit_by_name (src, "set-ptz", 4321, 8765, 99, &sig_ret);

  int32_t pan = -1, tilt = -1;
  mock_uvc_get_last_pantilt (&pan, &tilt);
  uint16_t zoom = mock_uvc_get_last_zoom ();
  gint rpan = -1, rtilt = -1, rzoom = -1;
  g_object_get (src, "pan", &rpan, "tilt", &rtilt, "zoom", &rzoom, NULL);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (src);
  gst_object_unref (pipeline);

  fail_unless (sret != GST_STATE_CHANGE_FAILURE,
      "start() should still succeed on a device without PTZ");
  fail_unless (pan == 0 && tilt == 0,
      "unsupported camera must not be driven: pan/tilt=%d/%d", pan, tilt);
  fail_unless (zoom == 0,
      "unsupported camera zoom must not be driven: zoom=%u", zoom);
  fail_unless (!sig_ret, "set-ptz must report failure when no axis is supported");
  fail_unless (rpan == 0 && rtilt == 0 && rzoom == 0,
      "read-back should be 0 on an unsupported device: %d/%d/%d",
      rpan, rtilt, rzoom);
}

GST_END_TEST;

static Suite *
ptz_suite (void)
{
  Suite *s = suite_create ("libuvch264src-ptz");
  TCase *tc = tcase_create ("ptz");

  tcase_set_timeout (tc, 60);
  tcase_add_checked_fixture (tc, setup, NULL);
  suite_add_tcase (s, tc);

  tcase_add_test (tc, test_ptz_properties);
  tcase_add_test (tc, test_ptz_capability_gate);

  return s;
}

GST_CHECK_MAIN (ptz);
