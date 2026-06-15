#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "gstlibuvch264src.h"
#include "gstlibuvch264src_internal.h"
#include "gstlibuvch264src_error.h"
#include "uvc_device.h"
#include "frame_pipeline.h"
#include "spspps_cache.h"
#include "ptz_control.h"
#include <gst/gst.h>
#include <libuvc/libuvc.h>

GST_DEBUG_CATEGORY(gst_libuvc_h264_src_debug);

enum {
  PROP_0,
  PROP_INDEX,
  PROP_PAN,
  PROP_TILT,
  PROP_ZOOM,
  PROP_CONTROL_SOCKET,
  PROP_CONTROL_SOCKET_PATH,
  PROP_RECONNECT,
  PROP_LAST
};

/* Sustained-silence disconnect detection. libuvc delivers no NULL frame on
 * unplug in callback mode (Task 4 spike), so create() infers a disconnect after
 * this many consecutive TIMEOUT_DURATION (1 s) pop timeouts with no frame. */
#define DISCONNECT_TIMEOUT_COUNT 5

/* Opt-in in-element reconnect: bounded exponential backoff 1,2,4,8,16 s. */
#define RECONNECT_MAX_RETRIES 5
#define RECONNECT_BACKOFF_INITIAL_S 1

#define H264_CAPS "video/x-h264," \
                  "stream-format=(string)byte-stream," \
                  "alignment=(string)au"
#define H265_CAPS "video/x-h265," \
                  "stream-format=(string)byte-stream," \
                  "alignment=(string)au"

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
  "src",
  GST_PAD_SRC,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS(H264_CAPS "; " H265_CAPS)
);

G_DEFINE_TYPE_WITH_CODE(GstLibuvcH264Src, gst_libuvc_h264_src, GST_TYPE_PUSH_SRC,
  GST_DEBUG_CATEGORY_INIT(gst_libuvc_h264_src_debug, "libuvch264src", 0, "libuvch264src element"));

static gboolean gst_libuvc_h264_negotiate(GstBaseSrc * basesrc);
static gboolean gst_libuvc_h264_src_query(GstBaseSrc *basesrc, GstQuery *query);
static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec);
static void gst_libuvc_h264_src_get_property(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec);
static gboolean gst_libuvc_h264_set_clock(GstElement *element, GstClock *clock);
static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_unlock(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_unlock_stop(GstBaseSrc *src);
static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf);
static void gst_libuvc_h264_src_finalize(GObject *object);
static gboolean gst_libuvc_h264_src_set_ptz(GstLibuvcH264Src *self,
                                            gint pan, gint tilt, gint zoom);

/* GAsyncQueue forbids NULL payloads, so create() can never receive a NULL
 * "no more frames" marker. unlock() instead pushes this dedicated address to
 * wake a blocked create(); its value is irrelevant, only its identity matters. */
static const gchar flush_sentinel = 0;
#define FLUSH_SENTINEL ((gpointer) &flush_sentinel)

