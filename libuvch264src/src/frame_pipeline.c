#include <string.h>
#include "gstlibuvch264src_internal.h"
#include "frame_pipeline.h"
#include "spspps_cache.h"

nal_unit_type_t convert_unit_type(enum uvc_frame_format format, int type) {
    if (format == UVC_FRAME_FORMAT_H264) {
        switch (type) {
            case 1:
                return UNIT_FRAME_NON_IDR;
            case 5:
                return UNIT_FRAME_IDR;
            case 7:
                return UNIT_SPS;
            case 8:
                return UNIT_PPS;
      }

    } else if (format == UVC_FRAME_FORMAT_H265) {
        switch (type) {
            case 1:
                return UNIT_FRAME_NON_IDR;
            case 20:
                return UNIT_FRAME_IDR;
            case 32:
                return UNIT_VPS;
            case 33:
                return UNIT_SPS;
            case 34:
                return UNIT_PPS;
        }
    }

    return UNIT_INVALID;
}

/* Locate the next Annex-B NAL unit at or after `start`.
 *
 * Detects BOTH the 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes. The
 * 3-byte form is legal Annex-B and is emitted by real DJI/UVC cameras; missing
 * it merges two slices into one oversized NAL (L3). With search != 0 the scan
 * walks forward to the first start code anywhere in the buffer, so a frame that
 * does not begin exactly at offset 0 is found rather than dropped.
 *
 * On success returns the NAL type, sets *offset to the first byte of the start
 * code and *sc_len to its length (3 or 4). Lengths are gsize so a frame larger
 * than INT_MAX cannot wrap to a negative length and be skipped (L4). */
int find_nal_unit(enum uvc_frame_format format,
                  unsigned char *buf, gsize buflen, gsize start, int search,
                  gsize *offset, gsize *sc_len) {
    if (format != UVC_FRAME_FORMAT_H264 && format != UVC_FRAME_FORMAT_H265) return -1;
    if (buf == NULL) return -1;
    /* A unit needs at least a 3-byte start code plus one NAL header byte. */
    if (buflen < 4 || start > buflen - 4) return -1;

    for (gsize i = start; i <= buflen - 4; i++) {
        gsize hdr;
        gsize code_len;

        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
            if (i + 4 >= buflen) break;   /* 4-byte start code with no header byte */
            hdr = i + 4;
            code_len = 4;
        } else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            hdr = i + 3;
            code_len = 3;
        } else {
            if (!search) break;
            continue;
        }

        if (offset) *offset = i;
        if (sc_len) *sc_len = code_len;
        if (format == UVC_FRAME_FORMAT_H264) {
            return convert_unit_type(format, buf[hdr] & 0x1F);
        }
        return convert_unit_type(format, (buf[hdr] >> 1) & 0x3F);
    }

    return -1;
}

/* Count the NAL units in `buf` so the caller can size an exact allocation and
 * never drop slices past a fixed cap (L2). */
gsize count_nal_units(enum uvc_frame_format format,
                      unsigned char *buf, gsize buflen) {
    gsize count = 0;
    gsize nal_offset = 0;
    gsize sc_len = 0;
    int next_type = find_nal_unit(format, buf, buflen, 0, 1, &nal_offset, &sc_len);
    while (next_type >= 0) {
        count++;
        next_type = find_nal_unit(format, buf, buflen, nal_offset + sc_len, 1,
                                  &nal_offset, &sc_len);
    }
    return count;
}

gsize parse_nal_units(enum uvc_frame_format format,
                      nal_unit_t *units, gsize max, unsigned char *buf, gsize buflen) {
    gsize i = 0;
    gsize nal_offset = 0;
    gsize sc_len = 0;

    /* First scan searches (search=1) so an offset-shifted frame is still found
       rather than dropped when it does not start exactly at offset 0 (L3). */
    int next_type = find_nal_unit(format, buf, buflen, 0, 1, &nal_offset, &sc_len);
    while (next_type >= 0 && i < max) {
        int type = next_type;
        gsize start = nal_offset;

        /* Advance past THIS start code only, so a 3-byte code sitting right
           after a 4-byte one is not skipped. */
        next_type = find_nal_unit(format, buf, buflen, nal_offset + sc_len, 1,
                                  &nal_offset, &sc_len);
        gsize end = (next_type >= 0) ? nal_offset : buflen;

        units[i].type = type;
        units[i].len = end - start;
        units[i].ptr = &buf[start];

        i++;
    }

    return i;
}

