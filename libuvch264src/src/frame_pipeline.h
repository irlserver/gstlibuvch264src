#ifndef GST_LIBUVC_H264_SRC_FRAME_PIPELINE_H
#define GST_LIBUVC_H264_SRC_FRAME_PIPELINE_H

#include <libuvc/libuvc.h>
#include "gstlibuvch264src.h"

G_BEGIN_DECLS

typedef enum {
    UNIT_INVALID,
    UNIT_FRAME_IDR,
    UNIT_FRAME_NON_IDR,
    UNIT_VPS,
    UNIT_SPS,
    UNIT_PPS,
    /* Access Unit Delimiter (H.264 type 9 / H.265 AUD_NUT 35). By definition the
       first NAL of its access unit, so it is the exact AU boundary marker when a
       device emits it. Appended LAST so the values above keep their numbering. */
    UNIT_AUD,
} nal_unit_type_t;

typedef struct {
    nal_unit_type_t type;
    unsigned char *ptr;
    gsize len;
} nal_unit_t;

nal_unit_type_t convert_unit_type(enum uvc_frame_format format, int type);
int find_nal_unit(enum uvc_frame_format format,
                  unsigned char *buf, gsize buflen, gsize start, int search,
                  gsize *offset, gsize *sc_len);
gsize count_nal_units(enum uvc_frame_format format,
                      unsigned char *buf, gsize buflen);
gsize parse_nal_units(enum uvc_frame_format format,
                      nal_unit_t *units, gsize max, unsigned char *buf, gsize buflen);
gsize split_access_units(enum uvc_frame_format format,
                         const nal_unit_t *units, gsize count,
                         gsize *starts, gsize max_starts);
void frame_callback(uvc_frame_t *frame, void *ptr);

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_FRAME_PIPELINE_H */
