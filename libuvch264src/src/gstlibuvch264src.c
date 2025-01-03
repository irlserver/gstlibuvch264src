#include <sys/stat.h>
#include <libusb-1.0/libusb.h>
#include "gstlibuvch264src.h"
#include <gst/gst.h>
#include <libuvc/libuvc.h>

GST_DEBUG_CATEGORY_STATIC(gst_libuvc_h264_src_debug);
#define GST_CAT_DEFAULT gst_libuvc_h264_src_debug

typedef struct {
    int type;
    unsigned char *ptr;
    int len;
} nal_unit_t;

enum {
  PROP_0,
  PROP_INDEX,
  PROP_LAST
};

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
  "src",
  GST_PAD_SRC,
  GST_PAD_ALWAYS,
  GST_STATIC_CAPS("video/x-h264, "
                  "stream-format=(string)byte-stream, "
                  "alignment=(string)au")
);

G_DEFINE_TYPE_WITH_CODE(GstLibuvcH264Src, gst_libuvc_h264_src, GST_TYPE_PUSH_SRC,
  GST_DEBUG_CATEGORY_INIT(gst_libuvc_h264_src_debug, "libuvch264src", 0, "libuvch264src element"));

static gboolean gst_libuvc_h264_src_set_caps(GstBaseSrc *basesrc, GstCaps *caps);
static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec);
static void gst_libuvc_h264_src_get_property(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec);
static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src);
static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf);
static void gst_libuvc_h264_src_finalize(GObject *object);

static void gst_libuvc_h264_src_class_init(GstLibuvcH264SrcClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);

  base_src_class->set_caps = GST_DEBUG_FUNCPTR(gst_libuvc_h264_src_set_caps);
  gobject_class->set_property = gst_libuvc_h264_src_set_property;
  gobject_class->get_property = gst_libuvc_h264_src_get_property;

  g_object_class_install_property(gobject_class, PROP_INDEX,
    g_param_spec_string("index", "Index", "Device location, e.g., '0'",
                        DEFAULT_DEVICE_INDEX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata(element_class,
    "UVC H.264 Video Source", "Source/Video",
    "Captures H.264 video from a UVC device", "Name");

  gst_element_class_add_pad_template(element_class,
    gst_static_pad_template_get(&src_template));

  base_src_class->start = gst_libuvc_h264_src_start;
  base_src_class->stop = gst_libuvc_h264_src_stop;
  push_src_class->create = gst_libuvc_h264_src_create;
  gobject_class->finalize = gst_libuvc_h264_src_finalize;
}

#define DIRBUFLEN 4096
__thread char dir_buf[DIRBUFLEN];
char *get_spspps_path(GstLibuvcH264Src *self, char *index) {
    const char *home_dir = getenv("HOME");
    if (home_dir == NULL) {
        GST_WARNING_OBJECT(self, "Warning: HOME environment variable not set.");
        home_dir = "";
    }

	int ret = snprintf(dir_buf, DIRBUFLEN, "%s/.spspps%s%s",
	                   home_dir,
	                   index ? "/" : "",
	                   index ? index : "");
	if (ret >= DIRBUFLEN) {
	    GST_ERROR_OBJECT(self, "Error building SPS/PPS path\n");
	    return NULL;
	}

	return dir_buf;
}

void create_hidden_directory(GstLibuvcH264Src *self) {
    char *hidden_dir = get_spspps_path(self, NULL);

    // Check if the directory exists
    struct stat st;
    if (stat(hidden_dir, &st) == -1) {
        // Directory does not exist; create it
        if (mkdir(hidden_dir, 0700) != 0)
            GST_ERROR_OBJECT(self, "Error creating directory %s\n", hidden_dir);
        else
            GST_WARNING_OBJECT(self, "Directory %s created successfully.\n", hidden_dir);
    } else if (!S_ISDIR(st.st_mode))
        // Path exists but is not a directory
        GST_WARNING_OBJECT(self, "Warning: %s exists but is not a directory.\n", hidden_dir);
}

FILE *open_spspps_file(GstLibuvcH264Src *self, char mode) {
    if (mode == 'w' || mode == 'a') {
        create_hidden_directory(self);
    }

    char m[3];
    sprintf(m, "%cb", mode);
    char *file_name = get_spspps_path(self, self->index);
    FILE *fp = fopen(file_name, m);
    return fp;
}

int find_nal_unit(unsigned char *buf, int buflen, int start, int search, int *offset) {
    if (buflen < (start + 5)) return -1;

    int i = start;
    do {
        if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1) {
            if (offset) *offset = i;
            return (buf[i+4] & 0x1F);
        }
        i++;
    } while (search && i < (buflen - 4));

    return -1;
}

