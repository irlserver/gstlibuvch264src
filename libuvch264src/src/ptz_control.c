#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "gstlibuvch264src_internal.h"
#include "ptz_control.h"
#include "gstlibuvch264src_error.h"

static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command);

/* Per-process counter so two elements in one process derive distinct default
 * socket paths - the pid is shared, only this disambiguates them (M9). */
static gint control_socket_seq = 0;

gboolean gst_libuvc_h264_src_control_socket_bind(GstLibuvcH264Src *self) {
    struct sockaddr_un addr;

    if (self->control_socket_path == NULL) {
        const gchar *runtime = g_getenv("XDG_RUNTIME_DIR");
        if (runtime == NULL || *runtime == '\0') {
            GST_ERROR_OBJECT(self, "XDG_RUNTIME_DIR is unset; refusing to bind the "
                             "control socket in a world-accessible location");
            return FALSE;
        }
        guint id = (guint) g_atomic_int_add(&control_socket_seq, 1);
        self->control_socket_path = g_strdup_printf("%s/libuvch264src-%d-%u.sock",
                                                    runtime, (int) getpid(), id);
    }

    if (strlen(self->control_socket_path) >= sizeof(addr.sun_path)) {
        GST_ERROR_OBJECT(self, "Control socket path too long: %s",
                         self->control_socket_path);
        return FALSE;
    }

    self->control_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (self->control_socket < 0) {
        GST_ERROR_OBJECT(self, "Failed to create control socket");
        return FALSE;
    }

    int flags = fcntl(self->control_socket, F_GETFL, 0);
    fcntl(self->control_socket, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, self->control_socket_path, sizeof(addr.sun_path));

    /* Drop a socket left by an unclean prior exit; bind to a live path fails. */
    unlink(self->control_socket_path);

    if (bind(self->control_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        GST_ERROR_OBJECT(self, "Failed to bind control socket at %s",
                         self->control_socket_path);
        close(self->control_socket);
        self->control_socket = -1;
        return FALSE;
    }

    /* Owner-only: the socket carries unauthenticated PTZ control, so restrict it
     * to this user on top of the already-0700 $XDG_RUNTIME_DIR (M9). */
    if (chmod(self->control_socket_path, S_IRUSR | S_IWUSR) < 0) {
        GST_ERROR_OBJECT(self, "Failed to chmod control socket %s to 0600",
                         self->control_socket_path);
        close(self->control_socket);
        self->control_socket = -1;
        unlink(self->control_socket_path);
        return FALSE;
    }

    if (listen(self->control_socket, 5) < 0) {
        GST_ERROR_OBJECT(self, "Failed to listen on control socket");
        close(self->control_socket);
        self->control_socket = -1;
        unlink(self->control_socket_path);
        return FALSE;
    }

    GST_INFO_OBJECT(self, "Control socket listening on %s", self->control_socket_path);
    return TRUE;
}

void gst_libuvc_h264_src_control_socket_unbind(GstLibuvcH264Src *self) {
    if (self->control_socket >= 0) {
        close(self->control_socket);
        self->control_socket = -1;
    }
    if (self->control_socket_path != NULL) {
        unlink(self->control_socket_path);
    }
}

// Control socket accept loop. The socket is already created, bound and listening
// (gst_libuvc_h264_src_control_socket_bind, run in start()), which closes the old
// race where the listening fd was assigned from inside this thread.
gpointer gst_libuvc_h264_src_control_thread(gpointer data) {
    GstLibuvcH264Src *self = (GstLibuvcH264Src *)data;
    int client_fd;
    char buffer[256];
    fd_set read_fds;
    struct timeval timeout;

    while (self->control_running) {
        FD_ZERO(&read_fds);
        FD_SET(self->control_socket, &read_fds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int result = select(self->control_socket + 1, &read_fds, NULL, NULL, &timeout);
        
        if (result > 0 && FD_ISSET(self->control_socket, &read_fds)) {
            client_fd = accept(self->control_socket, NULL, NULL);
            if (client_fd >= 0) {
                ssize_t len = read(client_fd, buffer, sizeof(buffer)-1);
                if (len > 0) {
                    buffer[len] = 0;
                    GST_INFO_OBJECT(self, "Received control command: %s", buffer);
                    char *response = gst_libuvc_h264_src_process_control_command(self, buffer);
                    if (response) {
                        if (write(client_fd, response, strlen(response)) < 0) {
                            GST_WARNING_OBJECT(self, "Failed to write response to control socket");
                        }
                        g_free(response);
                    } else {
                        const char *default_response = "OK";
                        if (write(client_fd, default_response, strlen(default_response)) < 0) {
                            GST_WARNING_OBJECT(self, "Failed to write default response to control socket");
                        }
                    }
                }
                close(client_fd);
            }
        } else if (result == 0) {
            continue;
        } else {
            if (self->control_running) {
                GST_WARNING_OBJECT(self, "Select error in control thread");
            }
            break;
        }
    }
    
    GST_DEBUG_OBJECT(self, "Control thread exiting");
    return NULL;
}

/* Map a normalized rate in [-1, 1] onto an absolute UVC range [min, max].
 * -1 -> min, 0 -> center, +1 -> max. Callers on the socket speak in normalized
 * units so they stay decoupled from per-device UVC ranges; the pan/tilt/zoom
 * GObject properties take absolute units for direct control. */
static gint ptz_norm_to_abs(double norm, gint min, gint max) {
    norm = CLAMP(norm, -1.0, 1.0);
    double v = (double) min + ((norm + 1.0) / 2.0) * (double) (max - min);
    /* Round half away from zero without pulling in libm (max/min may be negative). */
    return (gint) (v >= 0.0 ? v + 0.5 : v - 0.5);
}

/* Routes through the shared PTZ helpers (one clamp/gate/lock path for both
 * properties and socket). The setters lock control_mutex internally, so they
 * MUST be called without it held; GET_* snapshots cached state separately.
 *
 * PAN_TILT and ZOOM arguments are normalized floats in [-1, 1], mapped onto the
 * device's probed absolute range here. */
static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command) {
    double pan_n, tilt_n, zoom_n;

    if (sscanf(command, "PAN_TILT %lf %lf", &pan_n, &tilt_n) == 2) {
        g_mutex_lock(&self->control_mutex);
        gboolean ps = self->pan_supported, ts = self->tilt_supported;
        gint pmin = self->pan_min, pmax = self->pan_max;
        gint tmin = self->tilt_min, tmax = self->tilt_max;
        g_mutex_unlock(&self->control_mutex);

        gboolean any = FALSE, ok = TRUE;
        if (ps) {
            any = TRUE;
            ok = gst_libuvc_h264_src_ptz_set_pan(self, ptz_norm_to_abs(pan_n, pmin, pmax)) && ok;
        }
        if (ts) {
            any = TRUE;
            ok = gst_libuvc_h264_src_ptz_set_tilt(self, ptz_norm_to_abs(tilt_n, tmin, tmax)) && ok;
        }
        if (!any) return g_strdup("ERROR: pan/tilt not supported");
        if (!ok) return g_strdup("ERROR: pan/tilt transfer failed");

        g_mutex_lock(&self->control_mutex);
        gint p = self->pan_cur, t = self->tilt_cur;
        g_mutex_unlock(&self->control_mutex);
        return g_strdup_printf("OK pan=%d tilt=%d", p, t);
    }
    else if (sscanf(command, "ZOOM %lf", &zoom_n) == 1) {
        g_mutex_lock(&self->control_mutex);
        gboolean zs = self->zoom_supported;
        gint zmin = self->zoom_min, zmax = self->zoom_max;
        g_mutex_unlock(&self->control_mutex);

        if (!zs) return g_strdup("ERROR: zoom not supported");
        if (!gst_libuvc_h264_src_ptz_set_zoom(self, ptz_norm_to_abs(zoom_n, zmin, zmax)))
            return g_strdup("ERROR: zoom transfer failed");

        g_mutex_lock(&self->control_mutex);
        gint z = self->zoom_cur;
        g_mutex_unlock(&self->control_mutex);
        return g_strdup_printf("OK zoom=%d", z);
    }
    else if (strcmp(command, "GET_POSITION") == 0) {
        g_mutex_lock(&self->control_mutex);
        gint p = self->pan_cur, t = self->tilt_cur, z = self->zoom_cur;
        g_mutex_unlock(&self->control_mutex);
        return g_strdup_printf("OK pan=%d tilt=%d zoom=%d", p, t, z);
    }
    else if (strcmp(command, "GET_CAPABILITIES") == 0) {
        g_mutex_lock(&self->control_mutex);
        gboolean ps = self->pan_supported, ts = self->tilt_supported, zs = self->zoom_supported;
        gint pmin = self->pan_min, pmax = self->pan_max;
        gint tmin = self->tilt_min, tmax = self->tilt_max;
        gint zmin = self->zoom_min, zmax = self->zoom_max;
        g_mutex_unlock(&self->control_mutex);

        GString *caps = g_string_new("CAPABILITIES:");
        if (ps) g_string_append_printf(caps, " pan=[%d,%d]", pmin, pmax);
        if (ts) g_string_append_printf(caps, " tilt=[%d,%d]", tmin, tmax);
        if (zs) g_string_append_printf(caps, " zoom=[%d,%d]", zmin, zmax);
        return g_string_free(caps, FALSE);
    }

    return g_strdup("ERROR: Unknown command");
}

static gint ptz_clamp(gint value, gint lo, gint hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

void gst_libuvc_h264_src_ptz_probe_capabilities(GstLibuvcH264Src *self) {
    /* M6: start every axis gated off with zeroed ranges; only a fully-checked
     * probe re-enables one, so an unchecked GET_* value is never stored. */
    self->pan_supported = FALSE;
    self->tilt_supported = FALSE;
    self->zoom_supported = FALSE;
    self->ptz_supported = FALSE;
    self->pan_min = self->pan_max = self->pan_cur = 0;
    self->tilt_min = self->tilt_max = self->tilt_cur = 0;
    self->zoom_min = self->zoom_max = self->zoom_cur = 0;

    if (!self->uvc_devh) return;

    g_mutex_lock(&self->control_mutex);

    /* PanTilt is one UVC control carrying both axes. Trust the range only when
     * MIN, MAX and RES all succeed; an axis counts as supported only if its own
     * range is non-degenerate, so a device with a fixed axis stays gated off. */
    int32_t pan_min = 0, pan_max = 0, pan_res = 0;
    int32_t tilt_min = 0, tilt_max = 0, tilt_res = 0;
    uvc_error_t pt_min = uvc_get_pantilt_abs(self->uvc_devh, &pan_min, &tilt_min, UVC_GET_MIN);
    uvc_error_t pt_max = uvc_get_pantilt_abs(self->uvc_devh, &pan_max, &tilt_max, UVC_GET_MAX);
    uvc_error_t pt_res = uvc_get_pantilt_abs(self->uvc_devh, &pan_res, &tilt_res, UVC_GET_RES);

    if (pt_min == UVC_SUCCESS && pt_max == UVC_SUCCESS && pt_res == UVC_SUCCESS) {
        if (pan_min < pan_max) {
            self->pan_supported = TRUE;
            self->pan_min = pan_min;
            self->pan_max = pan_max;
        }
        if (tilt_min < tilt_max) {
            self->tilt_supported = TRUE;
            self->tilt_min = tilt_min;
            self->tilt_max = tilt_max;
        }
        int32_t pan_cur = 0, tilt_cur = 0;
        if (uvc_get_pantilt_abs(self->uvc_devh, &pan_cur, &tilt_cur, UVC_GET_CUR) == UVC_SUCCESS) {
            self->pan_cur = pan_cur;
            self->tilt_cur = tilt_cur;
        }
    }

    uint16_t zoom_min = 0, zoom_max = 0, zoom_res = 0;
    uvc_error_t z_min = uvc_get_zoom_abs(self->uvc_devh, &zoom_min, UVC_GET_MIN);
    uvc_error_t z_max = uvc_get_zoom_abs(self->uvc_devh, &zoom_max, UVC_GET_MAX);
    uvc_error_t z_res = uvc_get_zoom_abs(self->uvc_devh, &zoom_res, UVC_GET_RES);

    if (z_min == UVC_SUCCESS && z_max == UVC_SUCCESS && z_res == UVC_SUCCESS &&
        zoom_min < zoom_max) {
        self->zoom_supported = TRUE;
        self->zoom_min = zoom_min;
        self->zoom_max = zoom_max;
        uint16_t zoom_cur = 0;
        if (uvc_get_zoom_abs(self->uvc_devh, &zoom_cur, UVC_GET_CUR) == UVC_SUCCESS) {
            self->zoom_cur = zoom_cur;
        }
    }

    self->ptz_supported =
        self->pan_supported || self->tilt_supported || self->zoom_supported;

    g_mutex_unlock(&self->control_mutex);

    /* Only stored (checked) fields are logged; unsupported axes read back 0. */
    GST_INFO_OBJECT(self, "PTZ probe: pan=%s[%d,%d] tilt=%s[%d,%d] zoom=%s[%d,%d]",
                    self->pan_supported ? "on" : "off", self->pan_min, self->pan_max,
                    self->tilt_supported ? "on" : "off", self->tilt_min, self->tilt_max,
                    self->zoom_supported ? "on" : "off", self->zoom_min, self->zoom_max);
}

gboolean gst_libuvc_h264_src_ptz_set_pan(GstLibuvcH264Src *self, gint value) {
    if (!self->uvc_devh || !self->pan_supported) return FALSE;

    g_mutex_lock(&self->control_mutex);
    gint pan = ptz_clamp(value, self->pan_min, self->pan_max);
    /* Re-send the current tilt: pan and tilt are one control transfer. */
    uvc_error_t res = uvc_set_pantilt_abs(self->uvc_devh, pan, self->tilt_cur);
    if (res == UVC_SUCCESS) self->pan_cur = pan;
    g_mutex_unlock(&self->control_mutex);

    if (res != UVC_SUCCESS) {
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), res, "setting pan");
        return FALSE;
    }
    GST_DEBUG_OBJECT(self, "Pan set to %d", pan);
    return TRUE;
}

