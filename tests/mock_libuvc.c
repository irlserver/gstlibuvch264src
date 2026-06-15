/* Test-only mock implementation of the libuvc API surface the libuvch264src
 * element actually calls. Linked INTO a mock-backed copy of the plugin (see
 * tests/CMakeLists.txt) in place of the real libuvc, so the element can be
 * driven end to end with no capture hardware.
 *
 * It implements exactly the symbols the element references and nothing else:
 *   uvc_init / uvc_exit / uvc_find_devices / uvc_open / uvc_close
 *   uvc_unref_device / uvc_get_format_descs / uvc_get_stream_ctrl_format_size
 *   uvc_start_streaming / uvc_stop_streaming / uvc_strerror
 *   uvc_get_libusb_handle   (returns NULL so force_usb_release() no-ops)
 *   PTZ: uvc_set/get_pantilt_abs, uvc_set/get_zoom_abs
 *
 * NOTE (Task 4 spike): real libuvc does not deliver a NULL frame on disconnect
 * in callback mode - it just stops invoking the callback. DISCONNECT mode mirrors
 * that by stopping the feeder, never by passing NULL. uvc_get_libusb_handle()
 * returns NULL by default so force_usb_release() short-circuits; the USB-teardown
 * test (MOCK_LIBUSB_TEARDOWN) returns a mock handle to exercise the release path.
 *
 * No USB protocol, bandwidth, or timing is simulated - just the assembled-frame
 * contract the element consumes.
 */

/* usleep() lives behind _DEFAULT_SOURCE; define it before any include so the TU
 * compiles under strict -std=c11 as well as the build's default gnu11. */
#define _DEFAULT_SOURCE

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libuvc/libuvc.h>
#include "mock_libuvc.h"

/* The USB-teardown test (Task 11) links a separate libusb mock so the element's
 * force_usb_release() actually exercises the libusb handle, and uvc_close()
 * models libuvc closing that same handle - making a double-close observable.
 * Every other target keeps the historic no-op (uvc_get_libusb_handle -> NULL). */
#ifdef MOCK_LIBUSB_TEARDOWN
#include <libusb-1.0/libusb.h>
#include "mock_libusb.h"
#endif

/* -------------------------------------------------------------------------- */
/* Opaque libuvc handles (real libuvc keeps these private; we define our own). */
/* -------------------------------------------------------------------------- */

#define MOCK_MAX_DEVICES 16
#define MOCK_MAX_LISTS 8
#define MOCK_FRAME_BUF_CAP 8192

struct uvc_context {
  uvc_device_t *devices[MOCK_MAX_DEVICES];
  int device_count;
  uvc_device_t **lists[MOCK_MAX_LISTS]; /* arrays handed to uvc_find_devices */
  int list_count;
};

struct uvc_device {
  uvc_context_t *ctx;
  int index;
  int refcount;
};

struct uvc_device_handle {
  uvc_device_t *dev;

  /* Format descriptor returned verbatim by uvc_get_format_descs(). */
  uvc_format_desc_t fmt_desc;
  uvc_frame_desc_t frame_desc;
  uint32_t intervals[2];

  /* Feeder thread state. */
  pthread_t feeder;
  pthread_mutex_t lock; /* guards running */
  int running;
  int started;
  uvc_frame_callback_t *cb;
  void *user_ptr;
  uint8_t *frame_buf;
  void *usb_handle; /* mock libusb handle; only set under MOCK_LIBUSB_TEARDOWN */
};

/* -------------------------------------------------------------------------- */
/* Global, injectable configuration (guarded by g_lock).                      */
/* -------------------------------------------------------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_device_count = 1;
static enum uvc_frame_format g_frame_format = UVC_FRAME_FORMAT_H264;
static mock_uvc_frame_mode_t g_frame_mode = MOCK_UVC_FRAME_VALID;
static mock_uvc_format_mode_t g_format_mode = MOCK_UVC_FORMAT_NORMAL;
static int g_max_frames = 0; /* 0 = until uvc_stop_streaming() */
static int g_frames_delivered = 0;
static int g_device_lists_outstanding = 0; /* uvc_find_devices() not yet freed */
static int g_uvc_open_count = 0;  /* successful uvc_open() calls */
static int g_uvc_close_count = 0; /* uvc_close() calls on a live handle */

