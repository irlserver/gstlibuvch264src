#include "gstlibuvch264src_error.h"

#include <gst/gst.h>
#include <libuvc/libuvc.h>

void
gst_libuvc_h264_src_post_error (GstElement * element, uvc_error_t err,
    const char * context)
{
  const char * ctx = context ? context : "UVC operation";
  /* libuvc's own human-readable string goes in the debug field for diagnosis. */
  const char * detail = uvc_strerror (err);

  switch (err) {
    case UVC_ERROR_NO_DEVICE:
      GST_ELEMENT_ERROR (element, RESOURCE, NOT_FOUND,
          ("%s: UVC device not found or disconnected", ctx),
          ("uvc_error_t=%d (%s)", err, detail));
      break;
    case UVC_ERROR_BUSY:
      GST_ELEMENT_ERROR (element, RESOURCE, BUSY,
          ("%s: UVC device is busy (in use by another process)", ctx),
          ("uvc_error_t=%d (%s)", err, detail));
      break;
    case UVC_ERROR_NOT_SUPPORTED:
      GST_ELEMENT_ERROR (element, RESOURCE, SETTINGS,
          ("%s: requested mode/operation not supported by the UVC device", ctx),
          ("uvc_error_t=%d (%s)", err, detail));
      break;
    case UVC_ERROR_ACCESS:
      GST_ELEMENT_ERROR (element, RESOURCE, OPEN_READ_WRITE,
          ("%s: insufficient permissions to access the UVC device", ctx),
          ("uvc_error_t=%d (%s)", err, detail));
      break;
    default:
      GST_ELEMENT_ERROR (element, RESOURCE, FAILED,
          ("%s: UVC operation failed", ctx),
          ("uvc_error_t=%d (%s)", err, detail));
      break;
  }
}

void
gst_libuvc_h264_src_post_disconnect_error (GstElement * element)
{
  GST_ELEMENT_ERROR (element, RESOURCE, READ,
      ("UVC device disconnected during streaming"),
      ("frame delivery stopped; the device was removed (NO_DEVICE)"));
}
