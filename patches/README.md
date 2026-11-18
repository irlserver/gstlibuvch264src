# libuvc patches

This directory contains patches that must be applied to libuvc before
building gstlibuvch264src. They are bundled here so a single libuvc
checkout (tested against `libuvc/libuvc` tag `v0.0.7`) can support both
the plugin's H265 codepaths and the IRL streaming scenarios that
UnlimitedIRL hit with stock libuvc.

Apply them in the order of their number prefix, which is the order a
shell glob (`patches/*.patch`) gives. `apply.sh` does that from inside a
libuvc tree, and the `Dockerfile` and the CMake build call it.

## 01-uvc15-support.patch

Sourced from
[UnlimitedIRL-Team/libuvch264src](https://github.com/UnlimitedIRL-Team/libuvch264src/blob/experimental/patches/uvc15-support.patch).

Two changes:

1. `uvc_wrap()` calls `libusb_set_auto_detach_kernel_driver(usb_devh, 1)`
   so libuvc can claim USB interfaces that are already bound to the
   `uvcvideo` kernel driver, instead of failing in `uvc_open_internal`.
2. `uvc_parse_vc_header()` accepts UVC 1.5 (`bcdUVC == 0x0150`) headers
   in addition to UVC 1.0/1.1, so newer devices are no longer rejected
   with `UVC_ERROR_NOT_SUPPORTED`.

## 02-libuvc-h265-support.patch

Adds `UVC_FRAME_FORMAT_H265` to `enum uvc_frame_format`, registers the
H265 fourcc GUID in the format table, includes H265 in the
`UVC_FRAME_FORMAT_COMPRESSED` group, and adds a `frame->step` case so
H265 frames flow through libuvc's pipeline. Required by the H265
codepaths ported from upstream BELABOX.

## 03-cve-2026-1991-scan-streaming-nullguard.patch

From CeraLive's gstlibuvcsrc (`77cb641`). `uvc_scan_streaming()` takes the
interface index from the device's VideoControl header without checking it,
so a malformed descriptor could read past `interface[]` or reach an
interface with no altsetting. The patch rejects both with
`UVC_ERROR_INVALID_DEVICE`. It also checks the result of
`get_device_descriptor()` in `uvc_scan_control()` before using the
descriptor. It must follow `01-uvc15-support.patch`, which changes the
surrounding lines in `src/device.c`.

## 04 to 09: stopping a stream on a dead device

From CeraLive's libuvc fork (`ab49e21`, `dfba86f`, `a1949ae`, `3ede00e`,
`2dcca59`, `71588db`, each with its commit message and a cherry-pick line).

- `04-stream-bounded-stop.patch`: `uvc_stream_stop()` waits for the
  cancelled transfers of a dead or unplugged device for at most 5 seconds,
  then returns `UVC_ERROR_TIMEOUT`, instead of hanging the caller.
- `05` to `08`: when that wait times out, transfers can still complete
  later and touch the stream handle, the device handle and the context.
  Closing then keeps those three allocated (a deliberate leak) instead of
  freeing memory a late callback still uses, and does not kill or join the
  handler thread under them.
- `09-init-safe-exit-iteration.patch`: `uvc_exit()` closes each open device
  while iterating over the list that closing removes it from. It now uses
  a safe iteration.

## Apply (host build)

```sh
git clone https://github.com/libuvc/libuvc.git
cd libuvc
git checkout v0.0.7
sh /path/to/gstlibuvch264src/patches/apply.sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
         -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF
make -j"$(nproc)"
sudo make install
sudo ldconfig
```