gboolean gst_libuvc_h264_src_ptz_set_tilt(GstLibuvcH264Src *self, gint value) {
    if (!self->uvc_devh || !self->tilt_supported) return FALSE;

    g_mutex_lock(&self->control_mutex);
    gint tilt = ptz_clamp(value, self->tilt_min, self->tilt_max);
    uvc_error_t res = uvc_set_pantilt_abs(self->uvc_devh, self->pan_cur, tilt);
    if (res == UVC_SUCCESS) self->tilt_cur = tilt;
    g_mutex_unlock(&self->control_mutex);

    if (res != UVC_SUCCESS) {
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), res, "setting tilt");
        return FALSE;
    }
    GST_DEBUG_OBJECT(self, "Tilt set to %d", tilt);
    return TRUE;
}

gboolean gst_libuvc_h264_src_ptz_set_zoom(GstLibuvcH264Src *self, gint value) {
    if (!self->uvc_devh || !self->zoom_supported) return FALSE;

    g_mutex_lock(&self->control_mutex);
    gint zoom = ptz_clamp(value, self->zoom_min, self->zoom_max);
    uvc_error_t res = uvc_set_zoom_abs(self->uvc_devh, (uint16_t)zoom);
    if (res == UVC_SUCCESS) self->zoom_cur = zoom;
    g_mutex_unlock(&self->control_mutex);

    if (res != UVC_SUCCESS) {
        gst_libuvc_h264_src_post_error(GST_ELEMENT(self), res, "setting zoom");
        return FALSE;
    }
    GST_DEBUG_OBJECT(self, "Zoom set to %d", zoom);
    return TRUE;
}