static void gst_libuvc_h264_src_class_init(GstLibuvcH264SrcClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);

  base_src_class->negotiate = GST_DEBUG_FUNCPTR(gst_libuvc_h264_negotiate);
  base_src_class->query = GST_DEBUG_FUNCPTR(gst_libuvc_h264_src_query);
  gobject_class->set_property = gst_libuvc_h264_src_set_property;
  gobject_class->get_property = gst_libuvc_h264_src_get_property;

  g_object_class_install_property(gobject_class, PROP_INDEX,
    g_param_spec_string("index", "Index",
                        "Device selector: ordinal \"0\", \"vid:pid\" (hex, e.g. "
                        "\"1234:5678\"), \"serial:<sn>\", or \"bus:<bus>:<addr>\"",
                        DEFAULT_DEVICE_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Native PTZ properties. Param-spec bounds cover the UVC arcsecond / focal
   * domain; the real per-device range is enforced at set time, and a set on an
   * axis the device does not report is silently ignored (capability-gated). */
  g_object_class_install_property(gobject_class, PROP_PAN,
    g_param_spec_int("pan", "Pan", "Absolute pan position in UVC arcseconds",
                     -648000, 648000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_TILT,
    g_param_spec_int("tilt", "Tilt", "Absolute tilt position in UVC arcseconds",
                     -648000, 648000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_ZOOM,
    g_param_spec_int("zoom", "Zoom", "Absolute zoom as a UVC focal length",
                     0, 65535, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Opt-in PTZ control socket (M9). Default OFF: the legacy world-accessible
   * /tmp/libuvc_control is gone, so nothing binds unless asked. */
  g_object_class_install_property(gobject_class, PROP_CONTROL_SOCKET,
    g_param_spec_boolean("control-socket", "Control socket",
                         "Enable the Unix-domain PTZ control socket",
                         FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_CONTROL_SOCKET_PATH,
    g_param_spec_string("control-socket-path", "Control socket path",
                        "Explicit control socket path; empty selects a "
                        "per-instance path under $XDG_RUNTIME_DIR",
                        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Opt-in in-element auto-reconnect (Task 18). Default OFF: a mid-stream
   * disconnect always posts a RESOURCE/READ error; only with this enabled does
   * the element first attempt a bounded-backoff teardown/reopen before erroring. */
  g_object_class_install_property(gobject_class, PROP_RECONNECT,
    g_param_spec_boolean("reconnect", "Reconnect",
                         "Attempt bounded in-element auto-reconnect when the "
                         "device disconnects mid-stream",
                         FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Action signal driving all three axes in one emission; each axis is applied
   * only when the device supports it (gated in ptz_control.c). */
  g_signal_new_class_handler("set-ptz", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
    G_CALLBACK(gst_libuvc_h264_src_set_ptz), NULL, NULL, NULL,
    G_TYPE_BOOLEAN, 3, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);

  gst_element_class_set_static_metadata(element_class,
    "UVC H.264 / H.265 Video Source", "Source/Video",
    "Captures H.264 or H.265 video from a UVC device", "Name");

  gst_element_class_add_pad_template(element_class,
    gst_static_pad_template_get(&src_template));

  element_class->set_clock = gst_libuvc_h264_set_clock;
  base_src_class->start = gst_libuvc_h264_src_start;
  base_src_class->stop = gst_libuvc_h264_src_stop;
  base_src_class->unlock = gst_libuvc_h264_src_unlock;
  base_src_class->unlock_stop = gst_libuvc_h264_src_unlock_stop;
  push_src_class->create = gst_libuvc_h264_src_create;
  gobject_class->finalize = gst_libuvc_h264_src_finalize;
}

static void gst_libuvc_h264_src_init(GstLibuvcH264Src *self) {
  self->index = g_strdup(DEFAULT_DEVICE_INDEX);
  self->uvc_ctx = NULL;
  self->uvc_dev = NULL;
  self->uvc_devh = NULL;
  self->clock = NULL;
  self->frame_queue = g_async_queue_new();
  self->streaming = FALSE;
  self->flushing = 0;
  self->consecutive_timeouts = 0;
  self->reconnect_enabled = FALSE;
  self->frame_offset = 0;
  self->base_time = G_MAXUINT64;
  self->prev_pts = G_MAXUINT64;
  
  // Control socket initialization
  self->control_socket_enabled = FALSE;
  self->control_socket_path = NULL;
  self->control_socket = -1;
  self->control_thread = NULL;
  self->control_running = FALSE;
  g_mutex_init(&self->control_mutex);

  gchar sps[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x34, 0xAC, 0x4D, 0x00, 0xF0, 0x04, 0x4F, 0xCB, 0x35, 0x01, 0x01, 0x01, 0x40, 0x00, 0x00, 0xFA, 0x00, 0x00, 0x3A, 0x98, 0x03, 0xC7, 0x0C, 0xA8 };
  self->sps_length = sizeof(sps);
  memcpy(self->sps, sps, self->sps_length);

  gchar pps[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };
  self->pps_length = sizeof(pps);
  memcpy(self->pps, pps, self->pps_length);

  gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
  gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
}

static gboolean gst_libuvc_h264_negotiate(GstBaseSrc * basesrc) {
    GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(basesrc);

    GstCaps *thiscaps = gst_pad_query_caps(GST_BASE_SRC_PAD(basesrc), NULL);
    GST_INFO_OBJECT(basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);

    GstCaps *peercaps = gst_pad_peer_query_caps(GST_BASE_SRC_PAD(basesrc), NULL);
    GST_INFO_OBJECT(basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);

    GstCaps *caps = NULL;
    if (peercaps) {
        caps = gst_caps_intersect(peercaps, thiscaps);
        gst_caps_unref(thiscaps);
        gst_caps_unref(peercaps);
    } else {
        caps = thiscaps;
    }

    GST_INFO_OBJECT(basesrc, "caps intersection: %" GST_PTR_FORMAT, caps);

    gint width = -1, height = -1, framerate = -1;
    GstCaps *best_caps = NULL;
    gboolean result = FALSE;
    gboolean found_codec_format = FALSE;

    // Enumerate supported H264 / H265 resolutions and framerates
    // And select the highest compatible resolution, at the highest supported framerate
    for (const uvc_format_desc_t *format_desc = uvc_get_format_descs(self->uvc_devh);
         format_desc; format_desc = format_desc->next)
    {
        gboolean is_h264 = (memcmp(format_desc->fourccFormat, "H264", 4) == 0);
        gboolean is_h265 = (memcmp(format_desc->fourccFormat, "H265", 4) == 0);

        if (!is_h264 && !is_h265) continue;
        found_codec_format = TRUE;

        GstCaps *tmp_caps = gst_caps_from_string(is_h264? H264_CAPS : H265_CAPS);
        GstStructure *tmp_structure = gst_caps_get_structure(tmp_caps, 0);

        for (const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
             frame_desc; frame_desc = frame_desc->next)
        {
            gint resolution = frame_desc->wWidth * frame_desc->wHeight;

            gst_structure_set(tmp_structure,
                              "width", G_TYPE_INT, frame_desc->wWidth,
                              "height", G_TYPE_INT, frame_desc->wHeight,
                              NULL);

            gint fps = -1;
            if (frame_desc->intervals) {
                GValue framerates = G_VALUE_INIT;
                g_value_init(&framerates, GST_TYPE_LIST);

                for (const uint32_t *interval = frame_desc->intervals; *interval; interval++) {
                    gint _fps = 1e7 / *interval;
                    if (_fps > fps) {
                        fps = _fps;
                    }

                    GValue fps = G_VALUE_INIT;
                    g_value_init(&fps, GST_TYPE_FRACTION);
                    gst_value_set_fraction(&fps, (gint)_fps, 1);
                    gst_value_list_append_value(&framerates, &fps);
                    g_value_unset(&fps);
                }

                // gst_structure_set_value() copies the list, so the local GValue
                // owns a GST_TYPE_LIST that must be released or it leaks per call.
                gst_structure_set_value(tmp_structure, "framerate", &framerates);
                g_value_unset(&framerates);
            } else {
                // A device that reports a zero frame interval would divide by
                // zero here (SIGFPE); skip such a degenerate descriptor instead.
                if (frame_desc->dwMinFrameInterval == 0 ||
                    frame_desc->dwMaxFrameInterval == 0) {
                    GST_WARNING_OBJECT(self,
                        "Skipping %ux%u: device reported a zero frame interval",
                        frame_desc->wWidth, frame_desc->wHeight);
                    continue;
                }
                gint fps_min = 1e7 / frame_desc->dwMaxFrameInterval;
                gint fps = 1e7 / frame_desc->dwMinFrameInterval;
                gst_structure_set(tmp_structure, "framerate", GST_TYPE_FRACTION_RANGE, fps_min, 1, fps, 1, NULL);
            }

            if (gst_caps_can_intersect(caps, tmp_caps)) {
                if (resolution > (width * height)
                    || (resolution == (width * height) && fps > framerate)) {
                    width = frame_desc->wWidth;
                    height = frame_desc->wHeight;
                    self->frame_format = is_h264 ? UVC_FRAME_FORMAT_H264 : UVC_FRAME_FORMAT_H265;

                    if (best_caps) {
                        gst_caps_unref(best_caps);
                    }
                    best_caps = gst_caps_intersect(caps, tmp_caps);
                    GstStructure *s = gst_caps_get_structure(best_caps, 0);
                    gst_structure_fixate_field_nearest_fraction(s, "framerate", fps, 1);

                    gint fr_num, fr_den;
                    gst_structure_get_fraction(s, "framerate", &fr_num, &fr_den);
                    framerate = fr_num / fr_den;
                }
            }

        } // for frame_desc

        gst_caps_unref(tmp_caps);
    } // for format_desc

    if (!found_codec_format) {
        // The device exposes no H264/H265 format descriptor at all, so there is
        // nothing to stream. Post a bus ERROR (not just a debug log) so
        // downstream consumers can react, instead of falling
        // through with uninitialized width/height/framerate.
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), UVC_ERROR_NOT_SUPPORTED,
            "negotiating caps: device exposes no H264/H265 format");
        goto out;
    }

    // framerate <= 0 (not just < 0): a device whose fastest interval rounds down
    // to 0 fps would otherwise divide by zero at the frame_interval computation.
    if (width < 0 || height < 0 || framerate <= 0 || !best_caps) {
        GST_ERROR_OBJECT(self, "Unable to negotiate common caps");
        goto out;
    }

    int res = uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
                                              self->frame_format, width, height, framerate);
    if (res < 0) {
        GST_ERROR_OBJECT(self, "Unable to get stream control: %s", uvc_strerror(res));
        goto out;
    }

    GST_OBJECT_LOCK(self);
    self->frame_interval = (1000L * 1000L * 1000L) / framerate;
    GST_OBJECT_UNLOCK(self);

    /* Persist the negotiated resolution so the SPS/PPS cache key (L5) reflects
     * the active format; load_spspps/store_spspps read these. The framerate is
     * also kept so the opt-in reconnect path can re-run
     * uvc_get_stream_ctrl_format_size() with the original geometry. */
    self->negotiated_width = width;
    self->negotiated_height = height;
    self->negotiated_framerate = framerate;

    gst_base_src_set_caps(basesrc, best_caps);

    GST_INFO_OBJECT(basesrc, "Negotiated caps: %" GST_PTR_FORMAT, best_caps);

    load_spspps(self);

    result = TRUE;

out:
    // Single cleanup path: the working caps and the chosen caps are owned locals.
    // gst_base_src_set_caps() takes its own reference, so best_caps must be freed
    // here on success too, and both must be freed on every error path.
    if (caps)
        gst_caps_unref(caps);
    if (best_caps)
        gst_caps_unref(best_caps);
    return result;
}

static gboolean gst_libuvc_h264_src_query(GstBaseSrc *basesrc, GstQuery *query) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(basesrc);

  if (GST_QUERY_TYPE(query) == GST_QUERY_LATENCY) {
    /* A live source delivers a frame only once it has been fully captured, so
       the minimum latency is one frame interval; report it explicitly rather
       than leaving downstream sinks with the GstBaseSrc default of zero. max ==
       min: the element does not buffer ahead. frame_interval is shared with the
       frame_callback PTS estimator, which mutates it under the object lock, so
       read it the same way; until negotiate() sets it, defer to the base class. */
    GstClockTime latency;
    GST_OBJECT_LOCK(self);
    latency = self->frame_interval > 0
              ? (GstClockTime) self->frame_interval : GST_CLOCK_TIME_NONE;
    GST_OBJECT_UNLOCK(self);

    if (GST_CLOCK_TIME_IS_VALID(latency)) {
      gst_query_set_latency(query, TRUE, latency, latency);
      return TRUE;
    }
  }

  return GST_BASE_SRC_CLASS(gst_libuvc_h264_src_parent_class)->query(basesrc, query);
}

static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

  switch (prop_id) {
    case PROP_INDEX:
      g_free(self->index);
      self->index = g_value_dup_string(value);
      break;
    case PROP_PAN:
      gst_libuvc_h264_src_ptz_set_pan(self, g_value_get_int(value));
      break;
    case PROP_TILT:
      gst_libuvc_h264_src_ptz_set_tilt(self, g_value_get_int(value));
      break;
    case PROP_ZOOM:
      gst_libuvc_h264_src_ptz_set_zoom(self, g_value_get_int(value));
      break;
    case PROP_CONTROL_SOCKET:
      self->control_socket_enabled = g_value_get_boolean(value);
      break;
    case PROP_CONTROL_SOCKET_PATH: {
      const gchar *path = g_value_get_string(value);
      g_free(self->control_socket_path);
      self->control_socket_path = (path && *path) ? g_strdup(path) : NULL;
      break;
    }
    case PROP_RECONNECT:
      self->reconnect_enabled = g_value_get_boolean(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_libuvc_h264_src_get_property(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

  switch (prop_id) {
    case PROP_INDEX:
      g_value_set_string(value, self->index);
      break;
    case PROP_PAN:
      g_value_set_int(value, self->pan_cur);
      break;
    case PROP_TILT:
      g_value_set_int(value, self->tilt_cur);
      break;
    case PROP_ZOOM:
      g_value_set_int(value, self->zoom_cur);
      break;
    case PROP_CONTROL_SOCKET:
      g_value_set_boolean(value, self->control_socket_enabled);
      break;
    case PROP_CONTROL_SOCKET_PATH:
      g_value_set_string(value, self->control_socket_path);
      break;
    case PROP_RECONNECT:
      g_value_set_boolean(value, self->reconnect_enabled);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* "set-ptz" action handler: apply pan, tilt and zoom in one call. Each axis is
 * driven only when the device reports it; returns TRUE only if at least one
 * supported axis was driven and every attempted set succeeded. */
static gboolean gst_libuvc_h264_src_set_ptz(GstLibuvcH264Src *self,
                                            gint pan, gint tilt, gint zoom) {
  gboolean any = FALSE;
  gboolean ok = TRUE;

  if (self->pan_supported) {
    any = TRUE;
    ok = gst_libuvc_h264_src_ptz_set_pan(self, pan) && ok;
  }
  if (self->tilt_supported) {
    any = TRUE;
    ok = gst_libuvc_h264_src_ptz_set_tilt(self, tilt) && ok;
  }
  if (self->zoom_supported) {
    any = TRUE;
    ok = gst_libuvc_h264_src_ptz_set_zoom(self, zoom) && ok;
  }

  return any && ok;
}

static gboolean gst_libuvc_h264_set_clock(GstElement *element, GstClock *clock) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(element);

  GST_OBJECT_LOCK(self);

  if (self->clock) {
    gst_object_unref(self->clock);
    self->clock = NULL;
  }

  if (clock) {
    self->clock = gst_object_ref(clock);
    self->base_time = G_MAXUINT64;
    self->prev_pts = G_MAXUINT64;
    self->pts_offset_sum = 0;
    self->pts_stretch = 0;
  }

  GST_OBJECT_UNLOCK(self);

  return GST_ELEMENT_CLASS(gst_libuvc_h264_src_parent_class)->set_clock(element, clock);
}

/* The `index` property selects ONE device from the libuvc enumeration. It stays
 * a string (downstream callers pass a bare ordinal), but now also
 * accepts richer, hardware-stable selectors. Parsed ONCE at start():
 *   "N"            ordinal into the enumerated list (UNCHANGED, the default)
 *   "vid:pid"      hex vendor:product id, e.g. "1234:5678"
 *   "serial:<sn>"  exact USB serial-number string
 *   "bus:<b>:<a>"  decimal USB bus number and device address */
typedef enum {
  UVC_SEL_ORDINAL,
  UVC_SEL_VID_PID,
  UVC_SEL_SERIAL,
  UVC_SEL_BUS_ADDR,
} GstLibuvcSelectorType;

typedef struct {
  GstLibuvcSelectorType type;
  long ordinal;
  guint16 vid, pid;
  const gchar *serial;   /* borrows the index string, not owned */
  guint8 bus, addr;
} GstLibuvcDeviceSelector;

/* Parse one integer token that MUST consume the whole string (no trailing junk,
 * no overflow, within [min,max]). base 10 or 16. Mirrors the Task-6 strtol
 * validation so the ordinal path is byte-for-byte as strict as before. */
static gboolean
gst_libuvc_h264_src_parse_uint(const gchar *s, int base, long min, long max,
                               long *out) {
  if (s == NULL || *s == '\0')
    return FALSE;
  errno = 0;
  char *end = NULL;
  long v = strtol(s, &end, base);
  if (end == s || *end != '\0' || errno != 0 || v < min || v > max)
    return FALSE;
  *out = v;
  return TRUE;
}

/* Parse the `index` property into a selector. Returns FALSE with a human-readable
 * reason in *errmsg on a malformed selector (caller maps it to RESOURCE/SETTINGS).
 * A bare, non-negative decimal is the ordinal — anything that is not one of the
 * three prefixed forms falls back to the ordinal parse and so still fails loudly
 * (the old atoi()-silently-selects-0 trap stays closed). */
static gboolean
gst_libuvc_h264_src_parse_selector(const gchar *index,
                                   GstLibuvcDeviceSelector *sel,
                                   const gchar **errmsg) {
  if (index == NULL) {
    *errmsg = "index is NULL";
    return FALSE;
  }

  if (g_str_has_prefix(index, "serial:")) {
    const gchar *sn = index + strlen("serial:");
    if (*sn == '\0') {
      *errmsg = "serial selector requires a non-empty serial number";
      return FALSE;
    }
    sel->type = UVC_SEL_SERIAL;
    sel->serial = sn;
    return TRUE;
  }

  if (g_str_has_prefix(index, "bus:")) {
    const gchar *rest = index + strlen("bus:");
    const gchar *colon = strchr(rest, ':');
    if (colon == NULL) {
      *errmsg = "bus selector requires \"bus:<bus>:<addr>\"";
      return FALSE;
    }
    gchar *bus_str = g_strndup(rest, (gsize)(colon - rest));
    long bus_v = 0, addr_v = 0;
    gboolean ok = gst_libuvc_h264_src_parse_uint(bus_str, 10, 0, 255, &bus_v) &&
                  gst_libuvc_h264_src_parse_uint(colon + 1, 10, 0, 255, &addr_v);
    g_free(bus_str);
    if (!ok) {
      *errmsg = "bus selector requires \"bus:<bus>:<addr>\" (decimal 0..255 each)";
      return FALSE;
    }
    sel->type = UVC_SEL_BUS_ADDR;
    sel->bus = (guint8)bus_v;
    sel->addr = (guint8)addr_v;
    return TRUE;
  }

  /* A colon with no recognised prefix is the hex vid:pid form. */
  const gchar *colon = strchr(index, ':');
  if (colon != NULL) {
    gchar *vid_str = g_strndup(index, (gsize)(colon - index));
    long vid_v = 0, pid_v = 0;
    gboolean ok = gst_libuvc_h264_src_parse_uint(vid_str, 16, 0, 0xFFFF, &vid_v) &&
                  gst_libuvc_h264_src_parse_uint(colon + 1, 16, 0, 0xFFFF, &pid_v);
    g_free(vid_str);
    if (!ok) {
      *errmsg = "vid:pid selector requires hex \"<vid>:<pid>\" (0000..ffff each)";
      return FALSE;
    }
    sel->type = UVC_SEL_VID_PID;
    sel->vid = (guint16)vid_v;
    sel->pid = (guint16)pid_v;
    return TRUE;
  }

  long ord = 0;
  if (!gst_libuvc_h264_src_parse_uint(index, 10, 0, INT_MAX, &ord)) {
    *errmsg = "index must be a non-negative integer ordinal, \"vid:pid\", "
              "\"serial:<sn>\", or \"bus:<bus>:<addr>\"";
    return FALSE;
  }
  sel->type = UVC_SEL_ORDINAL;
  sel->ordinal = ord;
  return TRUE;
}

/* Test one enumerated device against the parsed selector. `ordinal` is the
 * device's position in the libuvc list. vid:pid and serial reads go through the
 * libuvc descriptor (freed before returning); bus/addr read the cached topology.
 * A device whose descriptor cannot be read simply does not match. */
static gboolean
gst_libuvc_h264_src_selector_matches(const GstLibuvcDeviceSelector *sel,
                                     uvc_device_t *dev, int ordinal) {
  switch (sel->type) {
    case UVC_SEL_ORDINAL:
      return (long)ordinal == sel->ordinal;
    case UVC_SEL_VID_PID: {
      uvc_device_descriptor_t *desc = NULL;
      if (uvc_get_device_descriptor(dev, &desc) != UVC_SUCCESS || desc == NULL)
        return FALSE;
      gboolean ok = (desc->idVendor == sel->vid && desc->idProduct == sel->pid);
      uvc_free_device_descriptor(desc);
      return ok;
    }
    case UVC_SEL_SERIAL: {
      uvc_device_descriptor_t *desc = NULL;
      if (uvc_get_device_descriptor(dev, &desc) != UVC_SUCCESS || desc == NULL)
        return FALSE;
      gboolean ok = (desc->serialNumber != NULL &&
                     g_strcmp0(desc->serialNumber, sel->serial) == 0);
      uvc_free_device_descriptor(desc);
      return ok;
    }
    case UVC_SEL_BUS_ADDR:
      return (uvc_get_bus_number(dev) == sel->bus &&
              uvc_get_device_address(dev) == sel->addr);
  }
  return FALSE;
}

static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);
  uvc_error_t res;

  GST_DEBUG_OBJECT(self, "Starting libuvc source");

  // Check if we need to cleanup a previous session
  if (self->uvc_ctx != NULL || self->uvc_devh != NULL) {
    GST_WARNING_OBJECT(self, "Previous session not fully cleaned up, forcing cleanup");
    gst_libuvc_h264_src_stop(src);
    usleep(1000000); // Wait 1 second for USB to settle
  }

  // Reset per-session frame state so a restart never forwards stale non-IDR
  // frames (or a stale PTS baseline) before a fresh IDR re-establishes the
  // stream. had_idr/send_sps_pps gate NAL forwarding in frame_callback(),
  // frame_count/prev_int_ts seed the PTS interval estimator, and prev_pts/
  // base_time use G_MAXUINT64 as the "latch on first frame" sentinel that
  // frame_callback() and create() test for.
  self->had_idr = FALSE;
  self->send_sps_pps = FALSE;
  self->frame_count = 0;
  self->frame_offset = 0;
  self->prev_int_ts = 0;
  self->prev_pts = G_MAXUINT64;
  self->base_time = G_MAXUINT64;
  self->consecutive_timeouts = 0;

  // Resolve the device selector up-front, before touching libuvc, so a
  // malformed index fails loudly here instead of silently selecting device 0.
  GstLibuvcDeviceSelector selector = {0};
  const gchar *parse_err = NULL;
  if (!gst_libuvc_h264_src_parse_selector(self->index, &selector, &parse_err)) {
    GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
        ("Invalid device index \"%s\"", self->index ? self->index : "(null)"),
        ("%s", parse_err));
    return FALSE;
  }
  long device_ordinal = -1;

  // Initialize libuvc context
  res = uvc_init(&self->uvc_ctx, NULL);
  if (res < 0) {
    GST_ERROR_OBJECT(self, "Failed to initialize libuvc: %s", uvc_strerror(res));
    return FALSE;
  }
  
  uvc_device_t **dev_list;
  res = uvc_find_devices(self->uvc_ctx, &dev_list, 0, 0, NULL);
  if (res < 0) {
    GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
        ("No UVC devices found"),
        ("uvc_find_devices failed: %s", uvc_strerror(res)));
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
    return FALSE;
  }

  for (int i = 0; dev_list[i] != NULL; ++i) {
    if (gst_libuvc_h264_src_selector_matches(&selector, dev_list[i], i)) {
      self->uvc_dev = dev_list[i];
      device_ordinal = i;
      break;
    }
  }

  if (!self->uvc_dev) {
    GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
        ("No UVC device matching \"%s\"", self->index ? self->index : "(null)"),
        ("selector matched none of the enumerated UVC devices"));
    uvc_free_device_list(dev_list, 1);
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
    return FALSE;
  }

  // The selected device aliases an entry in dev_list, and uvc_free_device_list()
  // unrefs every entry; take our own reference first so it survives the free.
  uvc_ref_device(self->uvc_dev);
  uvc_free_device_list(dev_list, 1);

  res = uvc_open(self->uvc_dev, &self->uvc_devh);
  if (res < 0) {
    GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE,
        ("Unable to open UVC device at index %ld", device_ordinal),
        ("uvc_open failed: %s", uvc_strerror(res)));
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
    return FALSE;
  }

  gst_libuvc_h264_src_v4l2_probe(GST_ELEMENT(self), (int)device_ordinal);

  // Probe PTZ ranges so only axes the device actually exposes are driven (M6).
  gst_libuvc_h264_src_ptz_probe_capabilities(self);

  // Opt-in control socket (M9): bind here BEFORE the thread so the listening fd
  // exists before any accept(); a bind failure is non-fatal to the media path.
  if (self->control_socket_enabled) {
    if (gst_libuvc_h264_src_control_socket_bind(self)) {
      self->control_running = TRUE;
      self->control_thread = g_thread_new("uvc-control",
                                          gst_libuvc_h264_src_control_thread,
                                          self);
    } else {
      GST_WARNING_OBJECT(self, "Control socket enabled but bind failed; "
                         "continuing without it");
    }
  }

  GST_DEBUG_OBJECT(self, "Libuvc source started successfully");
  return TRUE;
}

// FIXED: Proper cleanup with libusb handle release
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  GST_DEBUG_OBJECT(self, "Stopping libuvc source");

  // Stop control thread
  if (self->control_running) {
    GST_DEBUG_OBJECT(self, "Stopping control thread");
    self->control_running = FALSE;

    // Nudge the thread out of its select() at once by self-connecting to the
    // bound path; the 1s select timeout is the fallback if this misses.
    if (self->control_socket_path != NULL) {
      int wakeup_fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (wakeup_fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        g_strlcpy(addr.sun_path, self->control_socket_path, sizeof(addr.sun_path));
        fcntl(wakeup_fd, F_SETFL, O_NONBLOCK);
        connect(wakeup_fd, (struct sockaddr*)&addr, sizeof(addr));
        close(wakeup_fd);
      }
    }

    if (self->control_thread) {
      g_thread_join(self->control_thread);
      self->control_thread = NULL;
    }
  }

  // Close the listening fd and unlink the per-instance socket path.
  gst_libuvc_h264_src_control_socket_unbind(self);

  // CRITICAL FIX: Stop streaming and force USB release
  if (self->streaming && self->uvc_devh) {
    GST_DEBUG_OBJECT(self, "Stopping UVC streaming");
    uvc_stop_streaming(self->uvc_devh);
    self->streaming = FALSE;
    usleep(100000); // 100ms for streaming to stop
  }

  // Clear frame queue
  if (self->frame_queue) {
    GstBuffer *buffer;
    while ((buffer = g_async_queue_try_pop(self->frame_queue)) != NULL) {
      gst_buffer_unref(buffer);
    }
  }

  // FIXED: Release USB device BEFORE uvc_close
  if (self->uvc_devh) {
    GST_DEBUG_OBJECT(self, "Force releasing USB device");
    gst_libuvc_h264_src_force_usb_release(self);
    
    // Now call uvc_close (it may fail but that's OK since we already released)
    uvc_close(self->uvc_devh);
    self->uvc_devh = NULL;
  }

  // Unreference UVC device
  if (self->uvc_dev) {
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
  }

  // Exit UVC context
  if (self->uvc_ctx) {
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
  }

  // control_mutex is NOT cleared here: stop() runs on every restart and is even
  // re-entered from start()'s cleanup path, so clearing it would leave the
  // control thread locking a destroyed mutex. It is cleared once in finalize().

  GST_DEBUG_OBJECT(self, "Libuvc source fully stopped");
  return TRUE;
}

// Interrupt a create() that is blocked waiting for a frame (e.g. on disconnect
// or shutdown), so state changes and teardown never deadlock on a silent source.
static gboolean gst_libuvc_h264_src_unlock(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  GST_DEBUG_OBJECT(self, "Unlock: interrupting create()");

  g_atomic_int_set(&self->flushing, 1);
  g_async_queue_push(self->frame_queue, FLUSH_SENTINEL);

  return TRUE;
}

static gboolean gst_libuvc_h264_src_unlock_stop(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  GST_DEBUG_OBJECT(self, "Unlock stop: resuming create()");

  g_atomic_int_set(&self->flushing, 0);

  // Drop sentinels (and any frames buffered during the flush) so the next
  // create() resumes from a clean queue.
  gpointer item;
  while ((item = g_async_queue_try_pop(self->frame_queue)) != NULL) {
    if (item != FLUSH_SENTINEL) {
      gst_buffer_unref(item);
    }
  }

  return TRUE;
}

/* Opt-in in-element reconnect (Task 18), gated on the Task 4 spike verdict.
 *
 * Runs on the streaming thread from inside create() after a sustained-silence
 * disconnect is detected, so it never races stop() (GstBaseSrc serialises them;
 * unlock() only sets the flushing flag). Tears the dead handle down with the
 * spike's verified NATIVE sequence and re-resolves the `index` selector against
 * a fresh enumeration (bus/address can change across a replug), then reopens and
 * restarts streaming with bounded exponential backoff. Returns TRUE once
 * streaming has resumed, FALSE if every retry was exhausted or a concurrent
 * unlock() asked us to bail.
 *
 * CRITICAL: never call gst_libuvc_h264_src_force_usb_release() here — the spike
 * proved force_usb_release()+uvc_close() double-closes the libusb handle. The
 * native uvc_stop_streaming()->uvc_close() owns the single libusb_close(). */
static gboolean gst_libuvc_h264_src_reconnect(GstLibuvcH264Src *self) {
  GstLibuvcDeviceSelector selector = {0};
  const gchar *parse_err = NULL;
  if (!gst_libuvc_h264_src_parse_selector(self->index, &selector, &parse_err)) {
    GST_ERROR_OBJECT(self, "Reconnect: invalid index \"%s\": %s",
                     self->index ? self->index : "(null)", parse_err);
    return FALSE;
  }

  // Native teardown of the dead handle (NO force_usb_release — double-free).
  if (self->uvc_devh) {
    uvc_stop_streaming(self->uvc_devh);
    self->streaming = FALSE;
    uvc_close(self->uvc_devh);
    self->uvc_devh = NULL;
  }
  if (self->uvc_dev) {
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
  }

  // Drop anything left in the queue so a resumed stream never forwards a frame
  // captured before the disconnect (offset/PTS would be inconsistent).
  gpointer stale;
  while ((stale = g_async_queue_try_pop(self->frame_queue)) != NULL) {
    if (stale != FLUSH_SENTINEL) {
      gst_buffer_unref(stale);
    }
  }

  guint backoff_s = RECONNECT_BACKOFF_INITIAL_S;
  for (int attempt = 0; attempt < RECONNECT_MAX_RETRIES; attempt++) {
    // Interruptible backoff: bail at once if unlock() flagged a flush, so
    // teardown never blocks for the full (up to 16 s) backoff window.
    for (guint slept_ms = 0; slept_ms < backoff_s * 1000; slept_ms += 100) {
      if (g_atomic_int_get(&self->flushing)) {
        return FALSE;
      }
      g_usleep(100 * 1000);
    }

    GST_DEBUG_OBJECT(self, "Reconnect attempt %d/%d (after %u s backoff)",
                     attempt + 1, RECONNECT_MAX_RETRIES, backoff_s);
    backoff_s *= 2;

    uvc_device_t **dev_list = NULL;
    if (uvc_find_devices(self->uvc_ctx, &dev_list, 0, 0, NULL) < 0 ||
        dev_list == NULL) {
      continue;
    }

    uvc_device_t *selected = NULL;
    for (int i = 0; dev_list[i] != NULL; i++) {
      if (gst_libuvc_h264_src_selector_matches(&selector, dev_list[i], i)) {
        selected = dev_list[i];
        break;
      }
    }
    if (selected == NULL) {
      uvc_free_device_list(dev_list, 1);
      continue;
    }

    // Ref the chosen device before freeing the list (free unrefs every entry).
    uvc_ref_device(selected);
    uvc_free_device_list(dev_list, 1);

    if (uvc_open(selected, &self->uvc_devh) < 0) {
      self->uvc_devh = NULL;
      uvc_unref_device(selected);
      continue;
    }
    self->uvc_dev = selected;

    if (uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
            self->frame_format, self->negotiated_width, self->negotiated_height,
            self->negotiated_framerate) < 0) {
      uvc_close(self->uvc_devh);
      self->uvc_devh = NULL;
      uvc_unref_device(self->uvc_dev);
      self->uvc_dev = NULL;
      continue;
    }

    // Re-arm the stream state BEFORE the feeder spawns so frame_callback sees the
    // reset (pthread_create in uvc_start_streaming is the happens-before edge):
    // re-latch the PTS baseline and re-engage the IDR gate after the gap.
    self->had_idr = FALSE;
    self->send_sps_pps = FALSE;
    self->base_time = G_MAXUINT64;
    self->prev_pts = G_MAXUINT64;

    if (uvc_start_streaming(self->uvc_devh, &self->uvc_ctrl, frame_callback,
                            self, 0) < 0) {
      uvc_close(self->uvc_devh);
      self->uvc_devh = NULL;
      uvc_unref_device(self->uvc_dev);
      self->uvc_dev = NULL;
      continue;
    }

    self->streaming = TRUE;
    GST_INFO_OBJECT(self, "Reconnect succeeded on attempt %d", attempt + 1);
    return TRUE;
  }

  GST_WARNING_OBJECT(self, "Reconnect exhausted after %d attempts",
                     RECONNECT_MAX_RETRIES);
  return FALSE;
}

static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);
  uvc_error_t res;

  if (!self->streaming) {
    self->base_time = G_MAXUINT64;
    self->prev_pts = G_MAXUINT64;
    self->pts_offset_sum = 0;
    self->pts_stretch = 0;

    self->streaming = TRUE;

    // Start streaming
    res = uvc_start_streaming(self->uvc_devh, &self->uvc_ctrl, frame_callback, self, 0);
    if (res < 0) {
      self->streaming = FALSE;
      GST_ERROR_OBJECT(self, "Unable to start streaming: %s", uvc_strerror(res));
      return GST_FLOW_ERROR;
    }
  }

  // Bounded wait so unlock() can interrupt a stalled capture: the timeout is a
  // backstop for a silent source, while unlock()'s sentinel wakes us at once.
  while (TRUE) {
    gpointer item = g_async_queue_timeout_pop(self->frame_queue, TIMEOUT_DURATION);

    if (g_atomic_int_get(&self->flushing)) {
      if (item != NULL && item != FLUSH_SENTINEL) {
        gst_buffer_unref(item);
      }
      return GST_FLOW_FLUSHING;
    }

    if (item == NULL) {
      // A real pop timeout. libuvc delivers no NULL frame on unplug in callback
      // mode (it just goes silent, per the Task 4 spike), so sustained silence
      // is how a disconnect is detected. Count consecutive timeouts; a single
      // gap is tolerated, but DISCONNECT_TIMEOUT_COUNT in a row means the device
      // is gone.
      if (++self->consecutive_timeouts < DISCONNECT_TIMEOUT_COUNT) {
        continue;
      }

      GST_WARNING_OBJECT(self, "Device silent for %d s, assuming disconnect",
                         DISCONNECT_TIMEOUT_COUNT);

      // Opt-in reconnect: try to resume before erroring. Default off, so a
      // disconnect always surfaces as a RESOURCE/READ error downstream.
      if (self->reconnect_enabled && gst_libuvc_h264_src_reconnect(self)) {
        self->consecutive_timeouts = 0;
        continue;
      }

      // A flush raced in during the reconnect backoff: honour it over the error.
      if (g_atomic_int_get(&self->flushing)) {
        return GST_FLOW_FLUSHING;
      }

      gst_libuvc_h264_src_post_disconnect_error(GST_ELEMENT(self));
      return GST_FLOW_ERROR;
    }

    if (item == FLUSH_SENTINEL) {
      // A stale sentinel from a finished flush: not silence, so reset the
      // disconnect counter and keep waiting.
      self->consecutive_timeouts = 0;
      continue;
    }

    // A real frame arrived: silence is broken.
    self->consecutive_timeouts = 0;
    *buf = item;
    return GST_FLOW_OK;
  }
}