int parse_nal_units(nal_unit_t *units, int max, unsigned char *buf, int buflen) {
    int i = 0;

    int nal_offset = 0;
    int next_type = find_nal_unit(buf, buflen, 0, 0, &nal_offset);
    while (next_type >= 0 && i < max) {
        int type = next_type;
        int start = nal_offset;
        next_type = find_nal_unit(buf, buflen, nal_offset + 5, 1, &nal_offset);
        int end = (next_type >= 0) ? nal_offset : buflen;
        int length = end - start;

        units[i].type = type;
        units[i].len = length;
        units[i].ptr = &buf[start];

        i++;
    }

    return i;
}

void load_spspps(GstLibuvcH264Src *self) {
    FILE* fp = open_spspps_file(self, 'r');
    if (fp) {
        unsigned char buf[SPSPPSBUFSZ*2];
        gint read_bytes = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);

        #define MAX_UNITS_LOAD 2
        nal_unit_t units[MAX_UNITS_LOAD];
        int c = parse_nal_units(units, MAX_UNITS_LOAD, buf, read_bytes);

        for (int i = 0; i < c; i++) {
            if (units[i].type == 7) {
                memcpy(self->sps, units[i].ptr, units[i].len);
                self->sps_length = units[i].len;
            } else if (units[i].type == 8) {
                memcpy(self->pps, units[i].ptr, units[i].len);
                self->pps_length = units[i].len;
            }
        }
    }
}

void store_spspps(GstLibuvcH264Src *self) {
    FILE* fp = open_spspps_file(self, 'w');
	if (fp) {
		fwrite(self->sps, 1, self->sps_length, fp);
		fwrite(self->pps, 1, self->pps_length, fp);
		fclose(fp);
	}
}

static void gst_libuvc_h264_src_init(GstLibuvcH264Src *self) {
  self->index = g_strdup(DEFAULT_DEVICE_INDEX);
  self->uvc_ctx = NULL;
  self->uvc_dev = NULL;
  self->uvc_devh = NULL;
  self->frame_queue = g_async_queue_new();
  self->streaming = FALSE;
  self->width = DEFAULT_WIDTH;
  self->height = DEFAULT_HEIGHT;
  self->framerate = DEFAULT_FRAMERATE;
  self->frame_count = 0; // Added to track frames for timestamping

  // Initialization, not fixed
  gchar sps[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x34, 0xAC, 0x4D, 0x00, 0xF0, 0x04, 0x4F, 0xCB, 0x35, 0x01, 0x01, 0x01, 0x40, 0x00, 0x00, 0xFA, 0x00, 0x00, 0x3A, 0x98, 0x03, 0xC7, 0x0C, 0xA8 };
  self->sps_length = sizeof(sps);
  memcpy(self->sps, sps, self->sps_length);

  gchar pps[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };
  self->pps_length = sizeof(pps);
  memcpy(self->pps, pps, self->pps_length);

  gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
  gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
}