static int32_t g_pan_min = -180000, g_pan_max = 180000, g_pan_cur = 0;
static int32_t g_tilt_min = -90000, g_tilt_max = 90000, g_tilt_cur = 0;
static uint16_t g_zoom_min = 0, g_zoom_max = 100, g_zoom_cur = 0;
static bool g_ptz_supported = true; /* false -> uvc_*_abs() return NOT_SUPPORTED */

/* Per-device USB descriptor, indexed by enumeration position; read back by the
 * element's selector matchers via uvc_get_device_descriptor()/bus_number/
 * device_address(). g_opened_device_index records which device uvc_open() took. */
static uint16_t g_dev_vid[MOCK_MAX_DEVICES];
static uint16_t g_dev_pid[MOCK_MAX_DEVICES];
static char g_dev_serial[MOCK_MAX_DEVICES][64];
static uint8_t g_dev_bus[MOCK_MAX_DEVICES];
static uint8_t g_dev_addr[MOCK_MAX_DEVICES];
static int g_opened_device_index = -1;

/* Apply MOCK_UVC_* environment overrides. Idempotent: only touches a field when
 * its variable is set, so it never clobbers a programmatic setter. Call with
 * g_lock held. */
static void apply_env_overrides_locked(void) {
  const char *s;
  if ((s = getenv("MOCK_UVC_DEVICE_COUNT")) != NULL)
    g_device_count = atoi(s);
  if ((s = getenv("MOCK_UVC_MAX_FRAMES")) != NULL)
    g_max_frames = atoi(s);
  if ((s = getenv("MOCK_UVC_FRAME_FORMAT")) != NULL)
    g_frame_format = (strcmp(s, "H265") == 0 || strcmp(s, "h265") == 0)
                         ? UVC_FRAME_FORMAT_H265
                         : UVC_FRAME_FORMAT_H264;
  if ((s = getenv("MOCK_UVC_FRAME_MODE")) != NULL) {
    if (strcmp(s, "oversized_sps") == 0)
      g_frame_mode = MOCK_UVC_FRAME_OVERSIZED_SPS;
    else if (strcmp(s, "disconnect") == 0)
      g_frame_mode = MOCK_UVC_FRAME_DISCONNECT;
    else if (strcmp(s, "nonidr_lead") == 0)
      g_frame_mode = MOCK_UVC_FRAME_NONIDR_LEAD;
    else
      g_frame_mode = MOCK_UVC_FRAME_VALID;
  }
}

/* -------------------------------------------------------------------------- */
/* Control API (mock_libuvc.h).                                               */
/* -------------------------------------------------------------------------- */

void mock_uvc_reset(void) {
  pthread_mutex_lock(&g_lock);
  g_device_count = 1;
  g_frame_format = UVC_FRAME_FORMAT_H264;
  g_frame_mode = MOCK_UVC_FRAME_VALID;
  g_format_mode = MOCK_UVC_FORMAT_NORMAL;
  g_max_frames = 0;
  g_frames_delivered = 0;
  g_uvc_open_count = 0;
  g_uvc_close_count = 0;
  g_pan_min = -180000; g_pan_max = 180000; g_pan_cur = 0;
  g_tilt_min = -90000; g_tilt_max = 90000; g_tilt_cur = 0;
  g_zoom_min = 0; g_zoom_max = 100; g_zoom_cur = 0;
  g_ptz_supported = true;
  memset(g_dev_vid, 0, sizeof(g_dev_vid));
  memset(g_dev_pid, 0, sizeof(g_dev_pid));
  memset(g_dev_serial, 0, sizeof(g_dev_serial));
  memset(g_dev_bus, 0, sizeof(g_dev_bus));
  memset(g_dev_addr, 0, sizeof(g_dev_addr));
  g_opened_device_index = -1;
  apply_env_overrides_locked();
  pthread_mutex_unlock(&g_lock);
}

void mock_uvc_set_device_count(int count) {
  pthread_mutex_lock(&g_lock);
  g_device_count = count;
  pthread_mutex_unlock(&g_lock);
}

