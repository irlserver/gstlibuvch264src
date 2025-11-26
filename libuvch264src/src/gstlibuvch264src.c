#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/select.h>
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

static gboolean gst_libuvc_h264_negotiate(GstBaseSrc * basesrc);
static void gst_libuvc_h264_src_set_property(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec);
static void gst_libuvc_h264_src_get_property(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec);
static gboolean gst_libuvc_h264_src_start(GstBaseSrc *src);
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src);
static GstFlowReturn gst_libuvc_h264_src_create(GstPushSrc *src, GstBuffer **buf);
static void gst_libuvc_h264_src_finalize(GObject *object);

// Forward declarations for control functions
static gpointer gst_libuvc_h264_src_control_thread(gpointer data);
static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command);

static void gst_libuvc_h264_src_class_init(GstLibuvcH264SrcClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);

  base_src_class->negotiate = GST_DEBUG_FUNCPTR(gst_libuvc_h264_negotiate);
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
  self->uvc_start_time = G_MAXUINT64;
  self->prev_pts = G_MAXUINT64;
  
  // Control socket initialization
  self->control_socket = -1;
  self->control_thread = NULL;
  self->control_running = FALSE;
  g_mutex_init(&self->control_mutex);

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

// Enhanced control socket thread function with proper shutdown
static gpointer gst_libuvc_h264_src_control_thread(gpointer data) {
    GstLibuvcH264Src *self = (GstLibuvcH264Src *)data;
    struct sockaddr_un addr;
    int client_fd;
    char buffer[256];
    fd_set read_fds;
    struct timeval timeout;
    
    // Create socket
    self->control_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (self->control_socket < 0) {
        GST_ERROR_OBJECT(self, "Failed to create control socket");
        return NULL;
    }
    
    // Set socket to non-blocking for clean shutdown
    int flags = fcntl(self->control_socket, F_GETFL, 0);
    fcntl(self->control_socket, F_SETFL, flags | O_NONBLOCK);
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/libuvc_control");
    
    // Remove existing socket
    unlink(addr.sun_path);
    
    if (bind(self->control_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        GST_ERROR_OBJECT(self, "Failed to bind control socket");
        close(self->control_socket);
        self->control_socket = -1;
        return NULL;
    }
    
    if (listen(self->control_socket, 5) < 0) {
        GST_ERROR_OBJECT(self, "Failed to listen on control socket");
        close(self->control_socket);
        self->control_socket = -1;
        return NULL;
    }
    
    GST_INFO_OBJECT(self, "Control socket listening on /tmp/libuvc_control");
    
    while (self->control_running) {
        FD_ZERO(&read_fds);
        FD_SET(self->control_socket, &read_fds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int result = select(self->control_socket + 1, &read_fds, NULL, NULL, &timeout);
        
        if (result > 0 && FD_ISSET(self->control_socket, &read_fds)) {
            client_fd = accept(self->control_socket, NULL, NULL);
            if (client_fd > 0) {
                ssize_t len = read(client_fd, buffer, sizeof(buffer)-1);
                if (len > 0) {
                    buffer[len] = 0;
                    GST_INFO_OBJECT(self, "Received control command: %s", buffer);
                    char *response = gst_libuvc_h264_src_process_control_command(self, buffer);
                    if (response) {
                        write(client_fd, response, strlen(response));
                        g_free(response);
                    } else {
                        const char *default_response = "OK";
                        write(client_fd, default_response, strlen(default_response));
                    }
                }
                close(client_fd);
            }
        } else if (result == 0) {
            // Timeout - check if we should continue running
            continue;
        } else {
            // Error or interrupted
            if (self->control_running) {
                GST_WARNING_OBJECT(self, "Select error in control thread");
            }
            break;
        }
    }
    
    GST_DEBUG_OBJECT(self, "Control thread exiting");
    return NULL;
}

// Enhanced control command processor with zoom support
static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command) {
    int pan, tilt, zoom;
    uint16_t zoom_abs;
    
    g_mutex_lock(&self->control_mutex);
    
    if (sscanf(command, "PAN_TILT %d %d", &pan, &tilt) == 2) {
        if (self->uvc_devh) {
            uvc_error_t res = uvc_set_pantilt_abs(self->uvc_devh, pan, tilt);
            if (res == UVC_SUCCESS) {
                GST_INFO_OBJECT(self, "Set pan/tilt to: %d/%d", pan, tilt);
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("OK pan=%d tilt=%d", pan, tilt);
            } else {
                GST_WARNING_OBJECT(self, "Failed to set pan/tilt: %s", uvc_strerror(res));
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("ERROR: %s", uvc_strerror(res));
            }
        }
    } 
    else if (sscanf(command, "ZOOM %d", &zoom) == 1) {
        if (self->uvc_devh) {
            zoom_abs = (uint16_t)zoom;
            uvc_error_t res = uvc_set_zoom_abs(self->uvc_devh, zoom_abs);
            if (res == UVC_SUCCESS) {
                GST_INFO_OBJECT(self, "Set zoom to: %d", zoom_abs);
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("OK zoom=%d", zoom_abs);
            } else {
                GST_WARNING_OBJECT(self, "Failed to set zoom: %s", uvc_strerror(res));
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("ERROR: %s", uvc_strerror(res));
            }
        }
    }
    else if (strcmp(command, "GET_POSITION") == 0) {
        if (self->uvc_devh) {
            int32_t current_pan, current_tilt;
            uint16_t current_zoom;
            char *response = NULL;
            
            uvc_error_t res_pan = uvc_get_pantilt_abs(self->uvc_devh, &current_pan, &current_tilt, UVC_GET_CUR);
            uvc_error_t res_zoom = uvc_get_zoom_abs(self->uvc_devh, &current_zoom, UVC_GET_CUR);
            
            if (res_pan == UVC_SUCCESS && res_zoom == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=%d tilt=%d zoom=%d", current_pan, current_tilt, current_zoom);
            } else if (res_pan == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=%d tilt=%d zoom=unknown", current_pan, current_tilt);
            } else if (res_zoom == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=unknown tilt=unknown zoom=%d", current_zoom);
            } else {
                response = g_strdup("ERROR: Cannot read position");
            }
            
            GST_INFO_OBJECT(self, "Current position: pan=%d, tilt=%d, zoom=%d", 
                           current_pan, current_tilt, current_zoom);
            g_mutex_unlock(&self->control_mutex);
            return response;
        }
    }
    else if (strcmp(command, "GET_CAPABILITIES") == 0) {
        if (self->uvc_devh) {
            GString *caps = g_string_new("CAPABILITIES:");
            
            // Check pan/tilt capabilities
            int32_t pan_min, pan_max, pan_step;
            int32_t tilt_min, tilt_max, tilt_step;
            uvc_error_t res_pt = uvc_get_pantilt_abs(self->uvc_devh, &pan_min, &tilt_min, UVC_GET_MIN);
            if (res_pt == UVC_SUCCESS) {
                uvc_get_pantilt_abs(self->uvc_devh, &pan_max, &tilt_max, UVC_GET_MAX);
                uvc_get_pantilt_abs(self->uvc_devh, &pan_step, &tilt_step, UVC_GET_RES);
                g_string_append_printf(caps, " pan=[%d,%d,step=%d] tilt=[%d,%d,step=%d]", 
                                      pan_min, pan_max, pan_step, tilt_min, tilt_max, tilt_step);
            }
            
            // Check zoom capabilities
            uint16_t zoom_min, zoom_max, zoom_step;
            uvc_error_t res_zoom = uvc_get_zoom_abs(self->uvc_devh, &zoom_min, UVC_GET_MIN);
            if (res_zoom == UVC_SUCCESS) {
                uvc_get_zoom_abs(self->uvc_devh, &zoom_max, UVC_GET_MAX);
                uvc_get_zoom_abs(self->uvc_devh, &zoom_step, UVC_GET_RES);
                g_string_append_printf(caps, " zoom=[%d,%d,step=%d]", zoom_min, zoom_max, zoom_step);
            }
            
            GST_INFO_OBJECT(self, "Capabilities: %s", caps->str);
            g_mutex_unlock(&self->control_mutex);
            return g_string_free(caps, FALSE);
        }
    }
    else if (strncmp(command, "RESET", 5) == 0) {
        // Reset to default position
        if (self->uvc_devh) {
            uvc_set_pantilt_abs(self->uvc_devh, 0, 0);
            uvc_set_zoom_abs(self->uvc_devh, 100); // Typical default zoom
            GST_INFO_OBJECT(self, "Reset to default position");
            g_mutex_unlock(&self->control_mutex);
            return g_strdup("OK reset complete");
        }
    }
    
    g_mutex_unlock(&self->control_mutex);
    return g_strdup("ERROR: Unknown command");
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

    GstCaps *tmp_caps = gst_caps_new_simple("video/x-h264",
                                            "stream-format", G_TYPE_STRING, "byte-stream",
                                            "alignment", G_TYPE_STRING, "au",
                                            NULL
                                           );
    GstStructure *tmp_structure = gst_caps_get_structure(tmp_caps, 0);

    // Enumerate supported H264 resolutions and framerates
    // And select the highest compatible resolution, at the highest supported framerate
    for (const uvc_format_desc_t *format_desc = uvc_get_format_descs(self->uvc_devh);
         format_desc; format_desc = format_desc->next)
    {
        gboolean is_h264 = (memcmp(format_desc->fourccFormat, "H264", 4) == 0);
        if (!is_h264) continue;

        for (const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
             frame_desc; frame_desc = frame_desc->next)
        {
            gint resolution = frame_desc->wWidth * frame_desc->wHeight;

            gst_structure_set(tmp_structure,
                              "width", G_TYPE_INT, frame_desc->wWidth,
                              "height", G_TYPE_INT, frame_desc->wHeight,
                              NULL);

            // This holds the highest framerate for the current resolution
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
                }

                gst_structure_set_value(tmp_structure, "framerate", &framerates);
            } else {
                gint fps_min = 1e7 / frame_desc->dwMaxFrameInterval;
                gint fps = 1e7 / frame_desc->dwMinFrameInterval;
                gst_structure_set(tmp_structure, "framerate", GST_TYPE_FRACTION_RANGE, fps_min, 1, fps, 1, NULL);
            }

            GST_INFO_OBJECT(basesrc, "Testing hw caps: %" GST_PTR_FORMAT "...", tmp_caps);

            if (gst_caps_can_intersect(caps, tmp_caps)) {
                GST_INFO_OBJECT(basesrc, "  caps valid");

                if (resolution > (width * height)
                    || (resolution == (width * height) && fps > framerate)) {
                    width = frame_desc->wWidth;
                    height = frame_desc->wHeight;

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
            } else {
                GST_INFO_OBJECT(basesrc, "  caps invalid");
            }

        } // for frame_desc
    } // for format_desc

    gst_caps_unref(tmp_caps);

    if (width < 0 || height < 0 || framerate < 0 || !best_caps) {
        GST_ERROR_OBJECT(self, "Unable to negotiate common caps\n");
        return FALSE;
    }

    int res = uvc_get_stream_ctrl_format_size(self->uvc_devh, &self->uvc_ctrl,
                                              UVC_FRAME_FORMAT_H264, width, height, framerate);
    if (res < 0) {
        GST_ERROR_OBJECT(self, "Unable to get stream control: %s", uvc_strerror(res));
        return FALSE;
    }

    self->frame_interval = (1000L * 1000L * 1000L) / framerate;

    gst_base_src_set_caps(basesrc, best_caps);

    GST_INFO_OBJECT(basesrc, "Negotiated caps: %" GST_PTR_FORMAT, best_caps);

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

  GST_DEBUG_OBJECT(self, "Starting libuvc source");

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

  // Start control socket thread
  self->control_running = TRUE;
  self->control_thread = g_thread_new("uvc-control", 
                                     gst_libuvc_h264_src_control_thread, 
                                     self);

  load_spspps(self);

  GST_DEBUG_OBJECT(self, "Libuvc source started successfully");
  return TRUE;
}