static gboolean gst_libuvc_h264_src_set_caps(GstBaseSrc *basesrc, GstCaps *caps) {
    GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(basesrc);
    GstStructure *structure = gst_caps_get_structure(caps, 0);

    if (gst_structure_has_name(structure, "video/x-h264")) {
        gint width, height, framerate_num, framerate_den;

        if (gst_structure_get_int(structure, "width", &width))
            self->width = width;
        if (gst_structure_get_int(structure, "height", &height))
            self->height = height;
        if (gst_structure_get_fraction(structure, "framerate", &framerate_num, &framerate_den) && framerate_den != 0)
            self->framerate = framerate_num / framerate_den;

        GST_INFO("Set caps: width=%d, height=%d, framerate=%d", self->width, self->height, self->framerate);
    } else {
        GST_ERROR("Unsupported caps: %s", gst_structure_get_name(structure));
        return FALSE;
    }

    return TRUE;
}

static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

  switch (prop_id) {
    case PROP_INDEX:
      g_free(self->index);
      self->index = g_value_dup_string(value);
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);
  uvc_error_t res;

  // Initialize libuvc context
  res = uvc_init(&self->uvc_ctx, NULL);
  if (res < 0) {
    GST_ERROR_OBJECT(self, "Failed to initialize libuvc: %s", uvc_strerror(res));
    return FALSE;
  }
  
  uvc_device_t **dev_list;
  res = uvc_find_devices(self->uvc_ctx, &dev_list, 0, 0, NULL);
  if (res < 0) {
    GST_ERROR_OBJECT(self, "Unable to find any UVC devices");
    uvc_exit(self->uvc_ctx);
    return FALSE;
  }

  for (int i = 0; dev_list[i] != NULL; ++i) {
    uvc_device_t *dev = dev_list[i];
	if (i == atoi(self->index)) {
		self->uvc_dev = dev;
		break;
	}
  }
  
  if (!self->uvc_dev) {
    GST_ERROR_OBJECT(self, "Unable to find UVC device: %s", self->index);
    uvc_exit(self->uvc_ctx);
    return FALSE;
  }

  // Open the UVC device
  res = uvc_open(self->uvc_dev, &self->uvc_devh);
  if (res < 0) {
    GST_ERROR_OBJECT(self, "Unable to open UVC device: %s", uvc_strerror(res));
    uvc_unref_device(self->uvc_dev);
    uvc_exit(self->uvc_ctx);
    return FALSE;
  }
  
  // Enumerate and log supported formats and frame sizes
  const uvc_format_desc_t *format_desc = uvc_get_format_descs(self->uvc_devh);
  while (format_desc) {
      GST_INFO("Found format: %s", format_desc->guidFormat);
      const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
      while (frame_desc) {
          GST_INFO("  Frame size: %ux%u", frame_desc->wWidth, frame_desc->wHeight);
		  if (frame_desc->intervals) {
            const uint32_t *interval = frame_desc->intervals;
            while (*interval) {
                double fps = 1.0 / (*interval * 1e-7);
                GST_INFO("    Frame rate: %.2f fps", fps);
                interval++;
            }
		  }
		  else {
			GST_INFO("    Frame rate range: %.2f - %.2f fps",
					 1.0 / (frame_desc->dwMaxFrameInterval * 1e-7),
					 1.0 / (frame_desc->dwMinFrameInterval * 1e-7));
		  }
		  
          frame_desc = frame_desc->next;
      }
	  
      format_desc = format_desc->next;
  }

  res = uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
                                        UVC_FRAME_FORMAT_H264, self->width, self->height, self->framerate);
  if (res < 0) {
    GST_ERROR_OBJECT(self, "Unable to get stream control: %s", uvc_strerror(res));
    uvc_close(self->uvc_devh);
    uvc_unref_device(self->uvc_dev);
    uvc_exit(self->uvc_ctx);
    return FALSE;
  }
  
  load_spspps(self);

  return TRUE;
}

static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  if (self->streaming) {
    uvc_stop_streaming(self->uvc_devh);
    self->streaming = FALSE;
  }

  if (self->uvc_devh) {
    uvc_close(self->uvc_devh);
    self->uvc_devh = NULL;
  }

  if (self->uvc_dev) {
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
  }

  if (self->uvc_ctx) {
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
  }

  return TRUE;
}

