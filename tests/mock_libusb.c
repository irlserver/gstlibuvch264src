/* Test-only libusb-1.0 mock; see mock_libusb.h. Implements exactly the functions
 * force_usb_release() calls (plus the libusb_close() the teardown mock's
 * uvc_close() invokes), backed by our own concrete handle/device structs - the
 * real libusb keeps these opaque, so completing them here is safe because this
 * TU is never linked alongside the real library. Used single-threaded by the
 * USB-teardown test under its RESOURCE_LOCK. */

#include <stdint.h>
#include <stdlib.h>

#include <libusb-1.0/libusb.h>
#include "mock_libusb.h"

struct libusb_device {
  uint8_t bus;
  uint8_t addr;
};

struct libusb_device_handle {
  struct libusb_device *dev;
};

static struct libusb_device g_dev = { .bus = 1, .addr = 2 };

static int g_num_interfaces = 2;
static int g_open_count = 0;
static int g_close_count = 0;
static int g_config_query_count = 0;

/* -------------------------------------------------------------------------- */
/* Control / observability API.                                               */
/* -------------------------------------------------------------------------- */

struct libusb_device_handle *mock_libusb_alloc_handle(void) {
  struct libusb_device_handle *h = calloc(1, sizeof(*h));
  if (h) {
    h->dev = &g_dev;
    g_open_count++;
  }
  return h;
}

void mock_libusb_reset(void) {
  g_num_interfaces = 2;
  g_open_count = 0;
  g_close_count = 0;
  g_config_query_count = 0;
}

void mock_libusb_set_num_interfaces(int n) { g_num_interfaces = n; }
int mock_libusb_close_count(void) { return g_close_count; }
int mock_libusb_open_count(void) { return g_open_count; }
int mock_libusb_config_query_count(void) { return g_config_query_count; }

/* -------------------------------------------------------------------------- */
/* libusb-1.0 surface referenced by force_usb_release() / uvc_close().        */
/* -------------------------------------------------------------------------- */

void libusb_close(libusb_device_handle *dev_handle) {
  /* free() of an already-freed handle is the double-close the sanitizer flags
   * when force_usb_release() closes the handle uvc_close() also owns. */
  g_close_count++;
  free(dev_handle);
}

libusb_device *libusb_get_device(libusb_device_handle *dev_handle) {
  return dev_handle ? dev_handle->dev : NULL;
}

uint8_t libusb_get_bus_number(libusb_device *dev) {
  return dev ? dev->bus : 0;
}

uint8_t libusb_get_device_address(libusb_device *dev) {
  return dev ? dev->addr : 0;
}

int libusb_get_active_config_descriptor(libusb_device *dev,
                                        struct libusb_config_descriptor **config) {
  (void)dev;
  if (!config)
    return LIBUSB_ERROR_INVALID_PARAM;
  struct libusb_config_descriptor *c = calloc(1, sizeof(*c));
  if (!c)
    return LIBUSB_ERROR_NO_MEM;
  c->bNumInterfaces = (uint8_t)g_num_interfaces;
  *config = c;
  g_config_query_count++;
  return LIBUSB_SUCCESS;
}

void libusb_free_config_descriptor(struct libusb_config_descriptor *config) {
  free(config);
}

int libusb_release_interface(libusb_device_handle *dev_handle,
                             int interface_number) {
  (void)dev_handle;
  return interface_number < g_num_interfaces ? LIBUSB_SUCCESS
                                             : LIBUSB_ERROR_NOT_FOUND;
}

int libusb_kernel_driver_active(libusb_device_handle *dev_handle,
                                int interface_number) {
  (void)dev_handle;
  (void)interface_number;
  return 0;
}

int libusb_detach_kernel_driver(libusb_device_handle *dev_handle,
                                int interface_number) {
  (void)dev_handle;
  (void)interface_number;
  return LIBUSB_SUCCESS;
}

/* Defined defensively so a failing-first revert to the old force_usb_release()
 * (which referenced libusb_reset_device under a legacy macro) still links. */
int libusb_reset_device(libusb_device_handle *dev_handle) {
  (void)dev_handle;
  return LIBUSB_SUCCESS;
}
