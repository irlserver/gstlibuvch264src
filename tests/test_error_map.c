/* Unit test for the uvc_error_t -> GST_ELEMENT_ERROR mapping helper.
 *
 * This is a pure translation-unit test: it compiles gstlibuvch264src_error.c
 * directly (no plugin, no UVC device) and verifies that each libuvc error code
 * is posted on the element's bus as a GST_MESSAGE_ERROR carrying the expected
 * GError domain (GST_RESOURCE_ERROR) and code, with a non-empty message.
 *
 * A bare GstPipeline is used purely as a message-posting GstElement: it owns a
 * bus, so GST_ELEMENT_ERROR (which routes through gst_element_post_message)
 * lands a poppable ERROR message we can parse and assert on.
 */

#include <gst/check/gstcheck.h>

#include "gstlibuvch264src_error.h"

/* Post via the helper, pop the resulting ERROR message, and assert its GError
 * domain/code. @disconnect selects the disconnect-specific helper. */
static void
expect_resource_error (uvc_error_t err, gint expected_code, gboolean disconnect)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  fail_unless (pipeline != NULL, "could not create a pipeline");
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  fail_unless (bus != NULL, "pipeline has no bus");

  if (disconnect)
    gst_libuvc_h264_src_post_disconnect_error (GST_ELEMENT (pipeline));
  else
    gst_libuvc_h264_src_post_error (GST_ELEMENT (pipeline), err, "unit test");

  GstMessage *msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
  fail_unless (msg != NULL,
      "expected a GST_MESSAGE_ERROR on the bus (uvc_error_t=%d, disconnect=%d)",
      err, disconnect);

  GError *gerr = NULL;
  gchar *dbg = NULL;
  gst_message_parse_error (msg, &gerr, &dbg);

  fail_unless (gerr != NULL, "ERROR message carried no GError");
  fail_unless (gerr->domain == GST_RESOURCE_ERROR,
      "domain mismatch (uvc_error_t=%d): got '%s', want 'gst-resource-error-quark'",
      err, g_quark_to_string (gerr->domain));
  fail_unless (gerr->code == expected_code,
      "code mismatch (uvc_error_t=%d): got %d, want %d",
      err, gerr->code, expected_code);
  fail_unless (gerr->message != NULL && gerr->message[0] != '\0',
      "ERROR message text is empty (uvc_error_t=%d)", err);

  g_clear_error (&gerr);
  g_free (dbg);
  gst_message_unref (msg);
  gst_object_unref (bus);
  gst_object_unref (pipeline);
}

GST_START_TEST (test_no_device_maps_to_not_found)
{
  expect_resource_error (UVC_ERROR_NO_DEVICE, GST_RESOURCE_ERROR_NOT_FOUND,
      FALSE);
}

GST_END_TEST;

GST_START_TEST (test_busy_maps_to_busy)
{
  expect_resource_error (UVC_ERROR_BUSY, GST_RESOURCE_ERROR_BUSY, FALSE);
}

GST_END_TEST;

GST_START_TEST (test_not_supported_maps_to_settings)
{
  expect_resource_error (UVC_ERROR_NOT_SUPPORTED, GST_RESOURCE_ERROR_SETTINGS,
      FALSE);
}

GST_END_TEST;

GST_START_TEST (test_access_maps_to_open_read_write)
{
  expect_resource_error (UVC_ERROR_ACCESS, GST_RESOURCE_ERROR_OPEN_READ_WRITE,
      FALSE);
}

GST_END_TEST;

GST_START_TEST (test_unmapped_codes_fall_back_to_failed)
{
  /* Anything outside the explicit set must land on RESOURCE / FAILED. */
  expect_resource_error (UVC_ERROR_IO, GST_RESOURCE_ERROR_FAILED, FALSE);
  expect_resource_error (UVC_ERROR_INVALID_PARAM, GST_RESOURCE_ERROR_FAILED,
      FALSE);
  expect_resource_error (UVC_ERROR_OVERFLOW, GST_RESOURCE_ERROR_FAILED, FALSE);
  expect_resource_error (UVC_ERROR_PIPE, GST_RESOURCE_ERROR_FAILED, FALSE);
  expect_resource_error (UVC_ERROR_INTERRUPTED, GST_RESOURCE_ERROR_FAILED,
      FALSE);
  expect_resource_error (UVC_ERROR_NO_MEM, GST_RESOURCE_ERROR_FAILED, FALSE);
  expect_resource_error (UVC_ERROR_NOT_FOUND, GST_RESOURCE_ERROR_FAILED, FALSE);
  expect_resource_error (UVC_ERROR_OTHER, GST_RESOURCE_ERROR_FAILED, FALSE);
}

GST_END_TEST;

GST_START_TEST (test_disconnect_maps_to_read)
{
  /* err argument is irrelevant for the disconnect helper. */
  expect_resource_error (UVC_ERROR_NO_DEVICE, GST_RESOURCE_ERROR_READ, TRUE);
}

GST_END_TEST;

static Suite *
error_map_suite (void)
{
  Suite *s = suite_create ("libuvch264src-error-map");
  TCase *tc = tcase_create ("mapping");

  suite_add_tcase (s, tc);
  tcase_add_test (tc, test_no_device_maps_to_not_found);
  tcase_add_test (tc, test_busy_maps_to_busy);
  tcase_add_test (tc, test_not_supported_maps_to_settings);
  tcase_add_test (tc, test_access_maps_to_open_read_write);
  tcase_add_test (tc, test_unmapped_codes_fall_back_to_failed);
  tcase_add_test (tc, test_disconnect_maps_to_read);

  return s;
}

GST_CHECK_MAIN (error_map);
