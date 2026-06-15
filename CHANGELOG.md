# CHANGELOG for gstlibuvch264src

## 2.0, Hardening release

The bulk of this release (module split, mock-backed test suite, memory-safety and correctness fixes, PTZ properties, device selection, hardened control socket) was contributed by [CeraLive](https://github.com/CERALIVE/gstlibuvch264src) and merged here.

### What

A comprehensive hardening pass across the entire element. The monolithic source was split into cohesive modules, a hardware-independent mock-backed test suite was built from scratch, and 19 distinct correctness and security issues were fixed. New capabilities: hardware-stable device selection, native PTZ properties, an opt-in hardened control socket, and a V4L2 capability probe.

### Why

The original element had several latent memory-safety issues (heap overflow on oversized SPS/PPS NALs, USB handle double-close, caps negotiation leaks), correctness bugs (PTS monotonicity, restart state not reset, NAL start-code detection missing the 3-byte form), and security concerns (world-accessible control socket at a fixed `/tmp` path, no index validation). None of these were caught by the existing smoke test because it only checked plugin registration. No frames flowed, no device was opened, no PTZ was exercised.

The hardening pass adds a full mock-backed ctest suite (TSan, ASAN, LSAN variants) that exercises all of these paths without requiring hardware.

### Changes

**Module split (behavior-preserving)**
The ~1100-line monolithic `gstlibuvch264src.c` was split into five cohesive translation units: `gstlibuvch264src.c` (GObject boilerplate), `frame_pipeline.c` (NAL parsing and PTS), `spspps_cache.c` (SPS/PPS disk cache), `ptz_control.c` (PTZ and control socket), `uvc_device.c` (USB teardown and V4L2 probe). No behavior change.

**Test infrastructure**
A libuvc mock harness (`mock_libuvc.c`) covers ~16 libuvc functions. A libusb mock (`mock_libusb.c`) enables teardown double-close testing. TSan and ASAN ctest variants run the full suite under sanitizers. The Dockerfile and CMakeLists.txt now pin the base image (`ubuntu:24.04@sha256:786a8b55...`) and libuvc source (`68d07a00`, v0.0.7) by SHA. The arch matrix fails loudly on unknown `TARGETARCH` values.

**Security and correctness fixes**
- Heap overflow: SPS/PPS/VPS NAL copies now clamp to buffer size before `memcpy`
- USB teardown UAF/double-close: `force_usb_release()` no longer calls `libusb_close()`, so `uvc_close()` owns the single close
- Caps negotiation leaks: single `goto out` cleanup path, `GValue` (GST_TYPE_LIST) unset after `gst_structure_set_value()`
- PTS monotonicity: clamp plus offset bound guard, first-frame underflow guard
- Restart state: `had_idr`, `send_sps_pps`, `frame_count`, `prev_int_ts` reset on every `start()`
- NAL parser: detects both 3-byte and 4-byte Annex-B start codes, dynamic unit array (no 10-unit cap), `size_t` lengths throughout
- Device-list leak: `uvc_ref_device()` before `uvc_free_device_list()`, fatal error on zero devices
- Index validation: strict `strtol` replaces silent `atoi()`, malformed index posts `RESOURCE/SETTINGS`
- Mutex lifecycle: `control_mutex` cleared once in `finalize()`, not in `stop()`
- `unlock()`/`unlock_stop()`: implemented with `FLUSH_SENTINEL` plus `g_async_queue_timeout_pop`, so `create()` never deadlocks on disconnect
- Framerate guard: `framerate <= 0` check prevents SIGFPE, zero device interval skipped
- PTS hot-path locking: clock ref plus PTS state under `GST_OBJECT_LOCK` in `frame_callback`
- SPS/PPS cache: path traversal blocked, NULL guards, resolution key (`<idx>_<codec>_<WxH>`)
- Error mapping: `uvc_error_t` to `GST_ELEMENT_ERROR` helper covers all libuvc error codes
- Interface count: real `bNumInterfaces` from `libusb_get_active_config_descriptor` replaces hardcoded 8
- SPS/PPS write-on-change: disk write suppressed when content is unchanged

**New capabilities**
- Device selection: `index` now accepts `"vid:pid"` (hex), `"serial:<sn>"`, `"bus:<bus>:<addr>"` in addition to the existing ordinal form. Backward-compatible, default `"0"` unchanged.
- Native PTZ properties: `pan`, `tilt`, `zoom` (GObject properties, capability-gated). The `set-ptz` action signal drives all three axes in one call. Always available, no socket required.
- Opt-in control socket: `control-socket=true` enables a Unix-domain PTZ socket. Default off. Per-instance path under `$XDG_RUNTIME_DIR` (mode 0600). The old world-accessible `/tmp/libuvc_control` is gone.
- Control socket protocol: `PAN_TILT <pan> <tilt>` and `ZOOM <zoom>` accept normalized floats in `[-1.0, 1.0]`, mapped onto the device's probed absolute range. Callers stay decoupled from per-device UVC units while the `pan`/`tilt`/`zoom` properties keep absolute units.
- Disconnect behavior: always posts `GST_ELEMENT_ERROR(RESOURCE, READ)` on device unplug.
- Opt-in reconnect: default off, gated on a confirmed-safe libuvc teardown spike (`libuvch264src/docs/notes/reconnect-spike.md`).
- LATENCY query: reports `live=TRUE`, `min=1/fps` instead of the GstBaseSrc default of zero.
- Buffer OFFSET: monotonic per-frame counter set on every buffer.
- V4L2 probe: one `VIDIOC_TRY_FMT` at open, logs H.264 availability, non-fatal.

**Build**
- Multiarch paths via `$(gcc -print-multiarch)`, no hardcoded `aarch64-linux-gnu`.
- Pinned base image and libuvc SHA in both Dockerfile and CMakeLists.txt.
- The Debian package CI (`publish-deb.yml`) is retained as the release path; the plugin version string is `2.0`.

### How to verify

```bash
# Full test suite with sanitizers
cmake -B build -DENABLE_SANITIZERS=ON && cmake --build build && ctest --test-dir build --output-on-failure

# Spot-check key areas
ctest --test-dir build -R "ptz_properties|ptz_capability_gate"
ctest --test-dir build -R "socket_default_off|socket_hardened"
ctest --test-dir build -R "device_selector"
ctest --test-dir build -R "usb_teardown"
ctest --test-dir build -R "negotiate_leak"
ctest --test-dir build -R "pts_monotonic|restart_idr"
ctest --test-dir build -R "nal_parse"
ctest --test-dir build -R "v4l2_probe"
```

On hardware: `gst-inspect-1.0 libuvch264src` should list `pan`, `tilt`, `zoom`, `control-socket`, `control-socket-path`, and the `set-ptz` action signal alongside `index`.

### Risks

- **Module split:** Zero behavior change by design. All five translation units compile to the same symbols as before, the split is purely organizational. The full ctest suite (including TSan/ASAN) validates this.
- **Control socket default off:** Any consumer that relied on the old `/tmp/libuvc_control` socket must now set `control-socket=true` and either read the resolved `control-socket-path` property or pass an explicit `control-socket-path`. Downstream consumers in this group standardize on `$XDG_RUNTIME_DIR/libuvc_control`.
- **Index selector expansion:** The `index` property now rejects malformed values that `atoi()` would have silently mapped to 0. A pipeline that passed a non-numeric string as `index` now fails loudly at `start()` instead of silently selecting device 0. This is the correct behavior.
- **TSan suppressions:** Two suppressions in `tsan_pts.suppressions` are permanent. `GST_OBJECT_LOCK` uses a raw futex in uninstrumented GLib, so TSan cannot see the happens-before relationship for PTS/clock fields. The behavioral tests (`pts_thread_safety`, `frame_throughput`) provide real coverage that the suppressions cannot mask.