static void gst_libuvc_h264_src_finalize(GObject *object) {
    GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

    GST_DEBUG_OBJECT(self, "Finalizing libuvc source");

    // Force cleanup
    gst_libuvc_h264_src_stop(GST_BASE_SRC(self));

    if (self->index) {
        g_free(self->index);
        self->index = NULL;
    }

    // stop() above already unlinked the socket; free the owned path string.
    if (self->control_socket_path) {
        g_free(self->control_socket_path);
        self->control_socket_path = NULL;
    }

    if (self->frame_queue) {
        GstBuffer *buffer;
        while ((buffer = g_async_queue_try_pop(self->frame_queue)) != NULL) {
            gst_buffer_unref(buffer);
        }
        
        g_async_queue_unref(self->frame_queue);
        self->frame_queue = NULL;
    }

    // Sole clear point for control_mutex (paired with g_mutex_init in init): the
    // control thread was already joined by stop() above, so this is race-free.
    g_mutex_clear(&self->control_mutex);

    GST_DEBUG_OBJECT(self, "Libuvc source finalized");

    G_OBJECT_CLASS(gst_libuvc_h264_src_parent_class)->finalize(object);
}

static gboolean plugin_init(GstPlugin *plugin) {
    // Also register under the libuvch26xsrc alias since it now supports both H264 and H265
    if (!gst_element_register(plugin, "libuvch26xsrc", GST_RANK_NONE, GST_TYPE_LIBUVC_H264_SRC))
      return FALSE;
    return gst_element_register(plugin, "libuvch264src", GST_RANK_NONE, GST_TYPE_LIBUVC_H264_SRC);
}

#define PACKAGE "libuvch264src"
#define VERSION "2.0"
GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    libuvch264src,
    "UVC H264 / H265 Source Plugin",
    plugin_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "https://gstreamer.freedesktop.org/"
)
