/* Clip Studio Paint project files (.clip): the CSFCHUNK container, its embedded SQLite database and the
 * layers' tile streams, read into straight-alpha RGBA8 and 8-bit masks. A C library with no dependency
 * beyond SQLite (3.24+, for sqlite3_deserialize) and zlib; csp_clip_import.c. clip.cpp builds a Document
 * from it. */
#ifndef CSP_CLIP_IMPORT_H
#define CSP_CLIP_IMPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CSP_CLIP_OK = 0,
    CSP_CLIP_ERR_ARGUMENT = -1,
    CSP_CLIP_ERR_IO = -2,
    CSP_CLIP_ERR_FORMAT = -3,
    CSP_CLIP_ERR_MEMORY = -4,
    CSP_CLIP_ERR_SQLITE = -5,
    CSP_CLIP_ERR_NOT_FOUND = -6,
    CSP_CLIP_ERR_UNSUPPORTED = -7,
    CSP_CLIP_ERR_DECODE = -8,
    CSP_CLIP_ERR_LIMIT = -9
};

typedef enum csp_clip_layer_kind {
    CSP_CLIP_LAYER_UNKNOWN = 0,
    CSP_CLIP_LAYER_RASTER,
    CSP_CLIP_LAYER_GROUP,
    CSP_CLIP_LAYER_VECTOR,
    CSP_CLIP_LAYER_TEXT,
    CSP_CLIP_LAYER_FILL,
    CSP_CLIP_LAYER_CORRECTION,
    CSP_CLIP_LAYER_PAPER,
    CSP_CLIP_LAYER_ROOT,
    CSP_CLIP_LAYER_OTHER
} csp_clip_layer_kind;

/* Limits on what a file may make the reader allocate; csp_clip_default_options fills sensible ones. */
typedef struct csp_clip_options {
    uint64_t max_sqlite_bytes;
    size_t max_external_objects;
    uint64_t max_pixels;
    size_t max_tile_bytes;
    int include_root_layer;
} csp_clip_options;

typedef struct csp_clip_document_info {
    int64_t canvas_id;
    int width, height;
    double dpi;
    int64_t root_layer_id;
    int channel_bytes;
    size_t external_object_count;
} csp_clip_document_info;

/* One layer, in the document's order: each group is followed by its children, bottom of the stack first. */
typedef struct csp_clip_layer {
    const char *name, *uuid;
    int64_t id;
    csp_clip_layer_kind kind;
    double opacity;                 /* 0..1 (CSP stores 0..256) */
    int visible, mask_enabled, locked, clipped, is_group;
    int parent_index;               /* -1 at the top level */
    int depth;
    int raw_type, raw_composite, raw_visibility, raw_folder, raw_lock, raw_clip, raw_special_render_type;
    int64_t render_mipmap_id, mask_mipmap_id;
    int render_offset_x, render_offset_y, mask_offset_x, mask_offset_y;
} csp_clip_layer;

typedef struct csp_clip_document csp_clip_document;

void csp_clip_default_options(csp_clip_options *options);
int csp_clip_open(const char *path, const csp_clip_options *options, csp_clip_document **out_document,
                  char *error, size_t error_capacity);
void csp_clip_close(csp_clip_document *document);

const csp_clip_document_info *csp_clip_get_info(const csp_clip_document *document);
size_t csp_clip_get_layer_count(const csp_clip_document *document);
const csp_clip_layer *csp_clip_get_layer(const csp_clip_document *document, size_t index);

/* A layer's pixels as straight-alpha RGBA8 (free with csp_clip_free), placed at (x, y) on the canvas. */
int csp_clip_decode_layer_rgba(const csp_clip_document *document, size_t layer_index,
                               uint8_t **out_rgba, int *out_width, int *out_height,
                               size_t *out_stride, int *out_x, int *out_y,
                               char *error, size_t error_capacity);
/* A layer's mask as 8-bit gray (free with csp_clip_free), placed at (x, y) on the canvas. */
int csp_clip_decode_layer_mask_gray8(const csp_clip_document *document, size_t layer_index,
                                     uint8_t **out_gray, int *out_width, int *out_height,
                                     size_t *out_stride, int *out_x, int *out_y,
                                     char *error, size_t error_capacity);
/* The canvas preview image as stored (PNG data), and its size. */
int csp_clip_read_preview(const csp_clip_document *document, uint8_t **out_data, size_t *out_size,
                          int *out_width, int *out_height, char *error, size_t error_capacity);
int csp_clip_read_blob_by_main_id(const csp_clip_document *document, const char *table, const char *column,
                                  int64_t main_id, uint8_t **out_data, size_t *out_size,
                                  char *error, size_t error_capacity);

const char *csp_clip_layer_kind_name(csp_clip_layer_kind kind);
const char *csp_clip_blend_mode_name(int value);
const char *csp_clip_result_string(int result);
void csp_clip_free(void *memory);

#ifdef __cplusplus
}
#endif

#endif