void mock_uvc_set_device_descriptor(int idx, uint16_t vid, uint16_t pid,
                                    const char *serial, uint8_t bus,
                                    uint8_t addr) {
  if (idx < 0 || idx >= MOCK_MAX_DEVICES)
    return;
  pthread_mutex_lock(&g_lock);
  g_dev_vid[idx] = vid;
  g_dev_pid[idx] = pid;
  if (serial != NULL) {
    strncpy(g_dev_serial[idx], serial, sizeof(g_dev_serial[idx]) - 1);
    g_dev_serial[idx][sizeof(g_dev_serial[idx]) - 1] = '\0';
  } else {
    g_dev_serial[idx][0] = '\0';
  }
  g_dev_bus[idx] = bus;
  g_dev_addr[idx] = addr;
  pthread_mutex_unlock(&g_lock);
}

int mock_uvc_opened_device_index(void) {
  pthread_mutex_lock(&g_lock);
  int n = g_opened_device_index;
  pthread_mutex_unlock(&g_lock);
  return n;
}

void mock_uvc_set_frame_format(enum uvc_frame_format format) {
  pthread_mutex_lock(&g_lock);
  g_frame_format = format;
  pthread_mutex_unlock(&g_lock);
}

void mock_uvc_set_frame_mode(mock_uvc_frame_mode_t mode) {
  pthread_mutex_lock(&g_lock);
  g_frame_mode = mode;
  pthread_mutex_unlock(&g_lock);
}

void mock_uvc_set_format_mode(mock_uvc_format_mode_t mode) {
  pthread_mutex_lock(&g_lock);
  g_format_mode = mode;
  pthread_mutex_unlock(&g_lock);
}

void mock_uvc_set_max_frames(int max_frames) {
  pthread_mutex_lock(&g_lock);
  g_max_frames = max_frames;
  pthread_mutex_unlock(&g_lock);
}

void mock_uvc_set_ptz_range(int32_t pan_min, int32_t pan_max,
                            int32_t tilt_min, int32_t tilt_max,
                            uint16_t zoom_min, uint16_t zoom_max) {
  pthread_mutex_lock(&g_lock);
  g_pan_min = pan_min; g_pan_max = pan_max;
  g_tilt_min = tilt_min; g_tilt_max = tilt_max;
  g_zoom_min = zoom_min; g_zoom_max = zoom_max;
  pthread_mutex_unlock(&g_lock);
}

void mock_uvc_set_ptz_supported(bool supported) {
  pthread_mutex_lock(&g_lock);
  g_ptz_supported = supported;
  pthread_mutex_unlock(&g_lock);
}

void mock_uvc_get_last_pantilt(int32_t *pan, int32_t *tilt) {
  pthread_mutex_lock(&g_lock);
  if (pan) *pan = g_pan_cur;
  if (tilt) *tilt = g_tilt_cur;
  pthread_mutex_unlock(&g_lock);
}

uint16_t mock_uvc_get_last_zoom(void) {
  pthread_mutex_lock(&g_lock);
  uint16_t z = g_zoom_cur;
  pthread_mutex_unlock(&g_lock);
  return z;
}

int mock_uvc_frames_delivered(void) {
  pthread_mutex_lock(&g_lock);
  int n = g_frames_delivered;
  pthread_mutex_unlock(&g_lock);
  return n;
}

int mock_uvc_open_count(void) {
  pthread_mutex_lock(&g_lock);
  int n = g_uvc_open_count;
  pthread_mutex_unlock(&g_lock);
  return n;
}

int mock_uvc_close_count(void) {
  pthread_mutex_lock(&g_lock);
  int n = g_uvc_close_count;
  pthread_mutex_unlock(&g_lock);
  return n;
}

int mock_uvc_device_lists_outstanding(void) {
  pthread_mutex_lock(&g_lock);
  int n = g_device_lists_outstanding;
  pthread_mutex_unlock(&g_lock);
  return n;
}

/* -------------------------------------------------------------------------- */
/* NAL crafting.                                                              */
/* -------------------------------------------------------------------------- */

/* Append a single NAL: 4-byte Annex-B start code, header byte(s), then payload
 * of payload_len bytes (0x00 filled). Returns bytes written. */
static size_t append_nal_h264(uint8_t *p, uint8_t nal_type, size_t payload_len) {
  size_t n = 0;
  p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x01;
  p[n++] = (uint8_t)(0x60 | (nal_type & 0x1F)); /* nal_ref_idc=3, type */
  memset(p + n, 0x00, payload_len);
  n += payload_len;
  return n;
}

