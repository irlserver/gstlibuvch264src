# SPIKE: libuvc dead-handle teardown safety (reconnect feasibility)

**Task:** gstlibuvch264src-hardening Task 4
**Question:** Is it safe to call `uvc_stop_streaming()` / `uvc_close()` on a libuvc
handle *after* the device has been unplugged (`LIBUSB_TRANSFER_NO_DEVICE`)? Does
it block, crash, or complete cleanly? This gates Task 18 (in-element reconnect).
**Method:** Code analysis of vendored libuvc v0.0.7 (+ the two repo patches) and
the element's `stop()` path, plus a pthread-faithful mock harness
(`test-results/task-4-spike.c`) run on a host with no UVC hardware.
**Scope:** Investigation + verdict only. No reconnect code, no libuvc edits.

---

## VERDICT: **SAFE**

Calling libuvc's **native** teardown — `uvc_stop_streaming()` → `uvc_close()` —
on a handle whose device has already delivered `LIBUSB_TRANSFER_NO_DEVICE` is
**safe**: it does not deadlock, the callback thread joins cleanly, and the
underlying `libusb_device_handle` is closed exactly once. In-element reconnect is
therefore **feasible**.

**Mandatory precondition for Task 18 — non-negotiable:**
The reconnect path **MUST NOT** call the element's
`gst_libuvc_h264_src_force_usb_release()` before `uvc_close()`. That helper
performs a premature `libusb_close()` on the handle that `uvc_close()` then
closes a **second time** → double-free / use-after-free. The native sequence is
safe; the *current element teardown* is not. (See §3.)

---

## 1. What actually happens on unplug (`LIBUSB_TRANSFER_NO_DEVICE`)

Three threads are live while streaming (all spawned indirectly by the element's
`start()` → `uvc_init(ctx, NULL)` → `uvc_open()` → `uvc_start_streaming()`):

| Thread | libuvc function | Role |
|--------|-----------------|------|
| event thread | `_uvc_handle_events` (`init.c:86`) | pumps `libusb_handle_events_completed`, fires transfer callbacks |
| callback thread (`cb_thread`) | `_uvc_user_caller` (`stream.c:1298`) | invokes the element's `frame_callback` |
| streaming thread | GStreamer `create()` / `stop()` | drives the element |

On unplug, the event thread delivers `LIBUSB_TRANSFER_NO_DEVICE` to
`_uvc_stream_callback` (`stream.c:807`) for each in-flight transfer. The
`NO_DEVICE` branch (`stream.c:841-865`), under `cb_mutex`:

1. frees the transfer buffer + `libusb_free_transfer`, sets `transfers[i] = NULL`;
2. sets `resubmit = 0` (the transfer is **not** resubmitted);
3. `pthread_cond_broadcast(&cb_cond)`.

After every in-flight transfer is processed, **all `strmh->transfers[i] == NULL`**
and `strmh->running` is still `1`.

> **Correction to the inherited "NULL frame signals disconnect" note.** In
> *callback* mode libuvc does **not** invoke `user_cb` with a NULL frame on
> `NO_DEVICE`. `_uvc_user_caller` only calls `user_cb` after a frame is populated
> (`hold_seq` advances), which never happens post-unplug. The element's
> `frame_callback` simply **stops being called** — frames go silent. The element
> learns nothing directly; `create()` then blocks forever on
> `g_async_queue_pop` (`gstlibuvch264src.c:1050`). That blocking-create is a
> *detection* problem for Task 18, separate from teardown safety.

## 2. Native teardown of a dead handle — why it is SAFE

`uvc_stop_streaming(devh)` (`stream.c:1482`) → `uvc_stream_close(strmh)`
(`:1547`) → `uvc_stream_stop(strmh)` (`:1497`):

- **No hang in the wait loop.** The cancel loop (`:1510-1513`) only cancels
  non-NULL transfers; after `NO_DEVICE` they are all NULL, so nothing is
  cancelled. The wait loop (`:1516-1524`) is **guarded** — it `break`s the moment
  every slot is NULL (`i == LIBUVC_NUM_TRANSFER_BUFS`). All slots are already
  NULL, so it returns immediately. No `pthread_cond_wait`, no deadlock.
- **`cb_thread` joins cleanly.** `uvc_stream_stop` sets `running = 0`, broadcasts
  `cb_cond`, then `pthread_join(cb_thread)` (`:1534`). `_uvc_user_caller` wakes,
  observes `!running`, breaks (`:1310-1312`), returns. Join completes.
- **Dead-device libusb calls are harmless.** `uvc_stream_close` →
  `uvc_release_if` and `uvc_close` → `uvc_release_if` (`device.c:1014`) issue
  `libusb_set_interface_alt_setting` / `libusb_release_interface` on the gone
  device; these return `LIBUSB_ERROR_NO_DEVICE` and the return is ignored. No
  crash.
- **Exactly one `libusb_close`.** `uvc_close` (`device.c:1728`): `devh->streams`
  is already NULL (the stream was `DL_DELETE`d), so it does **not** re-stop. With
  the element's single device and self-owned ctx (`own_usb_ctx == 1`,
  `open_devices == devh`, `devh->next == NULL`), it takes the kill-handler branch
  (`:1741-1744`): sets `kill_handler_thread = 1`, `libusb_close(devh->usb_devh)`
  **once**, then `pthread_join(handler_thread)`. The event thread's
  `libusb_handle_events_completed` returns and the loop exits on the kill flag.
  Clean.

The race variant — `stop()` called while a transfer is still in flight (its
`NO_DEVICE` not yet delivered) — is also safe: `uvc_stream_stop` calls
`libusb_cancel_transfer`; the still-running event thread delivers the
completion, which frees the slot and broadcasts, so the guarded wait loop
exits. The loop can only hang if the event thread is dead, which it is not
during `stop()`.