// Enhanced stop function with proper cleanup sequence
static gboolean gst_libuvc_h264_src_stop(GstBaseSrc *src) {
  GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(src);

  GST_DEBUG_OBJECT(self, "Stopping libuvc source");

  // Stop control thread first
  if (self->control_running) {
    GST_DEBUG_OBJECT(self, "Stopping control thread");
    self->control_running = FALSE;
    
    // Force accept to wake up by connecting to the socket
    int wakeup_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (wakeup_fd >= 0) {
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      strcpy(addr.sun_path, "/tmp/libuvc_control");
      connect(wakeup_fd, (struct sockaddr*)&addr, sizeof(addr));
      close(wakeup_fd);
    }
    
    if (self->control_thread) {
      g_thread_join(self->control_thread);
      self->control_thread = NULL;
      GST_DEBUG_OBJECT(self, "Control thread stopped");
    }
  }

  // Close control socket
  if (self->control_socket >= 0) {
    GST_DEBUG_OBJECT(self, "Closing control socket");
    close(self->control_socket);
    self->control_socket = -1;
    // Remove socket file
    unlink("/tmp/libuvc_control");
  }

  // Stop streaming FIRST - this is crucial!
  if (self->streaming && self->uvc_devh) {
    GST_DEBUG_OBJECT(self, "Stopping UVC streaming");
    uvc_stop_streaming(self->uvc_devh);
    self->streaming = FALSE;
    
    // Small delay to ensure streaming is fully stopped
    usleep(100000); // 100ms
  }

  // Clear the frame queue to release any pending buffers
  if (self->frame_queue) {
    GST_DEBUG_OBJECT(self, "Clearing frame queue");
    GstBuffer *buffer;
    while ((buffer = g_async_queue_try_pop(self->frame_queue)) != NULL) {
      gst_buffer_unref(buffer);
    }
  }

  // Close UVC device handle
  if (self->uvc_devh) {
    GST_DEBUG_OBJECT(self, "Closing UVC device handle");
    uvc_close(self->uvc_devh);
    self->uvc_devh = NULL;
  }

  // Unreference UVC device
  if (self->uvc_dev) {
    GST_DEBUG_OBJECT(self, "Unreferencing UVC device");
    uvc_unref_device(self->uvc_dev);
    self->uvc_dev = NULL;
  }

  // Exit UVC context
  if (self->uvc_ctx) {
    GST_DEBUG_OBJECT(self, "Exiting UVC context");
    uvc_exit(self->uvc_ctx);
    self->uvc_ctx = NULL;
  }

  // Clear control mutex
  g_mutex_clear(&self->control_mutex);

  GST_DEBUG_OBJECT(self, "Libuvc source fully stopped");
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

    GstClockTime libuvc_ts = ((uint64_t)frame->capture_time_finished.tv_sec) * 1000L * 1000L * 1000L
                             + frame->capture_time_finished.tv_nsec;
    if (self->uvc_start_time == G_MAXUINT64) {
        self->uvc_start_time = libuvc_ts;
    }
    libuvc_ts -= self->uvc_start_time;

    for (int i = 0; i < c; i++) {
        nal_unit_t *unit = &units[i];
        GstBuffer *buffer = NULL;
        gsize buffer_offset = 0;

        switch (unit->type) {
            case 7:
                self->sps_length = unit->len;
                memcpy(self->sps, unit->ptr, self->sps_length);
                updated_sps_pps = TRUE;
                self->send_sps_pps = TRUE;
                // deliberately not sending SPS/PPS info in their own buffer
                continue;
            case 8:
                self->pps_length = unit->len;
                memcpy(self->pps, unit->ptr, self->pps_length);
                updated_sps_pps = TRUE;
                self->send_sps_pps = TRUE;
                // deliberately not sending SPS/PPS info in their own buffer
                continue;
            case 5: {
                if (!self->had_idr || self->send_sps_pps) {
                    buffer_offset = self->sps_length + self->pps_length;
                    buffer = gst_buffer_new_allocate(NULL, buffer_offset + unit->len, NULL);
                    gst_buffer_fill(buffer, 0, self->sps, self->sps_length);
                    gst_buffer_fill(buffer, self->sps_length, self->pps, self->pps_length);
                    self->send_sps_pps = FALSE;
                }
                if (!self->had_idr) {
                    self->had_idr = TRUE;
                }
                break;
            }
            default:
                if (!self->had_idr) {
                    continue;
                }
        } // switch

        if (!buffer) {
          buffer = gst_buffer_new_allocate(NULL, unit->len, NULL);
        }
        gst_buffer_fill(buffer, buffer_offset, unit->ptr, unit->len);

        // Set timestamps on the buffer
        if (units[i].type == 1 || units[i].type == 5) {
            /* The problems:
               * libuvc capture timestamps are jittery
               * video players skip and duplicate frames if the PTSes are noisy
               * the actual framerate is never precisely equal to the nominal value,
                 and can drift over time
            */

            /* We skipped the initial non-IDR frames, so we need to add their
               duration to the output PTS when we get the first IDR frame */
            if (self->prev_pts == G_MAXUINT64) {
                self->prev_pts = libuvc_ts - self->frame_interval;
            }

            // Average frame interval tracking
            self->frame_count++;
            if (units[i].type == 5 && self->frame_count >= MIN_FRAMES_CALC_INTERVAL) {
                // Throw away the first set results as they can be quite noisy
                if (self->prev_int_ts != 0) {
                    #define AVG_DIV 20
                    #define AVG_MULT 1
                    #define AVG_ROUNDING (AVG_DIV/2)

                    uint64_t interval = (libuvc_ts - self->prev_int_ts) / self->frame_count;
                    self->frame_interval = (self->frame_interval * (AVG_DIV-AVG_MULT) +
                                                interval + AVG_ROUNDING) / AVG_DIV;
                }
                self->frame_count = 0;
                self->prev_int_ts = libuvc_ts;
            }

            GstClockTime timestamp = self->prev_pts + self->frame_interval;

            /* Determine if we need to slightly speed up or slow down the PTSes
               to track the average libuvc timestamps */
            /* Don't adjust the timestamps while we're reciving the first few
               frames as the timing can be quite noisy */
            if (self->prev_int_ts != 0) {
                int64_t diff = libuvc_ts - timestamp;
                int64_t adj = 0;
                // +/- 2-frame interval hysteresis
                if (diff < (-2 * self->frame_interval) || diff > (2 * self->frame_interval)) {
                    adj = diff / 5;
                    adj = CLAMP(diff, -self->frame_interval / 2, self->frame_interval / 2);
                }
                timestamp += adj;
            }

            GST_BUFFER_PTS(buffer) = timestamp;
            GST_BUFFER_DTS(buffer) = timestamp;
            GST_BUFFER_DURATION(buffer) = timestamp - self->prev_pts;

            self->prev_pts = timestamp;
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
	self->uvc_start_time = G_MAXUINT64;
	self->prev_pts = G_MAXUINT64;
  }

  // Retrieve a buffer from the queue
  *buf = g_async_queue_pop(self->frame_queue);
  if (*buf == NULL) {
    GST_ERROR_OBJECT(self, "No frame available.");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

// Enhanced finalize function
static void gst_libuvc_h264_src_finalize(GObject *object) {
    GstLibuvcH264Src *self = GST_LIBUVC_H264_SRC(object);

    GST_DEBUG_OBJECT(self, "Finalizing libuvc source");

    // Ensure everything is stopped (in case stop wasn't called)
    gst_libuvc_h264_src_stop(GST_BASE_SRC(self));

    // Free the index string
    if (self->index) {
        g_free(self->index);
        self->index = NULL;
    }

    // Unreference and free the frame queue
    if (self->frame_queue) {
        GST_DEBUG_OBJECT(self, "Unreffing frame queue");
        
        // Drain any remaining buffers
        GstBuffer *buffer;
        while ((buffer = g_async_queue_try_pop(self->frame_queue)) != NULL) {
            gst_buffer_unref(buffer);
        }
        
        g_async_queue_unref(self->frame_queue);
        self->frame_queue = NULL;
    }

    GST_DEBUG_OBJECT(self, "Libuvc source finalized");

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