static size_t append_nal_h265(uint8_t *p, uint8_t nal_type, size_t payload_len) {
  size_t n = 0;
  p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x01;
  p[n++] = (uint8_t)((nal_type & 0x3F) << 1); /* 2-byte NAL header */
  p[n++] = 0x01;                              /* layer_id=0, tid=1   */
  memset(p + n, 0x00, payload_len);
  n += payload_len;
  return n;
}

/* Number of bare non-IDR slices MOCK_UVC_FRAME_NONIDR_LEAD emits before the
 * first real IDR access unit (models a stream joined mid-GOP). */
#define MOCK_NONIDR_LEAD_COUNT 5

/* Build one access unit into buf (capacity MOCK_FRAME_BUF_CAP). Returns length.
 * H264: SPS(7) + PPS(8) + IDR(5). H265: VPS(32) + SPS(33) + PPS(34) + IDR(20).
 * In NONIDR_LEAD mode the first MOCK_NONIDR_LEAD_COUNT frames are a single bare
 * non-IDR slice (type 1) with no SPS/PPS/IDR, so the element must drop them. */
static size_t craft_access_unit(uint8_t *buf, enum uvc_frame_format fmt,
                                mock_uvc_frame_mode_t mode, int frame_index) {
  size_t n = 0;

  if (mode == MOCK_UVC_FRAME_NONIDR_LEAD && frame_index < MOCK_NONIDR_LEAD_COUNT) {
    if (fmt == UVC_FRAME_FORMAT_H265)
      return append_nal_h265(buf, 1, 48); /* TRAIL non-IDR slice */
    return append_nal_h264(buf, 1, 48);   /* non-IDR slice */
  }
  /* OVERSIZED_SPS overflows the element's fixed 1024 B SPS buffer. The payload
   * must exceed the whole SPS+PPS+control tail of the instance struct so an
   * unclamped copy runs off the END of the GObject allocation (where ASan's
   * redzone lives), not merely into the adjacent pps[] field - an intra-object
   * spill ASan cannot see. 4096 clears that >2 KB tail with margin. */
  size_t sps_payload = (mode == MOCK_UVC_FRAME_OVERSIZED_SPS) ? 4096 : 12;

  if (fmt == UVC_FRAME_FORMAT_H265) {
    n += append_nal_h265(buf + n, 32, 8);            /* VPS */
    n += append_nal_h265(buf + n, 33, sps_payload);  /* SPS */
    n += append_nal_h265(buf + n, 34, 8);            /* PPS */
    n += append_nal_h265(buf + n, 20, 48);           /* IDR_W_RADL */
  } else {
    n += append_nal_h264(buf + n, 7, sps_payload);   /* SPS */
    n += append_nal_h264(buf + n, 8, 4);             /* PPS */
    n += append_nal_h264(buf + n, 5, 48);            /* IDR slice */
  }
  return n;
}

/* -------------------------------------------------------------------------- */
/* Feeder thread.                                                             */
/* -------------------------------------------------------------------------- */

static void *feeder_main(void *arg) {
  uvc_device_handle_t *h = arg;

  pthread_mutex_lock(&g_lock);
  enum uvc_frame_format fmt = g_frame_format;
  mock_uvc_frame_mode_t mode = g_frame_mode;
  int max_frames = g_max_frames;
  pthread_mutex_unlock(&g_lock);

  for (;;) {
    pthread_mutex_lock(&h->lock);
    int run = h->running;
    pthread_mutex_unlock(&h->lock);
    if (!run)
      break;

    pthread_mutex_lock(&g_lock);
    int delivered = g_frames_delivered;
    pthread_mutex_unlock(&g_lock);

    /* DISCONNECT: deliver up to the silence point, then go quiet (no NULL). */
    if (mode == MOCK_UVC_FRAME_DISCONNECT &&
        delivered >= (max_frames > 0 ? max_frames : 1)) {
      break;
    }

    size_t len = craft_access_unit(h->frame_buf, fmt, mode, delivered);

    uvc_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.data = h->frame_buf;
    frame.data_bytes = len;
    frame.frame_format = fmt;
    frame.width = 1920;
    frame.height = 1080;
    frame.source = h;
    frame.library_owns_data = 1;

    h->cb(&frame, h->user_ptr);

    pthread_mutex_lock(&g_lock);
    g_frames_delivered++;
    delivered = g_frames_delivered;
    pthread_mutex_unlock(&g_lock);

    if (mode != MOCK_UVC_FRAME_DISCONNECT && max_frames > 0 &&
        delivered >= max_frames) {
      break;
    }

    usleep(2000); /* ~500 fps ceiling; consumption paces the real rate */
  }
  return NULL;
}

