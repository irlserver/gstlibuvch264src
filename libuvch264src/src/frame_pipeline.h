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
void frame_callback(uvc_frame_t *frame, void *ptr);

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_FRAME_PIPELINE_H */
