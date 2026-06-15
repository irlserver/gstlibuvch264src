#ifndef GST_LIBUVC_H264_SRC_H
#define GST_LIBUVC_H264_SRC_H

#include <glib.h>
#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <libuvc/libuvc.h>

G_BEGIN_DECLS

#define GST_TYPE_LIBUVC_H264_SRC (gst_libuvc_h264_src_get_type())
G_DECLARE_FINAL_TYPE(GstLibuvcH264Src, gst_libuvc_h264_src, GST, LIBUVC_H264_SRC, GstPushSrc)

#define DEFAULT_DEVICE_INDEX "0"
#define TIMEOUT_DURATION G_TIME_SPAN_SECOND // 1 second
#define DJI_VENDOR_ID 0x2ca3
#define DJI_PRODUCT_ID 0x0023

#define SPSPPSBUFSZ 1024

#define MIN_FRAMES_CALC_INTERVAL 60

/* The instance struct (struct _GstLibuvcH264Src) is defined in the private
 * gstlibuvch264src_internal.h, shared by the element's translation units. */

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_H */
