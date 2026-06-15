#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <libusb-1.0/libusb.h>
#include "gstlibuvch264src_internal.h"
#include "uvc_device.h"

// Cheap, read-only V4L2 capability probe. Opens /dev/video<N>, issues exactly
// one VIDIOC_TRY_FMT with V4L2_PIX_FMT_H264, logs the result, and closes.
// Non-fatal: if the node is absent or the ioctl fails the element continues.
void gst_libuvc_h264_src_v4l2_probe(GstElement *element, int device_index) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/video%d", device_index);

    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        GST_INFO_OBJECT(element, "V4L2 probe unavailable: cannot open %s", path);
        return;
    }

    struct v4l2_format fmt = { 0 };
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = 1920;
    fmt.fmt.pix.height      = 1080;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    int ret = ioctl(fd, VIDIOC_TRY_FMT, &fmt);
    close(fd);

    // sizeimage > 0 means the kernel accepted the H.264 format.
    gboolean available = (ret == 0 && fmt.fmt.pix.sizeimage > 0 &&
                          fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_H264);
    GST_INFO_OBJECT(element, "V4L2 native H.264: %s",
                    available ? "available" : "unavailable");
}

// Release the USB interfaces claimed for the open device so a subsequent
// uvc_close() (and any later re-open) starts from a clean slate.
//
// This function MUST NOT close or reset the libusb handle: the handle is owned
// by uvc_devh, and uvc_close() in stop() closes it exactly once. The previous
// code called libusb_close() here and then stop() called uvc_close() on the
// same (now freed) handle - a double-free/use-after-free. The post-close
// libusb_reset_device() compounded it by touching the freed handle. Teardown is
// uvc_close()'s job; we only drop interface claims while the handle is still open.
void gst_libuvc_h264_src_force_usb_release(GstLibuvcH264Src *self) {
    GST_DEBUG_OBJECT(self, "Releasing USB interfaces");

    if (!self->uvc_devh) return;

    // Get the underlying libusb handle (kept OPEN - see note above).
    struct libusb_device_handle *usb_devh = uvc_get_libusb_handle(self->uvc_devh);
    if (!usb_devh) {
        GST_WARNING_OBJECT(self, "Cannot get libusb handle from uvc");
        return;
    }

    struct libusb_device *usb_dev = libusb_get_device(usb_devh);
    if (!usb_dev) {
        GST_WARNING_OBJECT(self, "Cannot get libusb device");
        return;
    }

    int bus = libusb_get_bus_number(usb_dev);
    int addr = libusb_get_device_address(usb_dev);
    GST_INFO_OBJECT(self, "USB device at bus %d, address %d", bus, addr);

    // Query the real interface count from the active configuration instead of
    // guessing a fixed 0..7 range (L8): a device may expose fewer or, in
    // principle, more than eight interfaces.
    int num_interfaces = 0;
    struct libusb_config_descriptor *config = NULL;
    if (libusb_get_active_config_descriptor(usb_dev, &config) == LIBUSB_SUCCESS && config) {
        num_interfaces = config->bNumInterfaces;
    } else {
        GST_WARNING_OBJECT(self, "Cannot read active config descriptor; skipping interface release");
    }

    for (int interface = 0; interface < num_interfaces; interface++) {
        int ret = libusb_release_interface(usb_devh, interface);
        if (ret == LIBUSB_SUCCESS) {
            GST_DEBUG_OBJECT(self, "Released interface %d", interface);
        }
    }

    // Reattach detached interfaces to the kernel where supported, so the device
    // returns to a usable state for other consumers after we let go.
    #ifdef LIBUSB_OPTION_DETACH_KERNEL_DRIVER
    for (int interface = 0; interface < num_interfaces; interface++) {
        if (libusb_kernel_driver_active(usb_devh, interface) == 1) {
            GST_DEBUG_OBJECT(self, "Detaching kernel driver from interface %d", interface);
            libusb_detach_kernel_driver(usb_devh, interface);
        }
    }
    #endif

    if (config) {
        libusb_free_config_descriptor(config);
    }

    // Handle intentionally left OPEN: uvc_close() owns closing it.
}
