#ifndef GST_LIBUVC_H264_SRC_INTERNAL_H
#define GST_LIBUVC_H264_SRC_INTERNAL_H

#include <glib.h>
#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <libuvc/libuvc.h>
#include "gstlibuvch264src.h"

G_BEGIN_DECLS

/* Debug category is defined (non-static) in gstlibuvch264src.c and referenced
 * by every translation unit of this element. Declared extern here so the split
 * modules share the single category created by GST_DEBUG_CATEGORY_INIT. */
GST_DEBUG_CATEGORY_EXTERN(gst_libuvc_h264_src_debug);
#define GST_CAT_DEFAULT gst_libuvc_h264_src_debug

struct _GstLibuvcH264Src {
  GstPushSrc parent_instance;
  gchar* index;
  uvc_context_t *uvc_ctx;
  uvc_device_t *uvc_dev;
  uvc_device_handle_t *uvc_devh;
  uvc_stream_ctrl_t uvc_ctrl;
  enum uvc_frame_format frame_format;
  gint negotiated_width;
  gint negotiated_height;
  /* The framerate negotiate() resolved, kept so the opt-in reconnect path can
   * re-run uvc_get_stream_ctrl_format_size() with the original geometry. Unlike
   * frame_interval (mutated by the PTS estimator), this stays the negotiated value. */
  gint negotiated_framerate;
  GAsyncQueue *frame_queue;
  gboolean streaming;
  gint flushing; /* atomic: set by unlock(), checked by create() to bail out */
  /* Sustained-silence disconnect detection + opt-in reconnect (Task 18). libuvc
   * delivers no NULL frame on unplug in callback mode (it goes silent), so
   * create() infers a disconnect after DISCONNECT_TIMEOUT_COUNT consecutive 1s
   * timeouts. Reset in start() and whenever a real frame arrives. */
  gint consecutive_timeouts;
  gboolean reconnect_enabled; /* PROP_RECONNECT: opt-in in-element auto-reconnect */
  GstClock *clock;
  int64_t pts_offset_sum;
  int64_t pts_stretch;
  GstClockTime base_time;
  GstClockTime prev_pts;
  gint64 frame_interval; // in ns
  guint64 prev_int_ts;
  gint frame_count;
  /* Monotonic per-session output counter for GST_BUFFER_OFFSET. Distinct from
   * frame_count, which is periodically reset by the PTS estimator and so is not
   * monotonic. Reset in start(); only touched on the feeder thread. */
  guint64 frame_offset;
  gboolean had_idr;
  gboolean send_sps_pps;
  gint vps_length;
  gint sps_length;
  gint pps_length;
  unsigned char vps[SPSPPSBUFSZ];
  unsigned char sps[SPSPPSBUFSZ];
  unsigned char pps[SPSPPSBUFSZ];
  
  // Control socket additions. Opt-in (M9): enabled gates it on; path is the bound
  // path (explicit property or per-instance $XDG_RUNTIME_DIR default), heap-owned
  // here and freed in finalize().
  gboolean control_socket_enabled;
  gchar* control_socket_path;
  gint control_socket;
  gpointer control_thread;
  gboolean control_running;
  GMutex control_mutex;

  // PTZ state, filled by ptz_probe_capabilities() at open and guarded by
  // control_mutex. pan and tilt share one UVC control, so a single-axis set must
  // re-send the other axis from *_cur (the last value written).
  gint pan_min, pan_max, pan_cur;
  gint tilt_min, tilt_max, tilt_cur;
  gint zoom_min, zoom_max, zoom_cur;
  gboolean pan_supported, tilt_supported, zoom_supported;
  gboolean ptz_supported;
};

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_INTERNAL_H */