## 3. The current element teardown is UNSAFE (pre-existing, unplug-independent)

`gst_libuvc_h264_src_stop()` (`gstlibuvch264src.c:768`) ordering:

```
uvc_stop_streaming(devh)        // l.806  — SAFE (per §2)
force_usb_release(self)         // l.822  — libusb_close(usb_devh)   *** premature
uvc_close(devh)                 // l.825  — libusb_close(devh->usb_devh) AGAIN
uvc_unref_device(dev)           // l.831
uvc_exit(ctx)                   // l.837
```

`force_usb_release` (`:297-353`) fetches the handle via
`uvc_get_libusb_handle` (which returns `devh->usb_devh`, `device.c:845-847`) and
calls `libusb_close(usb_devh)` (`:343`) — **without** nulling `devh->usb_devh`
(it cannot; the field is internal). `uvc_close` then:

- calls `uvc_release_if(devh, ctrl_if)` which touches the **freed**
  `devh->usb_devh` (the `claimed` bitmask was never cleared, because
  `force_usb_release` used raw `libusb_release_interface`, not `uvc_release_if`)
  → **use-after-free**, and
- calls `libusb_close(devh->usb_devh)` a **second time** (`device.c:1743`/`1746`)
  → **double-free**.

There is also a latent UAF at `:348`, `libusb_reset_device(usb_devh)` after the
handle is closed, guarded by `#ifdef LIBUSB_HAS_GET_DEVICE` (normally compiled
out on modern libusb).

This is broken on a **clean** stop too; the dead-device case only adds harmless
`NO_DEVICE` error returns on top. It "appears to work" today only because a
double `libusb_close` is undefined behaviour that does not always crash
immediately — it corrupts the heap.

## 4. Evidence — mock harness

`test-results/task-4-spike.c` reproduces the three control-flow shapes above with
real pthreads, a watchdog (turns a deadlock into a printed `*** HANG ***` rather
than blocking forever), and a mock libusb layer that counts closes. Full output:
`test-results/task-4-spike.log`.

| Scenario | Teardown | Result | close count | double-free |
|----------|----------|--------|-------------|-------------|
| A | NO_DEVICE → native (`stop_streaming`→`uvc_close`) | completed, no hang | 1 | 0 — **SAFE** |
| B | NO_DEVICE → current element (`force_usb_release`→`uvc_close`) | completed, no hang | 2 | 1 — **UNSAFE (double-free)** |
| C | `stop()` races in-flight transfer → native | completed, no hang | 1 | 0 — **SAFE** |

Scenario A/C confirm: native teardown of a dead handle neither hangs nor
double-closes. Scenario B reproduces the element's double `libusb_close`.

## 5. Recommended teardown → reopen sequence for Task 18

Reconnect is **feasible**. Use libuvc's **native** lifecycle and **drop**
`force_usb_release` from the reconnect path.

**Teardown of the dead handle:**
```
uvc_stop_streaming(devh);     // joins cb_thread, frees stream — SAFE on dead dev
uvc_close(devh);              // single libusb_close, joins event thread
uvc_unref_device(dev);        // dev = NULL
//  DO NOT call force_usb_release() anywhere in this path
```

**Re-open (two viable shapes):**

- **Keep the context** (lighter, preferred): retain `uvc_ctx`; after
  `uvc_close` + `uvc_unref_device`, re-enumerate and reopen:
  ```
  uvc_find_devices(ctx, &list, 0, 0, NULL);   // re-enumerate; device may have a new bus/addr
  // select by the element's `index` contract, then:
  uvc_ref_device(selected);
  uvc_free_device_list(list, 1);              // only AFTER uvc_ref_device(selected)
  uvc_open(dev, &devh);
  uvc_get_stream_ctrl_format_size(devh, &ctrl, ...);
  uvc_start_streaming(devh, &ctrl, frame_callback, self, 0);
  ```
  Note: after `uvc_close` closes the last device, `kill_handler_thread` is set
  and the event thread is joined; the next `uvc_open` re-spawns it via
  `uvc_start_handler_thread` (`device.c:396-399`). The kill flag is set on the
  **ctx**, not the devh — Task 18 MUST confirm a re-`uvc_open` on the same ctx
  re-clears `kill_handler_thread` before relying on this shape; if it does not,
  use the full-context shape below.

- **Full context recycle** (simplest, most robust): tear down to
  `uvc_exit(ctx)` and re-run the element's existing `start()` enumeration from
  `uvc_init` again. Zero shared-state risk; slightly heavier.

**Hard constraints for Task 18:**
1. Never call `force_usb_release()` / a bare `libusb_close()` before `uvc_close()`.
   Fix or delete that helper — it is the actual crash vector.
2. Disconnect must be *detected* explicitly (e.g. watchdog on frame silence /
   bounded `create()` pop), because libuvc delivers **no** NULL-frame signal in
   callback mode (§1).
3. Re-enumeration must re-resolve the device — bus/address can change across a
   replug; the element's `index` string contract still applies.

---

## Summary

| Question | Answer |
|----------|--------|
| `uvc_stop_streaming()` on a dead handle blocks? | **No** — wait loop is guarded; returns immediately |
| `uvc_close()` on a dead handle crashes? | **No**, *if* called natively (single `libusb_close`) |
| `cb_thread` exits cleanly? | **Yes** — `running=0` + broadcast wakes it; join returns |
| Is in-element reconnect feasible? | **Yes — SAFE**, provided `force_usb_release` is removed from the path |
| What breaks today? | The element's `force_usb_release` → `uvc_close` ordering double-closes the libusb handle |
