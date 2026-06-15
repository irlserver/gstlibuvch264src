/* Test-only mock of the libuvc API surface the libuvch264src element calls.
 *
 * This header declares ONLY the control knobs a functional test uses to inject
 * behavior into the mock. The libuvc functions themselves (uvc_init, uvc_open,
 * uvc_start_streaming, ...) keep their real prototypes from <libuvc/libuvc.h>;
 * the mock provides definitions for exactly the handful the element references
 * and nothing else (see mock_libuvc.c).
 *
 * Two equivalent ways to drive the mock:
 *   1. Programmatically, from a test linked against the mock object, via the
 *      mock_uvc_* setters below.
 *   2. Via environment variables, for a test that only dlopen()s the
 *      mock-backed plugin and never links the mock directly:
 *        MOCK_UVC_DEVICE_COUNT   integer, devices uvc_find_devices() exposes (default 1)
 *        MOCK_UVC_MAX_FRAMES     integer, frames the feeder delivers then stops (default 0 = until stop)
 *        MOCK_UVC_FRAME_FORMAT   "H264" | "H265" (default H264)
 *        MOCK_UVC_FRAME_MODE     "valid" | "oversized_sps" | "disconnect" (default valid)
 *   Environment variables, when present, win over the programmatic setters.
 */

#ifndef MOCK_LIBUVC_H
#define MOCK_LIBUVC_H

#include <stdbool.h>
#include <stdint.h>
#include <libuvc/libuvc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Behavior the frame feeder injects into the element's frame_callback. */
typedef enum {
  /* Well-formed access units: SPS + PPS + IDR with 4-byte start codes. */
  MOCK_UVC_FRAME_VALID = 0,
  /* An SPS NAL larger than SPSPPSBUFSZ (1024 B) to exercise the element's
   * fixed-size SPS/PPS copy buffers. For overflow-detection tests only. */
  MOCK_UVC_FRAME_OVERSIZED_SPS,
  /* Simulated cable pull: the feeder stops delivering frames and goes silent.
   * Real libuvc does NOT pass a NULL frame on disconnect in callback mode -
   * the callback simply stops being invoked (spike verdict, Task 4). */
  MOCK_UVC_FRAME_DISCONNECT,
  /* Mid-GOP join: the feeder leads each stream with a few bare non-IDR slices
   * (no SPS/PPS/IDR) before the first IDR access unit. Lets a test prove the
   * element drops them until a fresh IDR, including across a stop/start cycle. */
  MOCK_UVC_FRAME_NONIDR_LEAD,
} mock_uvc_frame_mode_t;

/* Shape of the single format/frame descriptor uvc_get_format_descs() advertises,
 * used to exercise the element's negotiate() edge cases. */
typedef enum {
  /* H264/H265 format, 1080p, one 30 fps interval (the default). */
  MOCK_UVC_FORMAT_NORMAL = 0,
  /* A non-codec format (fourcc "MJPG"): negotiate() must find no H264/H265
   * descriptor and post a bus error instead of streaming. */
  MOCK_UVC_FORMAT_NO_CODEC,
  /* No interval list and dwMin/MaxFrameInterval == 0: the device-interval
   * branch of negotiate() must not divide by zero. */
  MOCK_UVC_FORMAT_ZERO_DEVICE_INTERVAL,
  /* An interval so long it rounds to 0 fps: the frame_interval computation in
   * negotiate() must not divide by zero. */
  MOCK_UVC_FORMAT_ZERO_FRAMERATE,
} mock_uvc_format_mode_t;

/* Restore every mock knob to its default (1 device, H264, valid frames,
 * unlimited feed, nominal PTZ ranges, NORMAL format descriptor). Also re-reads
 * environment overrides. */
void mock_uvc_reset(void);

/* Shape of the advertised format/frame descriptor; see mock_uvc_format_mode_t. */
void mock_uvc_set_format_mode(mock_uvc_format_mode_t mode);

/* Number of devices the next uvc_find_devices()/uvc_open() will expose.
 * 0 makes uvc_find_devices() report UVC_ERROR_NO_DEVICE. */
void mock_uvc_set_device_count(int count);

/* Per-device USB descriptor the selector matchers read: vid/pid and serial via
 * uvc_get_device_descriptor(), bus/addr via uvc_get_bus_number()/_address().
 * idx is the device's position in the enumerated list (0-based). A NULL or empty
 * serial makes uvc_get_device_descriptor() report serialNumber == NULL. Reset
 * clears every slot to zero/none. */
void mock_uvc_set_device_descriptor(int idx, uint16_t vid, uint16_t pid,
                                    const char *serial, uint8_t bus,
                                    uint8_t addr);

/* Enumeration index of the device the last successful uvc_open() selected, or
 * -1 if none has been opened since mock_uvc_reset(). Lets a selection test prove
 * which device a given index selector resolved to. */
int mock_uvc_opened_device_index(void);

/* Pixel format the feeder crafts and uvc_get_format_descs() advertises. */
void mock_uvc_set_frame_format(enum uvc_frame_format format);

/* Frame feeder behavior; see mock_uvc_frame_mode_t. */
void mock_uvc_set_frame_mode(mock_uvc_frame_mode_t mode);

/* Stop the feeder after delivering this many frames (0 = run until
 * uvc_stop_streaming()). DISCONNECT mode treats this as the silence point. */
void mock_uvc_set_max_frames(int max_frames);

/* Stubbed PTZ ranges returned by the uvc_get_*_abs() MIN/MAX requests. */
void mock_uvc_set_ptz_range(int32_t pan_min, int32_t pan_max,
                            int32_t tilt_min, int32_t tilt_max,
                            uint16_t zoom_min, uint16_t zoom_max);

/* Whether the device exposes PTZ controls (default true). When false every
 * uvc_*_abs() get and set returns UVC_ERROR_NOT_SUPPORTED, emulating a camera
 * with no PanTilt/Zoom unit so the element's probe gates every axis off. */
void mock_uvc_set_ptz_supported(bool supported);

/* Last pan/tilt and zoom written via uvc_set_pantilt_abs()/uvc_set_zoom_abs()
 * (observability for assertions that a property actually drove the device). */
void mock_uvc_get_last_pantilt(int32_t *pan, int32_t *tilt);
uint16_t mock_uvc_get_last_zoom(void);

/* Frames the feeder has delivered since the last uvc_start_streaming()
 * (observability for assertions). */
int mock_uvc_frames_delivered(void);

/* uvc_open()/uvc_close() call counts since the last mock_uvc_reset(). A correct
 * teardown closes exactly once per successful open, so across N start/stop
 * cycles both counters reach N. */
int mock_uvc_open_count(void);
int mock_uvc_close_count(void);

/* Device-list arrays handed out by uvc_find_devices() that have not yet been
 * released with uvc_free_device_list(). A correct caller leaves this at its
 * starting value; a leak makes it grow. */
int mock_uvc_device_lists_outstanding(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LIBUVC_H */