void frame_callback(uvc_frame_t *frame, void *ptr) {
    GstLibuvcH264Src *self = (GstLibuvcH264Src *)ptr;

    if (!frame || !frame->data || frame->data_bytes <= 0) {
        GST_WARNING_OBJECT(self, "Empty or invalid frame received.");
        return;
    }

    /* data_bytes is a size_t; a frame larger than INT_MAX would historically
       truncate to a negative int length and be silently dropped. Such a frame
       is corrupt/absurd, so reject it explicitly up front (L4). */
    if (frame->data_bytes > (gsize)G_MAXINT) {
        GST_WARNING_OBJECT(self, "Dropping oversized frame (%" G_GSIZE_FORMAT
                           " bytes; exceeds G_MAXINT).", (gsize)frame->data_bytes);
        return;
    }

    unsigned char* data = frame->data;
    gsize data_bytes = frame->data_bytes;
    gboolean updated_sps_pps = FALSE;

    /* The clock and the PTS baseline are shared with set_clock(), which can swap
       the clock or reset the baseline from another thread. Snapshot the clock
       under the object lock and take our own ref, so reading the time (the
       expensive part) and dropping the ref happen outside the lock and can never
       race a concurrent unref/free. */
    GstClock *clock = NULL;
    GstClockTime base_time = 0;
    GST_OBJECT_LOCK(self);
    if (self->clock) {
        clock = gst_object_ref(self->clock);
        base_time = self->base_time;
    }
    GST_OBJECT_UNLOCK(self);

    if (!clock) return;
    GstClockTime now = gst_clock_get_time(clock);
    gst_object_unref(clock);

    /* Latch the running base time on the first frame after a (re)start or clock
       change. gst_element_get_base_time() takes the object lock itself, so read
       it before re-entering our critical section, then commit under the lock. */
    if (base_time == G_MAXUINT64) {
        base_time = gst_element_get_base_time(GST_ELEMENT(self));
        GST_OBJECT_LOCK(self);
        self->base_time = base_time;
        GST_OBJECT_UNLOCK(self);
    }
    GstClockTime ts = now - base_time;

    /* Size the array to the actual NAL count so a multi-slice frame (4K can
       carry well over a dozen slices) delivers every slice instead of dropping
       units past a fixed cap (L2). */
    gsize unit_count = count_nal_units(self->frame_format, data, data_bytes);
    nal_unit_t *units = g_new(nal_unit_t, unit_count ? unit_count : 1);
    gsize c = parse_nal_units(self->frame_format, units, unit_count, data, data_bytes);

    for (gsize i = 0; i < c; i++) {
        nal_unit_t *unit = &units[i];
        GstBuffer *buffer = NULL;
        gsize buffer_offset = 0;

        switch (unit->type) {
            case UNIT_VPS:
                if (unit->len == 0 || unit->len > SPSPPSBUFSZ) {
                    GST_WARNING_OBJECT(self, "Dropping oversized/invalid VPS NAL "
                        "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                        unit->len, SPSPPSBUFSZ);
                    continue;
                }
                // L10: only flag a disk write when the parameter set actually
                // changed. SPS/PPS/VPS repeat before every IDR, so an
                // unconditional store rewrites the cache file each GOP and wears
                // the flash for nothing. send_sps_pps still latches every time so
                // the sets are re-prepended in-band; only the cache write is gated.
                if ((gsize)self->vps_length != unit->len ||
                    memcmp(self->vps, unit->ptr, unit->len) != 0) {
                    self->vps_length = unit->len;
                    memcpy(self->vps, unit->ptr, self->vps_length);
                    updated_sps_pps = TRUE;
                }
                self->send_sps_pps = TRUE;
                continue;
            case UNIT_SPS:
                if (unit->len == 0 || unit->len > SPSPPSBUFSZ) {
                    GST_WARNING_OBJECT(self, "Dropping oversized/invalid SPS NAL "
                        "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                        unit->len, SPSPPSBUFSZ);
                    continue;
                }
                if ((gsize)self->sps_length != unit->len ||
                    memcmp(self->sps, unit->ptr, unit->len) != 0) {
                    self->sps_length = unit->len;
                    memcpy(self->sps, unit->ptr, self->sps_length);
                    updated_sps_pps = TRUE;
                }
                self->send_sps_pps = TRUE;
                continue;
            case UNIT_PPS:
                if (unit->len == 0 || unit->len > SPSPPSBUFSZ) {
                    GST_WARNING_OBJECT(self, "Dropping oversized/invalid PPS NAL "
                        "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                        unit->len, SPSPPSBUFSZ);
                    continue;
                }
                if ((gsize)self->pps_length != unit->len ||
                    memcmp(self->pps, unit->ptr, unit->len) != 0) {
                    self->pps_length = unit->len;
                    memcpy(self->pps, unit->ptr, self->pps_length);
                    updated_sps_pps = TRUE;
                }
                self->send_sps_pps = TRUE;
                continue;
            case UNIT_FRAME_IDR: {
                if (!self->had_idr || self->send_sps_pps) {
                    buffer_offset = self->sps_length + self->pps_length;
                    if (self->frame_format == UVC_FRAME_FORMAT_H265) {
                        buffer_offset += self->vps_length;
                    }

                    buffer = gst_buffer_new_allocate(NULL, buffer_offset + unit->len, NULL);
                    int offset = 0;
                    if (self->frame_format == UVC_FRAME_FORMAT_H265) {
                        gst_buffer_fill(buffer, offset, self->vps, self->vps_length);
                        offset += self->vps_length;
                    }
                    gst_buffer_fill(buffer, offset, self->sps, self->sps_length);
                    offset += self->sps_length;

                    gst_buffer_fill(buffer, offset, self->pps, self->pps_length);
                    self->send_sps_pps = FALSE;
                }
                if (!self->had_idr) {
                    self->had_idr = TRUE;
                }
                break;
            }
            default:
                if (!self->had_idr) {
                    continue;
                }
        }

        if (!buffer) {
          buffer = gst_buffer_new_allocate(NULL, unit->len, NULL);
        }
        gst_buffer_fill(buffer, buffer_offset, unit->ptr, unit->len);

        // Set timestamps on the buffer
        if (units[i].type == UNIT_FRAME_IDR || units[i].type == UNIT_FRAME_NON_IDR) {
            /* The problems:
               * libuvc capture timestamps are jittery
               * video players skip and duplicate frames if the PTSes are noisy
               * the actual framerate is never precisely equal to the nominal value,
                 and can drift over time
            */

            GstClockTime timestamp;
            GstClockTime duration;
            int64_t offset;

            /* prev_pts / pts_offset_sum / pts_stretch (and base_time, above) are
               shared with set_clock(); keep this read-modify-write under the
               object lock. The buffer fields are written afterwards from locals
               so the buffer alloc/fill and the queue push stay outside it. */
            GST_OBJECT_LOCK(self);

            // We'll set the first PTS to the current timestamp ts. Guard the
            // subtraction: if the first frame arrives less than one interval
            // after the clock baseline, ts - frame_interval would underflow the
            // unsigned baseline into a huge value and poison the first PTS.
            if (self->prev_pts == G_MAXUINT64) {
                self->prev_pts = (ts > (GstClockTime)self->frame_interval)
                                 ? ts - self->frame_interval : 0;
            }

            // Update the PTS calculation on the first IDR after MIN_FRAMES_CALC_INTERVAL frames
            self->frame_count++;
            gboolean update_pts_calc = (units[i].type == UNIT_FRAME_IDR &&
                                        self->frame_count >= MIN_FRAMES_CALC_INTERVAL);

            int64_t timestamp_offset = 0;
            if (update_pts_calc) {
                // Discard the first set of results, as they can be quite noisy
                if (self->prev_int_ts != 0) {
                    #define AVG_DIV 20
                    #define AVG_MULT 1
                    #define AVG_ROUNDING (AVG_DIV/2)

                    #define CLOCK_START_LEN (MIN_FRAMES_CALC_INTERVAL * 3 * (uint64_t)self->frame_interval)
                    #define PTS_JUMP_THRESHOLD (80L * 1000L * 1000L) // 80 ms
                    #define PTS_STRETCH_HYST   (8L * 1000L * 1000L)  //  8 ms
                    #define PTS_STRETCH_VAL    (50L * 1000L)         // 50 us (per frame)


                    // Average frame interval tracking
                    int64_t interval = ((ts - self->prev_int_ts) + self->frame_count / 2) / self->frame_count;
                    self->frame_interval = (self->frame_interval * (AVG_DIV-AVG_MULT) +
                                            interval + AVG_ROUNDING) / AVG_DIV;


                    // Determine if we need to resync the PTSes with the running clock
                    int64_t avg_offset = (self->pts_offset_sum + self->frame_count/2) / self->frame_count;
                    GST_DEBUG_OBJECT(self, "measured frame interval %ld us, average interval %ld us, "
                                           "average PTS offset: %ld us",
                                           interval / 1000, self->frame_interval / 1000, avg_offset / 1000);

                    // Usually we don't need to stretch the frame interval
                    self->pts_stretch = 0;

                    /* After just starting, jump immediately to resync on delta longer than a frame interval.
                       During normal execution, prefer gradual resync as it's less noticeable
                       We've seen delta up to around 75ms caused by dropped frames on a Pocket 3 in 4K60 */
                    if ((ts < CLOCK_START_LEN &&
                        (avg_offset < -self->frame_interval || avg_offset > self->frame_interval)) ||
                        avg_offset < -PTS_JUMP_THRESHOLD || avg_offset > PTS_JUMP_THRESHOLD) {
                        timestamp_offset = avg_offset;
                        GST_DEBUG_OBJECT(self, "  adjusting PTS offset by: %ld us", timestamp_offset / 1000);

                    // For smaller delta of +/- 8ms, slightly stretch or compress frame intervals to catch up
                    } else if (avg_offset > PTS_STRETCH_HYST) {
                        self->pts_stretch = PTS_STRETCH_VAL;
                        GST_DEBUG_OBJECT(self, "  stretching PTS interval by: %ld us", self->pts_stretch / 1000);

                    } else if (avg_offset < -PTS_STRETCH_HYST) {
                        self->pts_stretch = -PTS_STRETCH_VAL;
                        GST_DEBUG_OBJECT(self, "  compressing PTS interval by: %ld us", -self->pts_stretch / 1000);

                    }
                }

                // Reset all the counters regardless of whether the PTS calculations were updated
                self->frame_count = 0;
                self->pts_offset_sum = 0;
                self->prev_int_ts = ts;
            }

            // The interval, stretch and resync offset are signed deltas added
            // to an unsigned PTS. Early in the stream prev_pts is small, so a
            // strongly negative resync offset could drive the sum below zero and
            // wrap the guint64 into a huge timestamp that stalls downstream.
            // Bound the offset so the running PTS can never underflow.
            int64_t pts_base = (int64_t)self->prev_pts + self->frame_interval + self->pts_stretch;
            if (timestamp_offset < -pts_base) {
                timestamp_offset = -pts_base;
            }

            timestamp = self->prev_pts + self->frame_interval + self->pts_stretch + timestamp_offset;

            // Keep PTSes strictly increasing: a backwards or repeated PTS makes
            // players skip or stall, so clamp to at least one tick past prev_pts.
            if (timestamp <= self->prev_pts) {
                timestamp = self->prev_pts + 1;
            }

            offset = ts - timestamp;
            self->pts_offset_sum += offset;
            duration = timestamp - self->prev_pts;
            if (duration == 0) {
                duration = 1;
            }
            self->prev_pts = timestamp;

            GST_OBJECT_UNLOCK(self);

            GST_BUFFER_PTS(buffer) = timestamp;
            GST_BUFFER_DTS(buffer) = timestamp;
            GST_BUFFER_DURATION(buffer) = duration;
            GST_LOG_OBJECT(self, "PTS %lu, offset %ld us", timestamp, offset / 1000);
        }

        // Monotonic frame counter so downstream can detect drops. Only the
        // feeder thread runs frame_callback, so this needs no lock.
        GST_BUFFER_OFFSET(buffer) = self->frame_offset;
        GST_BUFFER_OFFSET_END(buffer) = self->frame_offset + 1;
        self->frame_offset++;

        g_async_queue_push(self->frame_queue, buffer);
    }

    g_free(units);

    if (updated_sps_pps) {
        store_spspps(self);
    }
}
