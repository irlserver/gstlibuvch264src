#ifndef GST_LIBUVC_H264_SRC_SPSPPS_CACHE_H
#define GST_LIBUVC_H264_SRC_SPSPPS_CACHE_H

#include <stdio.h>
#include "gstlibuvch264src.h"

G_BEGIN_DECLS

char *get_spspps_path(GstLibuvcH264Src *self, char *index);
void create_hidden_directory(GstLibuvcH264Src *self);
FILE *open_spspps_file(GstLibuvcH264Src *self, char mode);
void load_spspps(GstLibuvcH264Src *self);
void store_spspps(GstLibuvcH264Src *self);

G_END_DECLS

#endif /* GST_LIBUVC_H264_SRC_SPSPPS_CACHE_H */
