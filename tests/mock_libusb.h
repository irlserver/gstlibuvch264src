/* Test-only mock of the small libusb-1.0 surface the libuvch264src element's
 * force_usb_release() touches (Task 11, H1/L8). Linked ONLY into the USB-teardown
 * test, which does NOT link the real libusb, so these definitions never collide
 * with it. It models a single device with a configurable interface count and
 * tracks handle opens/closes plus active-config queries, letting the test prove
 * teardown is balanced (one close per open) and that the interface count came
 * from the config descriptor, not a hardcoded 0..7 range.
 */

#ifndef MOCK_LIBUSB_H
#define MOCK_LIBUSB_H

#include <libusb-1.0/libusb.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate a fresh mock libusb handle. uvc_open() (under MOCK_LIBUSB_TEARDOWN)
 * calls this to model acquiring the device's libusb handle. */
struct libusb_device_handle *mock_libusb_alloc_handle(void);

/* Restore counters and the configured interface count to defaults. */
void mock_libusb_reset(void);

/* Interfaces the mocked active config descriptor advertises (default 2). */
void mock_libusb_set_num_interfaces(int n);

/* libusb_close() calls since reset - exactly one per opened handle when teardown
 * is correct; the old double-close made it two (and tripped the sanitizer). */
int mock_libusb_close_count(void);

/* mock_libusb_alloc_handle() calls since reset. */
int mock_libusb_open_count(void);

/* libusb_get_active_config_descriptor() calls since reset - proves the element
 * queried the real interface count (L8) instead of assuming a fixed range. */
int mock_libusb_config_query_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LIBUSB_H */