/* -------------------------------------------------------------------------- */
/* libuvc API surface used by the element.                                    */
/* -------------------------------------------------------------------------- */

static uvc_device_t *mock_new_device(uvc_context_t *ctx, int index) {
  uvc_device_t *dev = calloc(1, sizeof(*dev));
  dev->ctx = ctx;
  dev->index = index;
  dev->refcount = 1;
  if (ctx->device_count < MOCK_MAX_DEVICES)
    ctx->devices[ctx->device_count++] = dev;
  return dev;
}

uvc_error_t uvc_init(uvc_context_t **ctx, struct libusb_context *usb_ctx) {
  (void)usb_ctx;
  pthread_mutex_lock(&g_lock);
  apply_env_overrides_locked();
  pthread_mutex_unlock(&g_lock);

  uvc_context_t *c = calloc(1, sizeof(*c));
  if (!c)
    return UVC_ERROR_NO_MEM;
  *ctx = c;
  return UVC_SUCCESS;
}

void uvc_exit(uvc_context_t *ctx) {
  if (!ctx)
    return;
  /* Device-list arrays are released by uvc_free_device_list(); the context owns
   * only the device objects. */
  for (int i = 0; i < ctx->device_count; i++)
    free(ctx->devices[i]);
  free(ctx);
}

uvc_error_t uvc_find_devices(uvc_context_t *ctx, uvc_device_t ***devs,
                             int vid, int pid, const char *sn) {
  (void)vid; (void)pid; (void)sn;
  if (!ctx)
    return UVC_ERROR_INVALID_PARAM;

  pthread_mutex_lock(&g_lock);
  int n = g_device_count;
  pthread_mutex_unlock(&g_lock);

  if (n <= 0) {
    *devs = NULL;
    return UVC_ERROR_NO_DEVICE;
  }
  if (n > MOCK_MAX_DEVICES)
    n = MOCK_MAX_DEVICES;

  uvc_device_t **list = calloc((size_t)n + 1, sizeof(*list));
  if (!list)
    return UVC_ERROR_NO_MEM;
  for (int i = 0; i < n; i++)
    list[i] = mock_new_device(ctx, i);
  list[n] = NULL;

  /* The element owns this array and must release it via uvc_free_device_list();
   * track only an outstanding count so a test can prove every list is freed
   * exactly once (ref-before-free, no leak, no double free). */
  pthread_mutex_lock(&g_lock);
  g_device_lists_outstanding++;
  pthread_mutex_unlock(&g_lock);

  *devs = list;
  return UVC_SUCCESS;
}

void uvc_unref_device(uvc_device_t *dev) {
  if (dev && dev->refcount > 0)
    dev->refcount--;
  /* Storage is owned by the context and reclaimed in uvc_exit(). */
}

void uvc_ref_device(uvc_device_t *dev) {
  if (dev)
    dev->refcount++;
}

void uvc_free_device_list(uvc_device_t **list, uint8_t unref_devices) {
  if (!list)
    return;
  if (unref_devices) {
    for (int i = 0; list[i] != NULL; i++)
      uvc_unref_device(list[i]);
  }
  free(list);
  pthread_mutex_lock(&g_lock);
  if (g_device_lists_outstanding > 0)
    g_device_lists_outstanding--;
  pthread_mutex_unlock(&g_lock);
}

/* Models real libuvc: allocate a descriptor the caller releases with
 * uvc_free_device_descriptor(). serialNumber is a heap copy (NULL when the
 * device reports no serial), matching the lifetime the element's matcher frees. */
