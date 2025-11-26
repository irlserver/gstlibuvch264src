// Enhanced control socket thread function
static gpointer gst_libuvc_h264_src_control_thread(GstLibuvcH264Src *self) {
    struct sockaddr_un addr;
    int client_fd;
    char buffer[256];
    
    // Create socket
    self->control_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (self->control_socket < 0) {
        GST_ERROR_OBJECT(self, "Failed to create control socket");
        return NULL;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/libuvc_control");
    
    // Remove existing socket
    unlink(addr.sun_path);
    
    if (bind(self->control_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        GST_ERROR_OBJECT(self, "Failed to bind control socket");
        close(self->control_socket);
        self->control_socket = -1;
        return NULL;
    }
    
    if (listen(self->control_socket, 5) < 0) {
        GST_ERROR_OBJECT(self, "Failed to listen on control socket");
        close(self->control_socket);
        self->control_socket = -1;
        return NULL;
    }
    
    GST_INFO_OBJECT(self, "Control socket listening on /tmp/libuvc_control");
    
    while (self->control_running) {
        client_fd = accept(self->control_socket, NULL, NULL);
        if (client_fd > 0) {
            ssize_t len = read(client_fd, buffer, sizeof(buffer)-1);
            if (len > 0) {
                buffer[len] = 0;
                GST_INFO_OBJECT(self, "Received control command: %s", buffer);
                char *response = gst_libuvc_h264_src_process_control_command(self, buffer);
                if (response) {
                    write(client_fd, response, strlen(response));
                    g_free(response);
                } else {
                    const char *default_response = "OK";
                    write(client_fd, default_response, strlen(default_response));
                }
            }
            close(client_fd);
        } else {
            // Small delay to prevent busy waiting
            usleep(100000); // 100ms
        }
    }
    
    return NULL;
}

// Enhanced control command processor with zoom support
static char* gst_libuvc_h264_src_process_control_command(GstLibuvcH264Src *self, const char *command) {
    int pan, tilt, zoom;
    uint16_t zoom_abs;
    
    g_mutex_lock(&self->control_mutex);
    
    if (sscanf(command, "PAN_TILT %d %d", &pan, &tilt) == 2) {
        if (self->uvc_devh) {
            uvc_error_t res = uvc_set_pantilt_abs(self->uvc_devh, pan, tilt);
            if (res == UVC_SUCCESS) {
                GST_INFO_OBJECT(self, "Set pan/tilt to: %d/%d", pan, tilt);
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("OK pan=%d tilt=%d", pan, tilt);
            } else {
                GST_WARNING_OBJECT(self, "Failed to set pan/tilt: %s", uvc_strerror(res));
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("ERROR: %s", uvc_strerror(res));
            }
        }
    } 
    else if (sscanf(command, "ZOOM %d", &zoom) == 1) {
        if (self->uvc_devh) {
            zoom_abs = (uint16_t)zoom;
            uvc_error_t res = uvc_set_zoom_abs(self->uvc_devh, zoom_abs);
            if (res == UVC_SUCCESS) {
                GST_INFO_OBJECT(self, "Set zoom to: %d", zoom_abs);
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("OK zoom=%d", zoom_abs);
            } else {
                GST_WARNING_OBJECT(self, "Failed to set zoom: %s", uvc_strerror(res));
                g_mutex_unlock(&self->control_mutex);
                return g_strdup_printf("ERROR: %s", uvc_strerror(res));
            }
        }
    }
    else if (strcmp(command, "GET_POSITION") == 0) {
        if (self->uvc_devh) {
            int32_t current_pan, current_tilt;
            uint16_t current_zoom;
            char *response = NULL;
            
            uvc_error_t res_pan = uvc_get_pantilt_abs(self->uvc_devh, &current_pan, &current_tilt, UVC_GET_CUR);
            uvc_error_t res_zoom = uvc_get_zoom_abs(self->uvc_devh, &current_zoom, UVC_GET_CUR);
            
            if (res_pan == UVC_SUCCESS && res_zoom == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=%d tilt=%d zoom=%d", current_pan, current_tilt, current_zoom);
            } else if (res_pan == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=%d tilt=%d zoom=unknown", current_pan, current_tilt);
            } else if (res_zoom == UVC_SUCCESS) {
                response = g_strdup_printf("OK pan=unknown tilt=unknown zoom=%d", current_zoom);
            } else {
                response = g_strdup("ERROR: Cannot read position");
            }
            
            GST_INFO_OBJECT(self, "Current position: pan=%d, tilt=%d, zoom=%d", 
                           current_pan, current_tilt, current_zoom);
            g_mutex_unlock(&self->control_mutex);
            return response;
        }
    }
    else if (strcmp(command, "GET_CAPABILITIES") == 0) {
        if (self->uvc_devh) {
            GString *caps = g_string_new("CAPABILITIES:");
            
            // Check pan/tilt capabilities
            int32_t pan_min, pan_max, pan_step;
            int32_t tilt_min, tilt_max, tilt_step;
            uvc_error_t res_pt = uvc_get_pantilt_abs(self->uvc_devh, &pan_min, &tilt_min, UVC_GET_MIN);
            if (res_pt == UVC_SUCCESS) {
                uvc_get_pantilt_abs(self->uvc_devh, &pan_max, &tilt_max, UVC_GET_MAX);
                uvc_get_pantilt_abs(self->uvc_devh, &pan_step, &tilt_step, UVC_GET_RES);
                g_string_append_printf(caps, " pan=[%d,%d,step=%d] tilt=[%d,%d,step=%d]", 
                                      pan_min, pan_max, pan_step, tilt_min, tilt_max, tilt_step);
            }
            
            // Check zoom capabilities
            uint16_t zoom_min, zoom_max, zoom_step;
            uvc_error_t res_zoom = uvc_get_zoom_abs(self->uvc_devh, &zoom_min, UVC_GET_MIN);
            if (res_zoom == UVC_SUCCESS) {
                uvc_get_zoom_abs(self->uvc_devh, &zoom_max, UVC_GET_MAX);
                uvc_get_zoom_abs(self->uvc_devh, &zoom_step, UVC_GET_RES);
                g_string_append_printf(caps, " zoom=[%d,%d,step=%d]", zoom_min, zoom_max, zoom_step);
            }
            
            GST_INFO_OBJECT(self, "Capabilities: %s", caps->str);
            g_mutex_unlock(&self->control_mutex);
            return g_string_free(caps, FALSE);
        }
    }
    else if (strncmp(command, "RESET", 5) == 0) {
        // Reset to default position (you might need to adjust these values)
        if (self->uvc_devh) {
            uvc_set_pantilt_abs(self->uvc_devh, 0, 0);
            uvc_set_zoom_abs(self->uvc_devh, 100); // Typical default zoom
            GST_INFO_OBJECT(self, "Reset to default position");
            g_mutex_unlock(&self->control_mutex);
            return g_strdup("OK reset complete");
        }
    }
    
    g_mutex_unlock(&self->control_mutex);
    return g_strdup("ERROR: Unknown command");
}
