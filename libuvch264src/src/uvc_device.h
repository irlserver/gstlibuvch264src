#ifndef GST_LIBUVC_H264_SRC_UVC_DEVICE_H
#define GST_LIBUVC_H264_SRC_UVC_DEVICE_H

#include "gstlibuvch264src.h"

G_BEGIN_DECLS

void gst_libuvc_h264_src_force_usb_release(GstLibuvcH264Src *self);
void gst_libuvc_h264_src_v4l2_probe(GstElement *element, int device_index);

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_UVC_DEVICE_H */