uvc_error_t uvc_get_device_descriptor(uvc_device_t *dev,
                                      uvc_device_descriptor_t **desc) {
  if (!dev || !desc)
    return UVC_ERROR_INVALID_PARAM;
  uvc_device_descriptor_t *d = calloc(1, sizeof(*d));
  if (!d)
    return UVC_ERROR_NO_MEM;
  pthread_mutex_lock(&g_lock);
  int idx = dev->index;
  if (idx >= 0 && idx < MOCK_MAX_DEVICES) {
    d->idVendor = g_dev_vid[idx];
    d->idProduct = g_dev_pid[idx];
    if (g_dev_serial[idx][0] != '\0')
      d->serialNumber = strdup(g_dev_serial[idx]);
  }
  pthread_mutex_unlock(&g_lock);
  *desc = d;
  return UVC_SUCCESS;
}

void uvc_free_device_descriptor(uvc_device_descriptor_t *desc) {
  if (!desc)
    return;
  free((void *)desc->serialNumber);
  free(desc);
}

uint8_t uvc_get_bus_number(uvc_device_t *dev) {
  if (!dev)
    return 0;
  pthread_mutex_lock(&g_lock);
  uint8_t b = (dev->index >= 0 && dev->index < MOCK_MAX_DEVICES)
                  ? g_dev_bus[dev->index] : 0;
  pthread_mutex_unlock(&g_lock);
  return b;
}

uint8_t uvc_get_device_address(uvc_device_t *dev) {
  if (!dev)
    return 0;
  pthread_mutex_lock(&g_lock);
  uint8_t a = (dev->index >= 0 && dev->index < MOCK_MAX_DEVICES)
                  ? g_dev_addr[dev->index] : 0;
  pthread_mutex_unlock(&g_lock);
  return a;
}

uvc_error_t uvc_open(uvc_device_t *dev, uvc_device_handle_t **devh) {
  if (!dev || !devh)
    return UVC_ERROR_INVALID_PARAM;
  /* A device unref'd to zero is freed by real libuvc; opening it then is a
   * use-after-free. Reject it so a missing ref-before-free fails loudly here. */
  if (dev->refcount <= 0)
    return UVC_ERROR_NO_DEVICE;

  uvc_device_handle_t *h = calloc(1, sizeof(*h));
  if (!h)
    return UVC_ERROR_NO_MEM;
  h->dev = dev;
  pthread_mutex_init(&h->lock, NULL);
  h->frame_buf = malloc(MOCK_FRAME_BUF_CAP);
  if (!h->frame_buf) {
    pthread_mutex_destroy(&h->lock);
    free(h);
    return UVC_ERROR_NO_MEM;
  }

  pthread_mutex_lock(&g_lock);
  enum uvc_frame_format fmt = g_frame_format;
  mock_uvc_format_mode_t format_mode = g_format_mode;
  pthread_mutex_unlock(&g_lock);

  /* One 1080p frame descriptor; its interval shape and fourcc vary by
   * format_mode so negotiate()'s edge cases can be exercised. */
  memset(&h->frame_desc, 0, sizeof(h->frame_desc));
  h->frame_desc.bDescriptorSubtype = UVC_VS_FRAME_FRAME_BASED;
  h->frame_desc.wWidth = 1920;
  h->frame_desc.wHeight = 1080;
  h->frame_desc.next = NULL;

  switch (format_mode) {
    case MOCK_UVC_FORMAT_ZERO_DEVICE_INTERVAL:
      /* No interval list; device min/max interval are zero (1e7 / 0 = SIGFPE). */
      h->frame_desc.intervals = NULL;
      h->frame_desc.dwMinFrameInterval = 0;
      h->frame_desc.dwMaxFrameInterval = 0;
      break;
    case MOCK_UVC_FORMAT_ZERO_FRAMERATE:
      /* One interval long enough that 1e7 / interval truncates to 0 fps. */
      h->intervals[0] = 20000000; /* 100ns units -> 0.5 fps -> 0 after int div */
      h->intervals[1] = 0;
      h->frame_desc.intervals = h->intervals;
      h->frame_desc.dwMinFrameInterval = 20000000;
      h->frame_desc.dwMaxFrameInterval = 20000000;
      break;
    default:
      h->intervals[0] = 333333; /* 100ns units -> 30 fps */
      h->intervals[1] = 0;
      h->frame_desc.intervals = h->intervals;
      h->frame_desc.dwMinFrameInterval = 333333;
      h->frame_desc.dwMaxFrameInterval = 333333;
      break;
  }

  memset(&h->fmt_desc, 0, sizeof(h->fmt_desc));
  if (format_mode == MOCK_UVC_FORMAT_NO_CODEC) {
    /* A format the element does not handle, so negotiate() finds no codec. */
    memcpy(h->fmt_desc.fourccFormat, "MJPG", 4);
  } else {
    memcpy(h->fmt_desc.fourccFormat,
           fmt == UVC_FRAME_FORMAT_H265 ? "H265" : "H264", 4);
  }
  h->fmt_desc.frame_descs = &h->frame_desc;
  h->fmt_desc.next = NULL;

#ifdef MOCK_LIBUSB_TEARDOWN
  h->usb_handle = mock_libusb_alloc_handle();
#endif

  pthread_mutex_lock(&g_lock);
  g_uvc_open_count++;
  g_opened_device_index = dev->index;
  pthread_mutex_unlock(&g_lock);

  *devh = h;
  return UVC_SUCCESS;
}

