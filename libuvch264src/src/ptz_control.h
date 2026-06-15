#ifndef GST_LIBUVC_H264_SRC_PTZ_CONTROL_H
#define GST_LIBUVC_H264_SRC_PTZ_CONTROL_H

#include <glib.h>
#include "gstlibuvch264src.h"

G_BEGIN_DECLS

/* Create, bind (per-instance path under $XDG_RUNTIME_DIR, mode 0600) and listen
 * on the opt-in control socket. Run in start() before the control thread spawns,
 * so the listening fd exists before any accept(). Returns FALSE (and binds
 * nothing) on failure; the media path runs on regardless. */
gboolean gst_libuvc_h264_src_control_socket_bind(GstLibuvcH264Src *self);

/* Close the listening fd and unlink the bound path. Safe to call when the socket
 * was never bound. The path string itself is freed in finalize(). */
void gst_libuvc_h264_src_control_socket_unbind(GstLibuvcH264Src *self);

gpointer gst_libuvc_h264_src_control_thread(gpointer data);

/* Probe pan/tilt/zoom ranges via the UVC GET_MIN/MAX/RES requests and record
 * which axes the device actually supports. Safe to call with no open handle
 * (clears every flag). Run once after the device is opened. */
void gst_libuvc_h264_src_ptz_probe_capabilities(GstLibuvcH264Src *self);

/* Drive a single PTZ axis to value, clamped to the probed device range. Each
 * returns TRUE on a successful control transfer, FALSE when the axis is
 * unsupported, no device is open, or the transfer failed (a bus error is
 * posted in that last case). */
gboolean gst_libuvc_h264_src_ptz_set_pan(GstLibuvcH264Src *self, gint value);
gboolean gst_libuvc_h264_src_ptz_set_tilt(GstLibuvcH264Src *self, gint value);
gboolean gst_libuvc_h264_src_ptz_set_zoom(GstLibuvcH264Src *self, gint value);

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_PTZ_CONTROL_H */
