#ifndef GST_LIBUVC_H264_SRC_ERROR_H
#define GST_LIBUVC_H264_SRC_ERROR_H

#include <gst/gst.h>
#include <libuvc/libuvc.h>

G_BEGIN_DECLS

/* Translate a libuvc error into the matching GST_ELEMENT_ERROR and post it on
 * the element's bus. Without this, fatal UVC failures only reach the debug log
 * via GST_ERROR_OBJECT and never surface to downstream consumers, which can
 * only react to device failures via bus ERROR messages.
 *
 * @context is a short human-readable phrase describing the operation that
 * failed (e.g. "opening device", "starting stream"); it is woven into the
 * error message. NULL is tolerated. */
void gst_libuvc_h264_src_post_error (GstElement * element, uvc_error_t err,
    const char * context);

/* Post the canonical "device disconnected mid-stream" error. The disconnect
 * path is distinct from an open/setup failure: frame delivery has stopped on a
 * device that was previously streaming, which maps to RESOURCE / READ. */
void gst_libuvc_h264_src_post_disconnect_error (GstElement * element);

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_ERROR_H */