void uvc_close(uvc_device_handle_t *devh) {
  if (!devh)
    return;
  /* Ensure the feeder is stopped even if the element forgot to. */
  if (devh->started) {
    pthread_mutex_lock(&devh->lock);
    devh->running = 0;
    pthread_mutex_unlock(&devh->lock);
    pthread_join(devh->feeder, NULL);
    devh->started = 0;
  }

#ifdef MOCK_LIBUSB_TEARDOWN
  /* Model real libuvc: uvc_close() closes the underlying libusb handle. If
   * force_usb_release() already closed it, this is the double-close ASan catches. */
  if (devh->usb_handle) {
    libusb_close((struct libusb_device_handle *)devh->usb_handle);
    devh->usb_handle = NULL;
  }
#endif

  pthread_mutex_lock(&g_lock);
  g_uvc_close_count++;
  pthread_mutex_unlock(&g_lock);

  pthread_mutex_destroy(&devh->lock);
  free(devh->frame_buf);
  free(devh);
}

const uvc_format_desc_t *uvc_get_format_descs(uvc_device_handle_t *devh) {
  if (!devh)
    return NULL;
  return &devh->fmt_desc;
}

uvc_error_t uvc_get_stream_ctrl_format_size(uvc_device_handle_t *devh,
                                            uvc_stream_ctrl_t *ctrl,
                                            enum uvc_frame_format format,
                                            int width, int height, int fps) {
  (void)format; (void)width; (void)height; (void)fps;
  if (!devh || !ctrl)
    return UVC_ERROR_INVALID_PARAM;
  memset(ctrl, 0, sizeof(*ctrl));
  ctrl->bFormatIndex = 1;
  ctrl->bFrameIndex = 1;
  ctrl->dwFrameInterval = 333333;
  ctrl->dwMaxVideoFrameSize = MOCK_FRAME_BUF_CAP;
  return UVC_SUCCESS;
}

uvc_error_t uvc_start_streaming(uvc_device_handle_t *devh,
                                uvc_stream_ctrl_t *ctrl,
                                uvc_frame_callback_t *cb, void *user_ptr,
                                uint8_t flags) {
  (void)ctrl; (void)flags;
  if (!devh || !cb)
    return UVC_ERROR_INVALID_PARAM;

  pthread_mutex_lock(&g_lock);
  g_frames_delivered = 0;
  pthread_mutex_unlock(&g_lock);

  devh->cb = cb;
  devh->user_ptr = user_ptr;
  pthread_mutex_lock(&devh->lock);
  devh->running = 1;
  pthread_mutex_unlock(&devh->lock);

  if (pthread_create(&devh->feeder, NULL, feeder_main, devh) != 0) {
    pthread_mutex_lock(&devh->lock);
    devh->running = 0;
    pthread_mutex_unlock(&devh->lock);
    return UVC_ERROR_OTHER;
  }
  devh->started = 1;
  return UVC_SUCCESS;
}

void uvc_stop_streaming(uvc_device_handle_t *devh) {
  if (!devh || !devh->started)
    return;
  pthread_mutex_lock(&devh->lock);
  devh->running = 0;
  pthread_mutex_unlock(&devh->lock);
  pthread_join(devh->feeder, NULL);
  devh->started = 0;
}

/* The element passes the result straight to libusb. Returns NULL by default so
 * force_usb_release() short-circuits (no libusb in this build); the USB-teardown
 * test returns a mock handle instead so the real release path is exercised. */