// Callback to handle frame data
void frame_callback(uvc_frame_t *frame, void *ptr) {
    GstLibuvcH264Src *self = (GstLibuvcH264Src *)ptr;

    if (!frame || !frame->data || frame->data_bytes <= 0) {
        GST_WARNING_OBJECT(self, "Empty or invalid frame received.");
        return;
    }
	
	unsigned char* data = frame->data;
    gboolean updated_sps_pps = FALSE;

    #define MAX_UNITS_MAIN 10
    nal_unit_t units[MAX_UNITS_MAIN];
    int c = parse_nal_units(units, MAX_UNITS_MAIN, data, frame->data_bytes);

    for (int i = 0; i < c; i++) {
        nal_unit_t *unit = &units[i];

        switch (unit->type) {
            case 7:
                self->sps_length = unit->len;
                memcpy(self->sps, unit->ptr, self->sps_length);
                updated_sps_pps = TRUE;
                break;
            case 8:
                self->pps_length = unit->len;
                memcpy(self->pps, unit->ptr, self->pps_length);
                updated_sps_pps = TRUE;
                break;
            case 5: {
                if (!self->had_idr) {
                    self->had_idr = TRUE;

                    GstBuffer *buffer = gst_buffer_new_allocate(NULL, self->sps_length + self->pps_length, NULL);
                    gst_buffer_fill(buffer, 0, self->sps, self->sps_length);
                    gst_buffer_fill(buffer, self->sps_length, self->pps, self->pps_length);
                    g_async_queue_push(self->frame_queue, buffer);
                }
                break;
            }
            default:
                if (!self->had_idr) {
                    continue;
                }
        } // switch

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, unit->len, NULL);
        gst_buffer_fill(buffer, 0, unit->ptr, unit->len);

        // Set timestamps on the buffer
        if (units[i].type == 1 || units[i].type == 5) {
            GstClockTime timestamp = gst_util_uint64_scale(self->frame_count * GST_SECOND, 1, self->framerate);
            GST_BUFFER_PTS(buffer) = timestamp;
            GST_BUFFER_DTS(buffer) = timestamp;
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, self->framerate);
            self->frame_count++;
        }

        g_async_queue_push(self->frame_queue, buffer);
    } // for

    if (updated_sps_pps) {
        store_spspps(self);
    }
}

static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);
  uvc_error_t res;

  if (!self->streaming) {
    // Start streaming
    res = uvc_start_streaming(self->uvc_devh, &self->uvc_ctrl, frame_callback, self, 0);
    if (res < 0) {
      GST_ERROR_OBJECT(self, "Unable to start streaming: %s", uvc_strerror(res));
      return GST_FLOW_ERROR;
    }
    self->streaming = TRUE;
	self->frame_count = 0; // Initialize frame count for timestamping
  }

  // Retrieve a buffer from the queue
  *buf = g_async_queue_pop(self->frame_queue);
  if (*buf == NULL) {
    GST_ERROR_OBJECT(self, "No frame available.");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static void gst_libuvc_h264_src_finalize(GObject *object) {
    GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

    // Ensure streaming is stopped
    if (self->streaming) {
        uvc_stop_streaming(self->uvc_devh);
        self->streaming = FALSE;
    }

    // Unreference and free the frame queue
    if (self->frame_queue) {
        g_async_queue_unref(self->frame_queue);
        self->frame_queue = NULL;
    }

    // Chain up to the parent class
    G_OBJECT_CLASS(gst_libuvc_h264_src_parent_class)->finalize(object);
}

// Plugin initialization function
static gboolean plugin_init(GstPlugin *plugin) {
    // Register your element
    return gst_element_register(plugin, "libuvch264src", GST_RANK_NONE, GST_TYPE_LIBUVC_H264_SRC);
}

// Define the plugin using GST_PLUGIN_DEFINE
#define PACKAGE "libuvch264src"
#define VERSION "1.0"
GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    libuvch264src,
    "UVC H264 Source Plugin",
    plugin_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "https://gstreamer.freedesktop.org/"
)
