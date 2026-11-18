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
            case 9:
                return UNIT_AUD;
      }

    } else if (format == UVC_FRAME_FORMAT_H265) {
        /* Every VCL type ITU-T H.265 Table 7-1 defines is a slice of a coded
           picture: 0 to 9 (trailing, TSA, STSA, RADL, RASL) and 16 to 21 (BLA,
           IDR, CRA). Both IDR types (19 IDR_W_RADL, the one most hardware
           encoders emit, and 20 IDR_N_LP) start a stream that decodes on its
           own. The reserved types stay invalid. */
        if ((type >= 0 && type <= 9) || (type >= 16 && type <= 21)) {
            return (type == 19 || type == 20) ? UNIT_FRAME_IDR : UNIT_FRAME_NON_IDR;
        }
        switch (type) {
            case 32:
                return UNIT_VPS;
            case 33:
                return UNIT_SPS;
            case 34:
                return UNIT_PPS;
            case 35:  /* AUD_NUT */
                return UNIT_AUD;
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

static gboolean nal_is_parameter_set(nal_unit_type_t type) {
    return type == UNIT_VPS || type == UNIT_SPS || type == UNIT_PPS;
}

static gboolean nal_is_slice(nal_unit_type_t type) {
    return type == UNIT_FRAME_IDR || type == UNIT_FRAME_NON_IDR;
}

/* Offset of the NAL header byte inside a parsed unit. parse_nal_units() always
 * puts unit->ptr on the first byte of the Annex-B start code, so the header sits
 * just past it: 3 bytes in for 00 00 01, 4 for 00 00 00 01. */
static gsize nal_header_offset(const nal_unit_t *unit) {
    return (unit->len >= 3 && unit->ptr[2] == 0x01) ? 3 : 4;
}

/* TRUE when this slice NAL opens a new picture - the AU boundary signal used
 * when the device emits no Access Unit Delimiter.
 *
 * H.264 slice headers start with first_mb_in_slice, H.265 slice segment headers
 * with first_slice_segment_in_pic_flag; either way the "this is the first slice"
 * condition is carried by the very first bit of the slice payload. An exp-Golomb
 * ue(v) of 0 is coded as the single bit `1`, and the H.265 flag is a raw u(1), so
 * both reduce to "the top bit of the first payload byte is set" - no bit reader
 * needed. Emulation prevention cannot disturb that byte either: a 0x03 is only
 * inserted after two 0x00 bytes, and the NAL header immediately preceding it is
 * non-zero for every slice NAL. */
static gboolean nal_opens_new_picture(enum uvc_frame_format format,
                                      const nal_unit_t *unit) {
    gsize payload = nal_header_offset(unit) +
                    ((format == UVC_FRAME_FORMAT_H265) ? 2 : 1);
    if (unit->len <= payload) return FALSE;
    return (unit->ptr[payload] & 0x80) != 0;
}

/* Partition `units` into access units, writing the index of each AU's first unit
 * into `starts` and returning the number of AUs found. AU k spans
 * [starts[k], starts[k+1]), the last one running to `count`.
 *
 * Two boundary signals, in priority order:
 *   AUD present  the delimiter IS the first NAL of its access unit, so it is an
 *                exact boundary - no heuristic involved.
 *   AUD absent   the standard fallback: a slice that opens a new picture ends the
 *                AU that already holds one. Any non-VCL run immediately before
 *                that slice (SEI, parameter sets) belongs to the NEW access unit,
 *                so the cut is placed at the head of that run, not at the slice.
 *
 * A device that emits neither an AUD nor a decodable first-slice bit simply never
 * splits, which degrades to the whole delivery as one AU - the same grouping a
 * single-picture frame gets, and never a mid-picture cut. */
gsize split_access_units(enum uvc_frame_format format,
                         const nal_unit_t *units, gsize count,
                         gsize *starts, gsize max_starts) {
    gsize n = 0;
    gboolean au_has_slice = FALSE;
    gsize nonvcl_run = G_MAXSIZE;   /* first non-VCL NAL after the last slice */

    for (gsize i = 0; i < count; i++) {
        gboolean is_slice = nal_is_slice(units[i].type);
        gboolean boundary = TRUE;
        gsize at = i;

        if (n == 0) {
            at = 0;
        } else if (units[i].type == UNIT_AUD) {
            at = i;
        } else if (is_slice && au_has_slice && nal_opens_new_picture(format, &units[i])) {
            at = (nonvcl_run != G_MAXSIZE) ? nonvcl_run : i;
        } else {
            boundary = FALSE;
        }

        if (boundary) {
            if (n >= max_starts) {
                GST_WARNING("Access-unit count exceeds max=%" G_GSIZE_FORMAT
                            "; stored %" G_GSIZE_FORMAT " and merged the remainder "
                            "into the last one. Size the array with the NAL count.",
                            max_starts, n);
                break;
            }
            starts[n++] = at;
            au_has_slice = FALSE;
        }

        if (is_slice) {
            au_has_slice = TRUE;
            nonvcl_run = G_MAXSIZE;
        } else if (nonvcl_run == G_MAXSIZE) {
            nonvcl_run = i;
        }
    }

    return n;
}

/* Latch one parameter set into the element's cache.
 *
 * An oversized or empty set is refused outright (heap-overflow guard against the
 * fixed SPSPPSBUFSZ arrays), and the cached copy is rewritten only when it
 * actually CHANGED (L10): SPS/PPS/VPS repeat before every IDR, so an
 * unconditional store rewrites the disk cache each GOP and wears the flash for
 * nothing. send_sps_pps latches on every set regardless, so the parameter sets
 * are always re-prepended in band even when the cache write is skipped. */
static void cache_parameter_set(GstLibuvcH264Src *self, const nal_unit_t *unit,
                                gboolean *updated) {
    unsigned char *dst;
    gint *dst_len;
    const char *label;

    switch (unit->type) {
        case UNIT_VPS: dst = self->vps; dst_len = &self->vps_length; label = "VPS"; break;
        case UNIT_SPS: dst = self->sps; dst_len = &self->sps_length; label = "SPS"; break;
        case UNIT_PPS: dst = self->pps; dst_len = &self->pps_length; label = "PPS"; break;
        default: return;
    }

    if (unit->len == 0 || unit->len > SPSPPSBUFSZ) {
        GST_WARNING_OBJECT(self, "Dropping oversized/invalid %s NAL (%" G_GSIZE_FORMAT
            " bytes; max %d) to prevent heap overflow", label, unit->len, SPSPPSBUFSZ);
        return;
    }

    if ((gsize)*dst_len != unit->len || memcmp(dst, unit->ptr, unit->len) != 0) {
        *dst_len = (gint)unit->len;
        memcpy(dst, unit->ptr, unit->len);
        *updated = TRUE;
    }
    self->send_sps_pps = TRUE;
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

    /* Proof of life for the silence watchdog in create(), which otherwise only
       sees queued buffers: warmup drops and parameter-set-only deliveries
       queue nothing from a camera that is working. */
    g_atomic_int_inc(&self->deliveries);

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

    /* ONE GstBuffer per ACCESS UNIT, never per NAL. The pad template advertises
       alignment=au, and every downstream consumer that trusts it (h264parse, the
       V4L2/MPP decoders) mis-frames a picture whose slices arrive as separate
       buffers. Single-slice 1080p is unaffected - its access unit is one slice,
       so the emitted bytes are identical to the per-NAL path - but a multi-slice
       or 4K picture used to be split across a dozen buffers all claiming to be
       whole access units. */
    gsize *au_starts = g_new(gsize, c ? c : 1);
    gsize au_count = split_access_units(self->frame_format, units, c, au_starts, c);

    for (gsize a = 0; a < au_count; a++) {
        gsize first = au_starts[a];
        gsize end = (a + 1 < au_count) ? au_starts[a + 1] : c;

        /* Consume this AU's parameter sets into the cache and measure what is
           left to forward. Parameter sets are never forwarded from the wire; the
           cached copy is re-prepended in front of the AU's first IDR below. */
        gboolean has_idr = FALSE;
        gboolean has_slice = FALSE;
        gsize payload_len = 0;
        for (gsize i = first; i < end; i++) {
            nal_unit_t *unit = &units[i];
            if (nal_is_parameter_set(unit->type)) {
                cache_parameter_set(self, unit, &updated_sps_pps);
                continue;
            }
            if (unit->type == UNIT_FRAME_IDR) has_idr = TRUE;
            if (nal_is_slice(unit->type)) has_slice = TRUE;
            payload_len += unit->len;
        }

        /* warmup-frames: discard the camera's first pictures before the
           keyframe gate below, so had_idr stays FALSE and the first picture
           forwarded afterwards is an IDR with the cached parameter sets. */
        if (has_slice && self->warmup_remaining > 0) {
            self->warmup_remaining--;
            continue;
        }

        /* Drop everything ahead of the first keyframe so downstream never sees an
           undecodable leading fragment, and drop an AU that carried nothing but
           parameter sets. */
        if ((!self->had_idr && !has_idr) || payload_len == 0) {
            continue;
        }

        gsize prefix_len = 0;
        if (has_idr && (!self->had_idr || self->send_sps_pps)) {
            prefix_len = (gsize)self->sps_length + (gsize)self->pps_length;
            if (self->frame_format == UVC_FRAME_FORMAT_H265) {
                prefix_len += (gsize)self->vps_length;
            }
        }

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, prefix_len + payload_len, NULL);
        gsize at = 0;
        gboolean prefixed = FALSE;
        for (gsize i = first; i < end; i++) {
            nal_unit_t *unit = &units[i];
            if (nal_is_parameter_set(unit->type)) continue;

            /* The cached parameter sets go immediately BEFORE the AU's first IDR
               slice, never at the head of the buffer: an AUD, when present, must
               remain the very first NAL of its access unit. */
            if (!prefixed && prefix_len > 0 && unit->type == UNIT_FRAME_IDR) {
                if (self->frame_format == UVC_FRAME_FORMAT_H265) {
                    gst_buffer_fill(buffer, at, self->vps, self->vps_length);
                    at += self->vps_length;
                }
                gst_buffer_fill(buffer, at, self->sps, self->sps_length);
                at += self->sps_length;
                gst_buffer_fill(buffer, at, self->pps, self->pps_length);
                at += self->pps_length;
                prefixed = TRUE;
                self->send_sps_pps = FALSE;
            }
            gst_buffer_fill(buffer, at, unit->ptr, unit->len);
            at += unit->len;
        }
        if (has_idr) {
            self->had_idr = TRUE;
        }

        // Set timestamps on the buffer
        if (has_slice) {
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
            gboolean update_pts_calc = (has_idr &&
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

        // Monotonic access-unit counter so downstream can detect drops. Only the
        // feeder thread runs frame_callback, so this needs no lock.
        GST_BUFFER_OFFSET(buffer) = self->frame_offset;
        GST_BUFFER_OFFSET_END(buffer) = self->frame_offset + 1;
        self->frame_offset++;

        g_async_queue_push(self->frame_queue, buffer);
    }

    g_free(au_starts);
    g_free(units);

    if (updated_sps_pps) {
        store_spspps(self);
    }
}