struct libusb_device_handle *uvc_get_libusb_handle(uvc_device_handle_t *devh) {
#ifdef MOCK_LIBUSB_TEARDOWN
  return devh ? (struct libusb_device_handle *)devh->usb_handle : NULL;
#else
  (void)devh;
  return NULL;
#endif
}

const char *uvc_strerror(uvc_error_t err) {
  switch (err) {
    case UVC_SUCCESS: return "Success (no error)";
    case UVC_ERROR_IO: return "Input/output error";
    case UVC_ERROR_INVALID_PARAM: return "Invalid parameter";
    case UVC_ERROR_NO_DEVICE: return "No such device";
    case UVC_ERROR_NOT_FOUND: return "Entity not found";
    case UVC_ERROR_NO_MEM: return "Insufficient memory";
    case UVC_ERROR_NOT_SUPPORTED: return "Operation not supported";
    default: return "Unknown error (mock)";
  }
}

/* -------------------------------------------------------------------------- */
/* PTZ stubs.                                                                 */
/* -------------------------------------------------------------------------- */

static int32_t ptz_pick_i32(enum uvc_req_code req, int32_t cur, int32_t mn,
                            int32_t mx) {
  switch (req) {
    case UVC_GET_MIN: return mn;
    case UVC_GET_MAX: return mx;
    case UVC_GET_RES: return 1;
    default: return cur;
  }
}

static uint16_t ptz_pick_u16(enum uvc_req_code req, uint16_t cur, uint16_t mn,
                             uint16_t mx) {
  switch (req) {
    case UVC_GET_MIN: return mn;
    case UVC_GET_MAX: return mx;
    case UVC_GET_RES: return 1;
    default: return cur;
  }
}

uvc_error_t uvc_set_pantilt_abs(uvc_device_handle_t *devh, int32_t pan,
                                int32_t tilt) {
  if (!devh)
    return UVC_ERROR_NO_DEVICE;
  pthread_mutex_lock(&g_lock);
  if (!g_ptz_supported) {
    pthread_mutex_unlock(&g_lock);
    return UVC_ERROR_NOT_SUPPORTED;
  }
  g_pan_cur = pan;
  g_tilt_cur = tilt;
  pthread_mutex_unlock(&g_lock);
  return UVC_SUCCESS;
}

uvc_error_t uvc_get_pantilt_abs(uvc_device_handle_t *devh, int32_t *pan,
                                int32_t *tilt, enum uvc_req_code req_code) {
  if (!devh || !pan || !tilt)
    return UVC_ERROR_NO_DEVICE;
  pthread_mutex_lock(&g_lock);
  if (!g_ptz_supported) {
    pthread_mutex_unlock(&g_lock);
    return UVC_ERROR_NOT_SUPPORTED;
  }
  *pan = ptz_pick_i32(req_code, g_pan_cur, g_pan_min, g_pan_max);
  *tilt = ptz_pick_i32(req_code, g_tilt_cur, g_tilt_min, g_tilt_max);
  pthread_mutex_unlock(&g_lock);
  return UVC_SUCCESS;
}

uvc_error_t uvc_set_zoom_abs(uvc_device_handle_t *devh, uint16_t focal_length) {
  if (!devh)
    return UVC_ERROR_NO_DEVICE;
  pthread_mutex_lock(&g_lock);
  if (!g_ptz_supported) {
    pthread_mutex_unlock(&g_lock);
    return UVC_ERROR_NOT_SUPPORTED;
  }
  g_zoom_cur = focal_length;
  pthread_mutex_unlock(&g_lock);
  return UVC_SUCCESS;
}

uvc_error_t uvc_get_zoom_abs(uvc_device_handle_t *devh, uint16_t *focal_length,
                             enum uvc_req_code req_code) {
  if (!devh || !focal_length)
    return UVC_ERROR_NO_DEVICE;
  pthread_mutex_lock(&g_lock);
  if (!g_ptz_supported) {
    pthread_mutex_unlock(&g_lock);
    return UVC_ERROR_NOT_SUPPORTED;
  }
  *focal_length = ptz_pick_u16(req_code, g_zoom_cur, g_zoom_min, g_zoom_max);
  pthread_mutex_unlock(&g_lock);
  return UVC_SUCCESS;
}
