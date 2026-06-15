# gstlibuvch264src

GStreamer source element for UVC H.264 (and H.265) capture devices, such as DJI action cameras and compatible USB UVC hardware. It feeds a raw H.264/H.265 bitstream into a downstream encode or streaming pipeline. HDMI capture paths bypass this element entirely.

---

## Example Pipelines

**Display on HDMI output (Rockchip, kernel 6.6):**
```
gst-launch-1.0 libuvch264src index=0 \
  ! video/x-h264,width=1920,height=1080,framerate=30/1 \
  ! queue ! h264parse ! queue ! v4l2slh264dec ! queue ! videoconvert ! kmssink
```

**Select device by USB serial number:**
```
gst-launch-1.0 libuvch264src index="serial:CAM-001" \
  ! video/x-h264,width=1920,height=1080,framerate=30/1 \
  ! queue ! h264parse ! fakesink
```

**Select device by vendor:product ID (hex):**
```
gst-launch-1.0 libuvch264src index="1234:5678" \
  ! video/x-h264 ! fakesink
```

**Pan/tilt/zoom (capability-gated, silently ignored if the device does not support it):**
```
gst-launch-1.0 libuvch264src index=0 pan=18000 tilt=0 zoom=100 \
  ! video/x-h264 ! fakesink
```

**Enable the opt-in PTZ control socket:**
```
gst-launch-1.0 libuvch264src index=0 control-socket=true \
  ! video/x-h264 ! fakesink
# Read the resolved socket path back via g_object_get("control-socket-path")
```

Rockchip decoder: kernel 5.10 uses `mppvideodec`, kernel 6.6 uses `v4l2slh264dec`.

---

## Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `index` | string | `"0"` | Device selector: ordinal `"0"`, `"vid:pid"` (hex), `"serial:<sn>"`, or `"bus:<bus>:<addr>"` |
| `pan` | int | `0` | Absolute pan in UVC arcseconds (±648000), capability-gated |
| `tilt` | int | `0` | Absolute tilt in UVC arcseconds (±648000), capability-gated |
| `zoom` | int | `0` | Absolute zoom as UVC focal length (0..65535), capability-gated |
| `control-socket` | bool | `false` | Enable the opt-in Unix-domain PTZ control socket (default off) |
| `control-socket-path` | string | `null` | Explicit socket path. Auto-selects `$XDG_RUNTIME_DIR/libuvch264src-<pid>-<seq>.sock` when null |

Action signal: `set-ptz(pan, tilt, zoom)` drives all three axes in one call and returns `TRUE` if at least one supported axis succeeded.

### Control socket protocol

When `control-socket=true`, the element listens on a Unix-domain socket and accepts one command per line:

| Command | Arguments | Effect |
|---------|-----------|--------|
| `PAN_TILT <pan> <tilt>` | normalized floats in `[-1.0, 1.0]` | Maps each axis onto the device's probed UVC range and sets the absolute position |
| `ZOOM <zoom>` | normalized float in `[-1.0, 1.0]` | Maps onto the device's probed zoom range |
| `GET_POSITION` | (none) | Replies with the current `pan`/`tilt`/`zoom` |
| `GET_CAPABILITIES` | (none) | Replies with the supported axes and their absolute ranges |

The socket carries normalized rates so callers stay decoupled from per-device UVC units. The `pan`/`tilt`/`zoom` GObject properties take absolute UVC units instead, for direct programmatic control.

---

## Build Steps

```bash
sudo apt install build-essential cmake git meson pkg-config
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
sudo apt install libusb-1.0-0 libusb-1.0-0-dev

# 1. Clone libuvc at the pinned SHA
git init libuvc && cd libuvc
git remote add origin https://github.com/libuvc/libuvc.git
git fetch --depth 1 origin 68d07a00e11d1944e27b7295ee69673239c00b4b
git checkout FETCH_HEAD
cd ..

# 2. Build and install libuvc
cd libuvc && cmake . && make && sudo make install && cd ..

# 3. Build the plugin
meson setup build libuvch264src/
cd build && meson compile && meson install

# 4. Move .so to the system GStreamer path (multiarch-aware)
MULTIARCH=$(gcc -print-multiarch)
sudo mv /usr/local/lib/${MULTIARCH}/gstreamer-1.0/libgstlibuvch264src.so \
        /lib/${MULTIARCH}/gstreamer-1.0/
sudo cp /usr/local/lib/libuvc.* /usr/lib/${MULTIARCH}/
```

`$(gcc -print-multiarch)` resolves to `aarch64-linux-gnu` on arm64 and `x86_64-linux-gnu` on amd64. Do not hardcode the arch string.

---

## Running Tests

The test suite is hardware-independent. It uses a libuvc mock and does not require a UVC device.

```bash
# With sanitizers (recommended)
cmake -B build -DENABLE_SANITIZERS=ON && cmake --build build && ctest --test-dir build --output-on-failure

# Without sanitizers (faster)
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

---

## Credits

Originally from [BELABOX](https://github.com/BELABOX/gstlibuvch264src). The 2.0 hardening pass (module split, mock-backed test suite, memory-safety and correctness fixes, PTZ properties, device selection, hardened control socket) was contributed by [CeraLive](https://github.com/CERALIVE/gstlibuvch264src).
