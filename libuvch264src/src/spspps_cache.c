#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gstlibuvch264src_internal.h"
#include "frame_pipeline.h"
#include "spspps_cache.h"
#include "spspps_path.h"

#define DIRBUFLEN 4096
__thread char dir_buf[DIRBUFLEN];
char *get_spspps_path(GstLibuvcH264Src *self, char *index) {
    const char *home_dir = getenv("HOME");
    if (home_dir == NULL) {
        GST_WARNING_OBJECT(self, "HOME environment variable not set.");
        home_dir = "";
    }

    int is_h265 = (self->frame_format == UVC_FRAME_FORMAT_H265);
    int ret = spspps_build_path(dir_buf, DIRBUFLEN, home_dir, index, is_h265,
                                self->negotiated_width, self->negotiated_height);
    if (ret < 0) {
        GST_ERROR_OBJECT(self, "Error building SPS/PPS path");
        return NULL;
    }

    return dir_buf;
}

void create_hidden_directory(GstLibuvcH264Src *self) {
    char *hidden_dir = get_spspps_path(self, NULL);
    if (hidden_dir == NULL) {
        GST_WARNING_OBJECT(self, "SPS/PPS cache directory path unavailable; skipping creation.");
        return;
    }

    struct stat st;
    if (stat(hidden_dir, &st) == -1) {
        if (mkdir(hidden_dir, 0700) != 0)
            GST_ERROR_OBJECT(self, "Error creating directory %s\n", hidden_dir);
        else
            GST_WARNING_OBJECT(self, "Directory %s created successfully.\n", hidden_dir);
    } else if (!S_ISDIR(st.st_mode))
        GST_WARNING_OBJECT(self, "Warning: %s exists but is not a directory.\n", hidden_dir);
}

FILE *open_spspps_file(GstLibuvcH264Src *self, char mode) {
    if (mode == 'w' || mode == 'a') {
        create_hidden_directory(self);
    }

    char m[3];
    sprintf(m, "%cb", mode);
    char *file_name = get_spspps_path(self, self->index);
    if (file_name == NULL) {
        GST_WARNING_OBJECT(self, "SPS/PPS cache path unavailable; skipping cache I/O.");
        return NULL;
    }
    FILE *fp = fopen(file_name, m);
    return fp;
}

// Must only be called after the caps have been negotiated and the format is known
void load_spspps(GstLibuvcH264Src *self) {
    FILE* fp = open_spspps_file(self, 'r');
    if (fp) {
        unsigned char buf[SPSPPSBUFSZ*3];
        gsize read_bytes = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);

        #define MAX_UNITS_LOAD 3
        nal_unit_t units[MAX_UNITS_LOAD];
        gsize c = parse_nal_units(self->frame_format, units, MAX_UNITS_LOAD, buf, read_bytes);

        for (gsize i = 0; i < c; i++) {
            switch (units[i].type) {
                case UNIT_VPS:
                    if (units[i].len == 0 || units[i].len > SPSPPSBUFSZ) {
                        GST_WARNING_OBJECT(self, "Dropping oversized/invalid cached VPS NAL "
                            "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                            units[i].len, SPSPPSBUFSZ);
                        break;
                    }
                    memcpy(self->vps, units[i].ptr, units[i].len);
                    self->vps_length = units[i].len;
                    break;
                case UNIT_SPS:
                    if (units[i].len == 0 || units[i].len > SPSPPSBUFSZ) {
                        GST_WARNING_OBJECT(self, "Dropping oversized/invalid cached SPS NAL "
                            "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                            units[i].len, SPSPPSBUFSZ);
                        break;
                    }
                    memcpy(self->sps, units[i].ptr, units[i].len);
                    self->sps_length = units[i].len;
                    break;
                case UNIT_PPS:
                    if (units[i].len == 0 || units[i].len > SPSPPSBUFSZ) {
                        GST_WARNING_OBJECT(self, "Dropping oversized/invalid cached PPS NAL "
                            "(%" G_GSIZE_FORMAT " bytes; max %d) to prevent heap overflow",
                            units[i].len, SPSPPSBUFSZ);
                        break;
                    }
                    memcpy(self->pps, units[i].ptr, units[i].len);
                    self->pps_length = units[i].len;
                    break;
                default:
                    // We shouldn't have other types; but ignore them if we do
                    break;
            }
        }
    }
}

void store_spspps(GstLibuvcH264Src *self) {
    FILE* fp = open_spspps_file(self, 'w');
	if (fp) {
		if (self->frame_format == UVC_FRAME_FORMAT_H265) {
			fwrite(self->vps, 1, self->vps_length, fp);
		}
		fwrite(self->sps, 1, self->sps_length, fp);
		fwrite(self->pps, 1, self->pps_length, fp);
		fclose(fp);
	}
}
