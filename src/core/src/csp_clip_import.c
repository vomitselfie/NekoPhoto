/* Clip Studio Paint project files (.clip): the CSFCHUNK container, its embedded SQLite database and the
 * layers' zlib tile streams. Written for this project as a self-contained C reader; changes here:
 * a layer's placement adds LayerOffset to LayerRenderOffscrOffset (checked against Clip Studio's own PSD
 * exports), and only a type-256 layer is the canvas root (folders also carry LayerFolder bit 1). */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "csp_clip_import.h"

#include <sqlite3.h>
#include <zlib.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_DEFAULT_MAX_SQLITE (256u * 1024u * 1024u)
#define CC_DEFAULT_MAX_EXTERNAL_OBJECTS 500000u
#define CC_DEFAULT_MAX_PIXELS 268435456ull
#define CC_DEFAULT_MAX_TILE_BYTES (32u * 1024u * 1024u)
#define CC_MAX_TREE_DEPTH 256
#define CC_MAX_CHUNK_ID 4096u

/* 8-byte outer tags. */
#define CC_TAG_HEAD "CHNKHead"
#define CC_TAG_EXTA "CHNKExta"
#define CC_TAG_SQLI "CHNKSQLi"
#define CC_TAG_FOOT "CHNKFoot"

typedef struct cc_external {
    char *id;
    size_t id_len;
    uint64_t body_offset;
    uint64_t body_length;
} cc_external;

typedef struct cc_raw_layer {
    char *name;
    char *uuid;

    int64_t id;
    int64_t first_child;
    int64_t next_sibling;
    int64_t render_mipmap;
    int64_t mask_mipmap;

    int raw_type;
    int raw_composite;
    int raw_opacity;
    int raw_visibility;
    int raw_folder;
    int raw_lock;
    int raw_clip;
    int special_render_type;

    /* Where a layer's offscreen bitmap sits on the canvas: the layer's own offset (how far it was moved)
     * plus the offscreen's offset from it. The offscreen grows by whole tiles, so after a move past the
     * canvas edge its origin is off the canvas (a 4608-wide bitmap at x = -256, say). */
    int render_offset_x;
    int render_offset_y;
    int mask_offset_x;
    int mask_offset_y;

    int has_vector_type;
    int has_text_type;
    int has_gradation_fill;
    int has_filter_info;
} cc_raw_layer;

typedef struct cc_idmap {
    int64_t id;
    size_t index;
} cc_idmap;

typedef struct cc_offscreen_attr {
    uint32_t width;
    uint32_t height;
    uint32_t grid_width;
    uint32_t grid_height;
    uint32_t values[16];
    uint32_t default_fill;
    uint32_t init_color[4];
    int has_init_color;
    uint32_t first_channels;
    uint32_t second_channels;
    int bit_packed;
} cc_offscreen_attr;

struct csp_clip_document {
    char *path;
    csp_clip_options options;
    csp_clip_document_info info;

    sqlite3 *db;
    unsigned char *sqlite_bytes;
    size_t sqlite_size;

    cc_external *externals;
    size_t external_count;
    size_t external_capacity;

    cc_raw_layer *raw_layers;
    size_t raw_layer_count;

    cc_idmap *idmap;

    csp_clip_layer *layers;
    size_t layer_count;
    size_t layer_capacity;
};

static void cc_set_error(char *error, size_t capacity, const char *fmt, ...)
{
    va_list ap;
    if (!error || capacity == 0) return;
    va_start(ap, fmt);
    (void)vsnprintf(error, capacity, fmt, ap);
    va_end(ap);
    error[capacity - 1] = '\0';
}

static void cc_clear_error(char *error, size_t capacity)
{
    if (error && capacity) error[0] = '\0';
}

static char *cc_strdup(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int cc_mul_size(size_t a, size_t b, size_t *out)
{
    if (!out) return 0;
    if (a != 0 && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static uint16_t cc_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t cc_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint32_t cc_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t cc_be64(const uint8_t *p)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

static int cc_read_exact(FILE *f, void *dst, size_t n)
{
    return n == 0 || fread(dst, 1, n, f) == n;
}

static int cc_seek(FILE *f, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX) return 0;
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
}

static int cc_get_file_size(FILE *f, uint64_t *out)
{
    off_t end;
    if (!f || !out) return 0;
    if (fseeko(f, 0, SEEK_END) != 0) return 0;
    end = ftello(f);
    if (end < 0) return 0;
    *out = (uint64_t)end;
    return fseeko(f, 0, SEEK_SET) == 0;
}

static int cc_ident_ok(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p || !*p) return 0;
    while (*p) {
        if (!(isalnum(*p) || *p == '_')) return 0;
        ++p;
    }
    return 1;
}

static int cc_stmt_col(sqlite3_stmt *st, const char *name)
{
    int i, n;
    if (!st || !name) return -1;
    n = sqlite3_column_count(st);
    for (i = 0; i < n; ++i) {
        const char *cn = sqlite3_column_name(st, i);
        if (cn && strcmp(cn, name) == 0) return i;
    }
    return -1;
}

static int64_t cc_col_i64(sqlite3_stmt *st, int col, int64_t fallback)
{
    if (col < 0 || sqlite3_column_type(st, col) == SQLITE_NULL) return fallback;
    return sqlite3_column_int64(st, col);
}

static int cc_col_int(sqlite3_stmt *st, int col, int fallback)
{
    if (col < 0 || sqlite3_column_type(st, col) == SQLITE_NULL) return fallback;
    return sqlite3_column_int(st, col);
}

static double cc_col_double(sqlite3_stmt *st, int col, double fallback)
{
    if (col < 0 || sqlite3_column_type(st, col) == SQLITE_NULL) return fallback;
    return sqlite3_column_double(st, col);
}

static char *cc_col_strdup(sqlite3_stmt *st, int col)
{
    const unsigned char *s;
    if (col < 0 || sqlite3_column_type(st, col) == SQLITE_NULL) return cc_strdup("");
    s = sqlite3_column_text(st, col);
    return cc_strdup(s ? (const char *)s : "");
}

static int cc_col_has_value(sqlite3_stmt *st, int col)
{
    return col >= 0 && sqlite3_column_type(st, col) != SQLITE_NULL;
}

static int cc_flex_layer_type(sqlite3_stmt *st, int col)
{
    const char *s;
    if (col < 0 || sqlite3_column_type(st, col) == SQLITE_NULL) return 0;
    if (sqlite3_column_type(st, col) != SQLITE_TEXT) return sqlite3_column_int(st, col);
    s = (const char *)sqlite3_column_text(st, col);
    if (!s) return 0;
    if (strcmp(s, "lt_bitmap") == 0 || strcmp(s, "bitmap") == 0) return 1;
    if (strcmp(s, "lt_root") == 0 || strcmp(s, "root") == 0) return 256;
    if (strcmp(s, "lt_paper") == 0 || strcmp(s, "paper") == 0) return 1584;
    return 0;
}

void csp_clip_default_options(csp_clip_options *options)
{
    if (!options) return;
    options->max_sqlite_bytes = CC_DEFAULT_MAX_SQLITE;
    options->max_external_objects = CC_DEFAULT_MAX_EXTERNAL_OBJECTS;
    options->max_pixels = CC_DEFAULT_MAX_PIXELS;
    options->max_tile_bytes = CC_DEFAULT_MAX_TILE_BYTES;
    options->include_root_layer = 0;
}

static int cc_push_external(csp_clip_document *doc, const char *id, size_t id_len,
                            uint64_t body_offset, uint64_t body_length,
                            char *error, size_t error_capacity)
{
    cc_external *tmp;
    char *copy;
    size_t newcap;

    if (doc->external_count >= doc->options.max_external_objects) {
        cc_set_error(error, error_capacity, "too many external objects");
        return CSP_CLIP_ERR_LIMIT;
    }
    if (doc->external_count == doc->external_capacity) {
        newcap = doc->external_capacity ? doc->external_capacity * 2 : 64;
        if (newcap > doc->options.max_external_objects) newcap = doc->options.max_external_objects;
        tmp = (cc_external *)realloc(doc->externals, newcap * sizeof(*tmp));
        if (!tmp) {
            cc_set_error(error, error_capacity, "out of memory growing external-object table");
            return CSP_CLIP_ERR_MEMORY;
        }
        doc->externals = tmp;
        doc->external_capacity = newcap;
    }
    copy = (char *)malloc(id_len + 1);
    if (!copy) {
        cc_set_error(error, error_capacity, "out of memory copying external id");
        return CSP_CLIP_ERR_MEMORY;
    }
    memcpy(copy, id, id_len);
    copy[id_len] = '\0';

    doc->externals[doc->external_count].id = copy;
    doc->externals[doc->external_count].id_len = id_len;
    doc->externals[doc->external_count].body_offset = body_offset;
    doc->externals[doc->external_count].body_length = body_length;
    doc->external_count++;
    return CSP_CLIP_OK;
}

static int cc_parse_container(csp_clip_document *doc, char *error, size_t error_capacity)
{
    FILE *f = NULL;
    uint8_t hdr[24];
    uint64_t file_size = 0;
    uint64_t pos;
    int found_sqlite = 0;
    int result = CSP_CLIP_OK;

    f = fopen(doc->path, "rb");
    if (!f) {
        cc_set_error(error, error_capacity, "cannot open '%s': %s", doc->path, strerror(errno));
        return CSP_CLIP_ERR_IO;
    }
    if (!cc_get_file_size(f, &file_size) || file_size < sizeof(hdr) ||
        !cc_read_exact(f, hdr, sizeof(hdr))) {
        cc_set_error(error, error_capacity, "file is too small or unreadable");
        result = CSP_CLIP_ERR_IO;
        goto done;
    }
    if (memcmp(hdr, "CSFCHUNK", 8) != 0) {
        cc_set_error(error, error_capacity, "not a CLIP STUDIO CSFCHUNK file");
        result = CSP_CLIP_ERR_FORMAT;
        goto done;
    }

    pos = cc_be64(hdr + 16);
    if (pos < 24 || pos > file_size - 16) pos = 24;

    while (pos + 16 <= file_size) {
        uint8_t ch[16];
        char tag[9];
        uint64_t payload_len, payload_off, payload_end;

        if (!cc_seek(f, pos) || !cc_read_exact(f, ch, sizeof(ch))) {
            cc_set_error(error, error_capacity, "failed reading chunk header at offset %llu",
                         (unsigned long long)pos);
            result = CSP_CLIP_ERR_IO;
            goto done;
        }
        memcpy(tag, ch, 8);
        tag[8] = '\0';
        payload_len = cc_be64(ch + 8);
        payload_off = pos + 16;
        if (payload_len > file_size - payload_off) {
            cc_set_error(error, error_capacity, "chunk %.8s exceeds file bounds", tag);
            result = CSP_CLIP_ERR_FORMAT;
            goto done;
        }
        payload_end = payload_off + payload_len;

        if (memcmp(tag, CC_TAG_EXTA, 8) == 0) {
            uint8_t b8[8];
            uint64_t id_len, body_len, body_off;
            char *id = NULL;

            if (payload_len < 16 || !cc_seek(f, payload_off) || !cc_read_exact(f, b8, 8)) {
                cc_set_error(error, error_capacity, "truncated CHNKExta header");
                result = CSP_CLIP_ERR_FORMAT;
                goto done;
            }
            id_len = cc_be64(b8);
            if (id_len == 0 || id_len > CC_MAX_CHUNK_ID || id_len > payload_len - 16) {
                cc_set_error(error, error_capacity, "invalid CHNKExta identifier length");
                result = CSP_CLIP_ERR_FORMAT;
                goto done;
            }
            id = (char *)malloc((size_t)id_len);
            if (!id) {
                result = CSP_CLIP_ERR_MEMORY;
                cc_set_error(error, error_capacity, "out of memory reading external id");
                goto done;
            }
            if (!cc_read_exact(f, id, (size_t)id_len) || !cc_read_exact(f, b8, 8)) {
                free(id);
                cc_set_error(error, error_capacity, "truncated CHNKExta payload");
                result = CSP_CLIP_ERR_FORMAT;
                goto done;
            }
            body_len = cc_be64(b8);
            body_off = payload_off + 8 + id_len + 8;
            if (body_len > payload_end - body_off) {
                free(id);
                cc_set_error(error, error_capacity, "external body exceeds CHNKExta payload");
                result = CSP_CLIP_ERR_FORMAT;
                goto done;
            }
            result = cc_push_external(doc, id, (size_t)id_len, body_off, body_len,
                                      error, error_capacity);
            free(id);
            if (result != CSP_CLIP_OK) goto done;
        } else if (memcmp(tag, CC_TAG_SQLI, 8) == 0 && !found_sqlite) {
            if (payload_len < 16 || payload_len > doc->options.max_sqlite_bytes || payload_len > SIZE_MAX) {
                cc_set_error(error, error_capacity, "embedded SQLite database size is invalid or exceeds limit");
                result = payload_len > doc->options.max_sqlite_bytes ? CSP_CLIP_ERR_LIMIT : CSP_CLIP_ERR_FORMAT;
                goto done;
            }
            doc->sqlite_bytes = (unsigned char *)sqlite3_malloc64((sqlite3_uint64)payload_len);
            if (!doc->sqlite_bytes) {
                cc_set_error(error, error_capacity, "out of memory reading embedded SQLite database");
                result = CSP_CLIP_ERR_MEMORY;
                goto done;
            }
            if (!cc_seek(f, payload_off) || !cc_read_exact(f, doc->sqlite_bytes, (size_t)payload_len)) {
                cc_set_error(error, error_capacity, "failed reading embedded SQLite database");
                result = CSP_CLIP_ERR_IO;
                goto done;
            }
            if (memcmp(doc->sqlite_bytes, "SQLite format 3\0", 16) != 0) {
                cc_set_error(error, error_capacity, "CHNKSQLi does not contain a SQLite database");
                result = CSP_CLIP_ERR_FORMAT;
                goto done;
            }
            doc->sqlite_size = (size_t)payload_len;
            found_sqlite = 1;
        }

        if (payload_end <= pos) {
            cc_set_error(error, error_capacity, "invalid chunk length overflow");
            result = CSP_CLIP_ERR_FORMAT;
            goto done;
        }
        pos = payload_end;
        if (memcmp(tag, CC_TAG_FOOT, 8) == 0) break;
    }

    if (!found_sqlite) {
        cc_set_error(error, error_capacity, "CLIP file contains no CHNKSQLi database");
        result = CSP_CLIP_ERR_NOT_FOUND;
    }

done:
    fclose(f);
    return result;
}

static int cc_open_sqlite(csp_clip_document *doc, char *error, size_t error_capacity)
{
    int rc;
    rc = sqlite3_open_v2(":memory:", &doc->db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        cc_set_error(error, error_capacity, "sqlite open failed: %s",
                     doc->db ? sqlite3_errmsg(doc->db) : "unknown error");
        return CSP_CLIP_ERR_SQLITE;
    }
#if defined(SQLITE_VERSION_NUMBER) && SQLITE_VERSION_NUMBER >= 3024000
    rc = sqlite3_deserialize(doc->db, "main", doc->sqlite_bytes,
                             (sqlite3_int64)doc->sqlite_size,
                             (sqlite3_int64)doc->sqlite_size,
                             SQLITE_DESERIALIZE_READONLY);
    if (rc != SQLITE_OK) {
        cc_set_error(error, error_capacity, "sqlite deserialize failed: %s", sqlite3_errmsg(doc->db));
        return CSP_CLIP_ERR_SQLITE;
    }
#else
    (void)doc;
    cc_set_error(error, error_capacity, "SQLite 3.24+ with sqlite3_deserialize() is required");
    return CSP_CLIP_ERR_UNSUPPORTED;
#endif
    return CSP_CLIP_OK;
}

static int cc_load_canvas(csp_clip_document *doc, char *error, size_t error_capacity)
{
    sqlite3_stmt *st = NULL;
    int rc;
    int c_id, c_w, c_h, c_dpi, c_root, c_chbytes;

    rc = sqlite3_prepare_v2(doc->db, "SELECT * FROM Canvas LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        cc_set_error(error, error_capacity, "cannot query Canvas: %s", sqlite3_errmsg(doc->db));
        return CSP_CLIP_ERR_SQLITE;
    }
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        cc_set_error(error, error_capacity, "Canvas table is empty or unreadable");
        sqlite3_finalize(st);
        return rc == SQLITE_DONE ? CSP_CLIP_ERR_NOT_FOUND : CSP_CLIP_ERR_SQLITE;
    }

    c_id = cc_stmt_col(st, "MainId");
    c_w = cc_stmt_col(st, "CanvasWidth");
    c_h = cc_stmt_col(st, "CanvasHeight");
    c_dpi = cc_stmt_col(st, "CanvasResolution");
    c_root = cc_stmt_col(st, "CanvasRootFolder");
    c_chbytes = cc_stmt_col(st, "CanvasChannelBytes");

    doc->info.canvas_id = cc_col_i64(st, c_id, 0);
    doc->info.width = cc_col_int(st, c_w, 0);
    doc->info.height = cc_col_int(st, c_h, 0);
    doc->info.dpi = cc_col_double(st, c_dpi, 72.0);
    doc->info.root_layer_id = cc_col_i64(st, c_root, 0);
    doc->info.channel_bytes = cc_col_int(st, c_chbytes, 1);

    sqlite3_finalize(st);

    if (doc->info.width <= 0 || doc->info.height <= 0) {
        cc_set_error(error, error_capacity, "invalid Canvas dimensions %dx%d",
                     doc->info.width, doc->info.height);
        return CSP_CLIP_ERR_FORMAT;
    }
    if ((uint64_t)doc->info.width * (uint64_t)doc->info.height > doc->options.max_pixels) {
        cc_set_error(error, error_capacity, "Canvas exceeds configured pixel limit");
        return CSP_CLIP_ERR_LIMIT;
    }
    return CSP_CLIP_OK;
}

static csp_clip_layer_kind cc_classify_layer(const cc_raw_layer *r)
{
    if (!r) return CSP_CLIP_LAYER_UNKNOWN;
    if (r->raw_type == 256) return CSP_CLIP_LAYER_ROOT;
    if ((r->raw_folder & 1) != 0) return CSP_CLIP_LAYER_GROUP;
    if (r->has_vector_type) return CSP_CLIP_LAYER_VECTOR;
    if (r->has_text_type) return CSP_CLIP_LAYER_TEXT;
    if (r->special_render_type == 13 || r->raw_type == 4098) return CSP_CLIP_LAYER_CORRECTION;
    if (r->special_render_type == 20 || r->raw_type == 1584) return CSP_CLIP_LAYER_PAPER;
    if (r->raw_type == 2 || r->has_gradation_fill) return CSP_CLIP_LAYER_FILL;
    if (r->raw_type == 1) return CSP_CLIP_LAYER_RASTER;
    if (r->raw_type == 0 && r->has_filter_info) return CSP_CLIP_LAYER_OTHER;
    return CSP_CLIP_LAYER_UNKNOWN;
}

static int cc_load_raw_layers(csp_clip_document *doc, char *error, size_t error_capacity)
{
    sqlite3_stmt *st = NULL;
    int rc;
    size_t cap = 0;
    int c_id, c_name, c_uuid, c_type, c_comp, c_opacity, c_vis, c_folder;
    int c_lock, c_clip, c_first, c_next, c_render, c_mask;
    int c_rox, c_roy, c_mox, c_moy, c_special, c_vec, c_text, c_grad, c_filter;
    int c_lox, c_loy, c_lmx, c_lmy;

    rc = sqlite3_prepare_v2(doc->db, "SELECT * FROM Layer", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        cc_set_error(error, error_capacity, "cannot query Layer: %s", sqlite3_errmsg(doc->db));
        return CSP_CLIP_ERR_SQLITE;
    }

    c_id = cc_stmt_col(st, "MainId");
    c_name = cc_stmt_col(st, "LayerName");
    c_uuid = cc_stmt_col(st, "LayerUuid");
    c_type = cc_stmt_col(st, "LayerType");
    c_comp = cc_stmt_col(st, "LayerComposite");
    c_opacity = cc_stmt_col(st, "LayerOpacity");
    c_vis = cc_stmt_col(st, "LayerVisibility");
    c_folder = cc_stmt_col(st, "LayerFolder");
    c_lock = cc_stmt_col(st, "LayerLock");
    c_clip = cc_stmt_col(st, "LayerClip");
    c_first = cc_stmt_col(st, "LayerFirstChildIndex");
    c_next = cc_stmt_col(st, "LayerNextIndex");
    c_render = cc_stmt_col(st, "LayerRenderMipmap");
    c_mask = cc_stmt_col(st, "LayerLayerMaskMipmap");
    c_rox = cc_stmt_col(st, "LayerRenderOffscrOffsetX");
    c_roy = cc_stmt_col(st, "LayerRenderOffscrOffsetY");
    c_mox = cc_stmt_col(st, "LayerMaskOffscrOffsetX");
    c_moy = cc_stmt_col(st, "LayerMaskOffscrOffsetY");
    c_lox = cc_stmt_col(st, "LayerOffsetX");
    c_loy = cc_stmt_col(st, "LayerOffsetY");
    c_lmx = cc_stmt_col(st, "LayerMaskOffsetX");
    c_lmy = cc_stmt_col(st, "LayerMaskOffsetY");
    c_special = cc_stmt_col(st, "SpecialRenderType");
    c_vec = cc_stmt_col(st, "VectorNormalType");
    c_text = cc_stmt_col(st, "TextLayerType");
    c_grad = cc_stmt_col(st, "GradationFillInfo");
    c_filter = cc_stmt_col(st, "FilterLayerInfo");

    if (c_id < 0) {
        sqlite3_finalize(st);
        cc_set_error(error, error_capacity, "Layer table has no MainId column");
        return CSP_CLIP_ERR_FORMAT;
    }

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        cc_raw_layer *r;
        cc_raw_layer *tmp;
        if (doc->raw_layer_count == cap) {
            size_t newcap = cap ? cap * 2 : 64;
            if (newcap > 1000000u) {
                sqlite3_finalize(st);
                cc_set_error(error, error_capacity, "unreasonable layer count");
                return CSP_CLIP_ERR_LIMIT;
            }
            tmp = (cc_raw_layer *)realloc(doc->raw_layers, newcap * sizeof(*tmp));
            if (!tmp) {
                sqlite3_finalize(st);
                cc_set_error(error, error_capacity, "out of memory loading layers");
                return CSP_CLIP_ERR_MEMORY;
            }
            doc->raw_layers = tmp;
            cap = newcap;
        }
        r = &doc->raw_layers[doc->raw_layer_count];
        memset(r, 0, sizeof(*r));
        r->id = cc_col_i64(st, c_id, 0);
        r->name = cc_col_strdup(st, c_name);
        r->uuid = cc_col_strdup(st, c_uuid);
        if (!r->name || !r->uuid) {
            free(r->name);
            free(r->uuid);
            r->name = NULL;
            r->uuid = NULL;
            sqlite3_finalize(st);
            cc_set_error(error, error_capacity, "out of memory copying layer metadata");
            return CSP_CLIP_ERR_MEMORY;
        }
        r->raw_type = cc_flex_layer_type(st, c_type);
        r->raw_composite = cc_col_int(st, c_comp, 0);
        r->raw_opacity = cc_col_int(st, c_opacity, 256);
        r->raw_visibility = cc_col_int(st, c_vis, 1);
        r->raw_folder = cc_col_int(st, c_folder, 0);
        r->raw_lock = cc_col_int(st, c_lock, 0);
        r->raw_clip = cc_col_int(st, c_clip, 0);
        r->first_child = cc_col_i64(st, c_first, 0);
        r->next_sibling = cc_col_i64(st, c_next, 0);
        r->render_mipmap = cc_col_i64(st, c_render, 0);
        r->mask_mipmap = cc_col_i64(st, c_mask, 0);
        r->render_offset_x = cc_col_int(st, c_lox, 0) + cc_col_int(st, c_rox, 0);
        r->render_offset_y = cc_col_int(st, c_loy, 0) + cc_col_int(st, c_roy, 0);
        r->mask_offset_x = cc_col_int(st, c_lmx, 0) + cc_col_int(st, c_mox, 0);
        r->mask_offset_y = cc_col_int(st, c_lmy, 0) + cc_col_int(st, c_moy, 0);
        r->special_render_type = cc_col_int(st, c_special, 0);
        r->has_vector_type = cc_col_has_value(st, c_vec);
        r->has_text_type = cc_col_has_value(st, c_text);
        r->has_gradation_fill = cc_col_has_value(st, c_grad);
        r->has_filter_info = cc_col_has_value(st, c_filter);
        doc->raw_layer_count++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        cc_set_error(error, error_capacity, "Layer query failed: %s", sqlite3_errmsg(doc->db));
        return CSP_CLIP_ERR_SQLITE;
    }
    return CSP_CLIP_OK;
}

static int cc_idmap_cmp(const void *a, const void *b)
{
    const cc_idmap *aa = (const cc_idmap *)a;
    const cc_idmap *bb = (const cc_idmap *)b;
    if (aa->id < bb->id) return -1;
    if (aa->id > bb->id) return 1;
    return 0;
}

static int cc_build_idmap(csp_clip_document *doc, char *error, size_t error_capacity)
{
    size_t i;
    if (doc->raw_layer_count == 0) return CSP_CLIP_OK;
    doc->idmap = (cc_idmap *)malloc(doc->raw_layer_count * sizeof(*doc->idmap));
    if (!doc->idmap) {
        cc_set_error(error, error_capacity, "out of memory building layer id map");
        return CSP_CLIP_ERR_MEMORY;
    }
    for (i = 0; i < doc->raw_layer_count; ++i) {
        doc->idmap[i].id = doc->raw_layers[i].id;
        doc->idmap[i].index = i;
    }
    qsort(doc->idmap, doc->raw_layer_count, sizeof(*doc->idmap), cc_idmap_cmp);
    return CSP_CLIP_OK;
}

static size_t cc_find_raw_index(const csp_clip_document *doc, int64_t id)
{
    size_t lo = 0, hi = doc->raw_layer_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (doc->idmap[mid].id == id) return doc->idmap[mid].index;
        if (doc->idmap[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return SIZE_MAX;
}

static int cc_push_public_layer(csp_clip_document *doc, size_t raw_index,
                                int parent_index, int depth,
                                int *out_public_index,
                                char *error, size_t error_capacity)
{
    csp_clip_layer *tmp;
    csp_clip_layer *p;
    const cc_raw_layer *r = &doc->raw_layers[raw_index];
    size_t newcap;
    csp_clip_layer_kind kind;

    if (doc->layer_count == doc->layer_capacity) {
        newcap = doc->layer_capacity ? doc->layer_capacity * 2 : 64;
        tmp = (csp_clip_layer *)realloc(doc->layers, newcap * sizeof(*tmp));
        if (!tmp) {
            cc_set_error(error, error_capacity, "out of memory building layer tree");
            return CSP_CLIP_ERR_MEMORY;
        }
        doc->layers = tmp;
        doc->layer_capacity = newcap;
    }
    p = &doc->layers[doc->layer_count];
    memset(p, 0, sizeof(*p));
    kind = cc_classify_layer(r);
    p->name = r->name;
    p->uuid = r->uuid;
    p->id = r->id;
    p->kind = kind;
    p->opacity = r->raw_opacity <= 0 ? 0.0 : (r->raw_opacity >= 256 ? 1.0 : r->raw_opacity / 256.0);
    p->visible = (r->raw_visibility & 1) != 0;
    p->mask_enabled = (r->raw_visibility & 2) != 0;
    p->locked = r->raw_lock != 0;
    p->clipped = r->raw_clip != 0;
    p->is_group = kind == CSP_CLIP_LAYER_GROUP || kind == CSP_CLIP_LAYER_ROOT;
    p->parent_index = parent_index;
    p->depth = depth;
    p->raw_type = r->raw_type;
    p->raw_composite = r->raw_composite;
    p->raw_visibility = r->raw_visibility;
    p->raw_folder = r->raw_folder;
    p->raw_lock = r->raw_lock;
    p->raw_clip = r->raw_clip;
    p->raw_special_render_type = r->special_render_type;
    p->render_mipmap_id = r->render_mipmap;
    p->mask_mipmap_id = r->mask_mipmap;
    p->render_offset_x = r->render_offset_x;
    p->render_offset_y = r->render_offset_y;
    p->mask_offset_x = r->mask_offset_x;
    p->mask_offset_y = r->mask_offset_y;
    if (out_public_index) *out_public_index = (int)doc->layer_count;
    doc->layer_count++;
    return CSP_CLIP_OK;
}

static int cc_walk_sibling_chain(csp_clip_document *doc, int64_t first_id,
                                 int parent_public, int depth,
                                 uint8_t *visited, int recursion_depth,
                                 char *error, size_t error_capacity)
{
    int64_t id = first_id;
    size_t steps = 0;
    if (recursion_depth > CC_MAX_TREE_DEPTH) {
        cc_set_error(error, error_capacity, "layer tree exceeds maximum nesting depth");
        return CSP_CLIP_ERR_LIMIT;
    }
    while (id != 0) {
        size_t ri = cc_find_raw_index(doc, id);
        const cc_raw_layer *r;
        int pub = -1;
        int rc;
        if (ri == SIZE_MAX) break;
        if (visited[ri]) break;
        visited[ri] = 1;
        r = &doc->raw_layers[ri];
        rc = cc_push_public_layer(doc, ri, parent_public, depth, &pub, error, error_capacity);
        if (rc != CSP_CLIP_OK) return rc;
        if (r->first_child != 0) {
            rc = cc_walk_sibling_chain(doc, r->first_child, pub, depth + 1,
                                       visited, recursion_depth + 1, error, error_capacity);
            if (rc != CSP_CLIP_OK) return rc;
        }
        id = r->next_sibling;
        if (++steps > doc->raw_layer_count) {
            cc_set_error(error, error_capacity, "cycle detected in layer sibling chain");
            return CSP_CLIP_ERR_FORMAT;
        }
    }
    return CSP_CLIP_OK;
}

static int cc_build_layer_tree(csp_clip_document *doc, char *error, size_t error_capacity)
{
    uint8_t *visited;
    size_t root_index, i;
    int rc;
    visited = (uint8_t *)calloc(doc->raw_layer_count ? doc->raw_layer_count : 1, 1);
    if (!visited) {
        cc_set_error(error, error_capacity, "out of memory building layer visitation map");
        return CSP_CLIP_ERR_MEMORY;
    }

    root_index = cc_find_raw_index(doc, doc->info.root_layer_id);
    if (root_index != SIZE_MAX) {
        const cc_raw_layer *root = &doc->raw_layers[root_index];
        if (doc->options.include_root_layer) {
            int root_pub = -1;
            visited[root_index] = 1;
            rc = cc_push_public_layer(doc, root_index, -1, 0, &root_pub, error, error_capacity);
            if (rc != CSP_CLIP_OK) { free(visited); return rc; }
            rc = cc_walk_sibling_chain(doc, root->first_child, root_pub, 1, visited, 1,
                                       error, error_capacity);
        } else {
            visited[root_index] = 1;
            rc = cc_walk_sibling_chain(doc, root->first_child, -1, 0, visited, 1,
                                       error, error_capacity);
        }
        if (rc != CSP_CLIP_OK) { free(visited); return rc; }
    }

    /* Keep schema/version oddities accessible instead of silently dropping rows. */
    for (i = 0; i < doc->raw_layer_count; ++i) {
        if (!visited[i]) {
            if (!doc->options.include_root_layer &&
                (doc->raw_layers[i].id == doc->info.root_layer_id ||
                 cc_classify_layer(&doc->raw_layers[i]) == CSP_CLIP_LAYER_ROOT)) {
                visited[i] = 1;
                continue;
            }
            visited[i] = 1;
            rc = cc_push_public_layer(doc, i, -1, 0, NULL, error, error_capacity);
            if (rc != CSP_CLIP_OK) { free(visited); return rc; }
        }
    }

    free(visited);
    return CSP_CLIP_OK;
}

int csp_clip_open(const char *path, const csp_clip_options *options,
                  csp_clip_document **out_document,
                  char *error, size_t error_capacity)
{
    csp_clip_document *doc;
    csp_clip_options opt;
    int rc;

    cc_clear_error(error, error_capacity);
    if (!path || !out_document) {
        cc_set_error(error, error_capacity, "path and out_document are required");
        return CSP_CLIP_ERR_ARGUMENT;
    }
    *out_document = NULL;
    csp_clip_default_options(&opt);
    if (options) opt = *options;
    if (opt.max_sqlite_bytes == 0 || opt.max_external_objects == 0 ||
        opt.max_pixels == 0 || opt.max_tile_bytes == 0) {
        cc_set_error(error, error_capacity, "invalid zero-valued import limit");
        return CSP_CLIP_ERR_ARGUMENT;
    }

    doc = (csp_clip_document *)calloc(1, sizeof(*doc));
    if (!doc) {
        cc_set_error(error, error_capacity, "out of memory allocating document");
        return CSP_CLIP_ERR_MEMORY;
    }
    doc->path = cc_strdup(path);
    doc->options = opt;
    if (!doc->path) {
        csp_clip_close(doc);
        cc_set_error(error, error_capacity, "out of memory copying path");
        return CSP_CLIP_ERR_MEMORY;
    }

    rc = cc_parse_container(doc, error, error_capacity);
    if (rc != CSP_CLIP_OK) { csp_clip_close(doc); return rc; }
    doc->info.external_object_count = doc->external_count;
    rc = cc_open_sqlite(doc, error, error_capacity);
    if (rc != CSP_CLIP_OK) { csp_clip_close(doc); return rc; }
    rc = cc_load_canvas(doc, error, error_capacity);
    if (rc != CSP_CLIP_OK) { csp_clip_close(doc); return rc; }
    rc = cc_load_raw_layers(doc, error, error_capacity);
    if (rc != CSP_CLIP_OK) { csp_clip_close(doc); return rc; }
    rc = cc_build_idmap(doc, error, error_capacity);
    if (rc != CSP_CLIP_OK) { csp_clip_close(doc); return rc; }
    rc = cc_build_layer_tree(doc, error, error_capacity);
    if (rc != CSP_CLIP_OK) { csp_clip_close(doc); return rc; }

    *out_document = doc;
    return CSP_CLIP_OK;
}

void csp_clip_close(csp_clip_document *document)
{
    size_t i;
    if (!document) return;
    if (document->db) sqlite3_close(document->db);
    if (document->sqlite_bytes) sqlite3_free(document->sqlite_bytes);
    for (i = 0; i < document->external_count; ++i) free(document->externals[i].id);
    for (i = 0; i < document->raw_layer_count; ++i) {
        free(document->raw_layers[i].name);
        free(document->raw_layers[i].uuid);
    }
    free(document->externals);
    free(document->raw_layers);
    free(document->idmap);
    free(document->layers);
    free(document->path);
    free(document);
}

const csp_clip_document_info *csp_clip_get_info(const csp_clip_document *document)
{
    return document ? &document->info : NULL;
}

size_t csp_clip_get_layer_count(const csp_clip_document *document)
{
    return document ? document->layer_count : 0;
}

const csp_clip_layer *csp_clip_get_layer(const csp_clip_document *document, size_t index)
{
    if (!document || index >= document->layer_count) return NULL;
    return &document->layers[index];
}

static const cc_external *cc_find_external(const csp_clip_document *doc,
                                           const char *id, size_t id_len)
{
    size_t i;
    for (i = 0; i < doc->external_count; ++i) {
        if (doc->externals[i].id_len == id_len &&
            memcmp(doc->externals[i].id, id, id_len) == 0) return &doc->externals[i];
    }
    return NULL;
}

static int cc_read_external_body(const csp_clip_document *doc, const cc_external *ext,
                                 uint8_t **out, size_t *out_size,
                                 char *error, size_t error_capacity)
{
    FILE *f;
    uint8_t *buf;
    if (!doc || !ext || !out || !out_size) return CSP_CLIP_ERR_ARGUMENT;
    *out = NULL; *out_size = 0;
    if (ext->body_length > SIZE_MAX) {
        cc_set_error(error, error_capacity, "external body too large for this platform");
        return CSP_CLIP_ERR_LIMIT;
    }
    buf = (uint8_t *)malloc((size_t)ext->body_length ? (size_t)ext->body_length : 1);
    if (!buf) {
        cc_set_error(error, error_capacity, "out of memory reading external tile stream");
        return CSP_CLIP_ERR_MEMORY;
    }
    f = fopen(doc->path, "rb");
    if (!f) {
        free(buf);
        cc_set_error(error, error_capacity, "cannot reopen '%s': %s", doc->path, strerror(errno));
        return CSP_CLIP_ERR_IO;
    }
    if (!cc_seek(f, ext->body_offset) || !cc_read_exact(f, buf, (size_t)ext->body_length)) {
        fclose(f); free(buf);
        cc_set_error(error, error_capacity, "failed reading external tile stream");
        return CSP_CLIP_ERR_IO;
    }
    fclose(f);
    *out = buf;
    *out_size = (size_t)ext->body_length;
    return CSP_CLIP_OK;
}

static int cc_read_csp_utf16_label(const uint8_t *data, size_t size, size_t *pos,
                                   const char *expected)
{
    uint32_t n;
    size_t i, need;
    if (!data || !pos || *pos > size || size - *pos < 4) return 0;
    n = cc_be32(data + *pos);
    *pos += 4;
    if (!cc_mul_size((size_t)n, 2, &need) || need > size - *pos) return 0;
    if (expected) {
        size_t en = strlen(expected);
        if (en != n) return 0;
        for (i = 0; i < en; ++i) {
            if (data[*pos + i * 2] != 0 || data[*pos + i * 2 + 1] != (uint8_t)expected[i]) return 0;
        }
    }
    *pos += need;
    return 1;
}

static int cc_parse_offscreen_attr(const uint8_t *data, size_t size,
                                   cc_offscreen_attr *out,
                                   char *error, size_t error_capacity)
{
    size_t p = 0;
    uint32_t header_size, info_size, extra_size;
    size_t i;
    if (!data || !out || size < 16) {
        cc_set_error(error, error_capacity, "Offscreen.Attribute is truncated");
        return CSP_CLIP_ERR_FORMAT;
    }
    memset(out, 0, sizeof(*out));
    header_size = cc_be32(data + p); p += 4;
    info_size = cc_be32(data + p); p += 4;
    extra_size = cc_be32(data + p); p += 4;
    (void)cc_be32(data + p); p += 4;
    if (header_size != 16 || info_size < 80 || extra_size < 16) {
        /* Keep permissive on versioned sizes, strict on obviously wrong values. */
        if (header_size < 12 || info_size > 65536 || extra_size > 65536) {
            cc_set_error(error, error_capacity, "unrecognized Offscreen.Attribute header");
            return CSP_CLIP_ERR_UNSUPPORTED;
        }
    }
    if (!cc_read_csp_utf16_label(data, size, &p, "Parameter")) {
        cc_set_error(error, error_capacity, "Offscreen.Attribute missing Parameter label");
        return CSP_CLIP_ERR_FORMAT;
    }
    if (size - p < 16 + 16 * 4) {
        cc_set_error(error, error_capacity, "Offscreen.Attribute parameter section is truncated");
        return CSP_CLIP_ERR_FORMAT;
    }
    out->width = cc_be32(data + p); p += 4;
    out->height = cc_be32(data + p); p += 4;
    out->grid_width = cc_be32(data + p); p += 4;
    out->grid_height = cc_be32(data + p); p += 4;
    for (i = 0; i < 16; ++i) { out->values[i] = cc_be32(data + p); p += 4; }
    if (!cc_read_csp_utf16_label(data, size, &p, "InitColor")) {
        cc_set_error(error, error_capacity, "Offscreen.Attribute missing InitColor label");
        return CSP_CLIP_ERR_FORMAT;
    }
    if (size - p < 20) {
        cc_set_error(error, error_capacity, "Offscreen.Attribute InitColor section is truncated");
        return CSP_CLIP_ERR_FORMAT;
    }
    p += 4; /* observed unknown/reserved value */
    out->default_fill = cc_be32(data + p); p += 4;
    p += 12; /* observed unknown/reserved values */
    if (size - p >= 16) {
        for (i = 0; i < 4; ++i) { out->init_color[i] = cc_be32(data + p); p += 4; }
        out->has_init_color = 1;
    }
    out->first_channels = out->values[1];
    out->second_channels = out->values[2];
    out->bit_packed = out->values[8] == 32;
    if (out->width == 0 || out->height == 0 || out->grid_width == 0 || out->grid_height == 0) {
        cc_set_error(error, error_capacity, "invalid Offscreen bitmap/grid dimensions");
        return CSP_CLIP_ERR_FORMAT;
    }
    return CSP_CLIP_OK;
}

static int cc_resolve_offscreen(const csp_clip_document *doc, int64_t mipmap_id,
                                char **out_external_id, size_t *out_external_id_len,
                                uint8_t **out_attr, size_t *out_attr_size,
                                char *error, size_t error_capacity)
{
    sqlite3_stmt *st = NULL;
    int rc;
    int64_t mip_info = 0, offscreen = 0;
    const void *p;
    int n;
    char *id = NULL;
    uint8_t *attr = NULL;

    if (!doc || mipmap_id == 0 || !out_external_id || !out_external_id_len || !out_attr || !out_attr_size)
        return CSP_CLIP_ERR_ARGUMENT;
    *out_external_id = NULL; *out_external_id_len = 0; *out_attr = NULL; *out_attr_size = 0;

    rc = sqlite3_prepare_v2(doc->db, "SELECT BaseMipmapInfo FROM Mipmap WHERE MainId=?1 LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) goto sql_fail;
    sqlite3_bind_int64(st, 1, mipmap_id);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    mip_info = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st); st = NULL;

    rc = sqlite3_prepare_v2(doc->db, "SELECT Offscreen FROM MipmapInfo WHERE MainId=?1 LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) goto sql_fail;
    sqlite3_bind_int64(st, 1, mip_info);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    offscreen = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st); st = NULL;

    rc = sqlite3_prepare_v2(doc->db, "SELECT BlockData, Attribute FROM Offscreen WHERE MainId=?1 LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) goto sql_fail;
    sqlite3_bind_int64(st, 1, offscreen);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }

    if (sqlite3_column_type(st, 0) == SQLITE_NULL || sqlite3_column_type(st, 1) == SQLITE_NULL) {
        sqlite3_finalize(st);
        return CSP_CLIP_ERR_NOT_FOUND;
    }
    p = sqlite3_column_blob(st, 0);
    n = sqlite3_column_bytes(st, 0);
    if (!p || n <= 0) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    id = (char *)malloc((size_t)n + 1);
    if (!id) { sqlite3_finalize(st); return CSP_CLIP_ERR_MEMORY; }
    memcpy(id, p, (size_t)n); id[n] = '\0';

    p = sqlite3_column_blob(st, 1);
    n = sqlite3_column_bytes(st, 1);
    if (!p || n <= 0) { free(id); sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    attr = (uint8_t *)malloc((size_t)n);
    if (!attr) { free(id); sqlite3_finalize(st); return CSP_CLIP_ERR_MEMORY; }
    memcpy(attr, p, (size_t)n);
    *out_external_id = id;
    *out_external_id_len = (size_t)sqlite3_column_bytes(st, 0);
    *out_attr = attr;
    *out_attr_size = (size_t)n;
    sqlite3_finalize(st);
    return CSP_CLIP_OK;

sql_fail:
    cc_set_error(error, error_capacity, "SQLite mipmap lookup failed: %s", sqlite3_errmsg(doc->db));
    if (st) sqlite3_finalize(st);
    free(id); free(attr);
    return CSP_CLIP_ERR_SQLITE;
}

static int cc_utf16_label_at(const uint8_t *p, size_t avail, const char *ascii)
{
    size_t i, n = strlen(ascii);
    if (avail < n * 2) return 0;
    for (i = 0; i < n; ++i) {
        if (p[i * 2] != 0 || p[i * 2 + 1] != (uint8_t)ascii[i]) return 0;
    }
    return 1;
}

typedef enum cc_decode_mode {
    CC_DECODE_RGBA = 0,
    CC_DECODE_GRAY = 1
} cc_decode_mode;

static void cc_fill_default_rgba(uint8_t *dst, size_t pixels, const cc_offscreen_attr *a)
{
    size_t i;
    uint8_t v = a->default_fill ? 255u : 0u;
    uint8_t alpha = a->default_fill ? 255u : 0u;
    if (a->has_init_color) {
        /* Observed values use the high byte for component intensity. */
        uint8_t r = (uint8_t)(a->init_color[0] >> 24);
        uint8_t g = (uint8_t)(a->init_color[1] >> 24);
        uint8_t b = (uint8_t)(a->init_color[2] >> 24);
        uint8_t aa = (uint8_t)(a->init_color[3] >> 24);
        if (r || g || b || aa) {
            for (i = 0; i < pixels; ++i) {
                dst[i * 4 + 0] = r; dst[i * 4 + 1] = g;
                dst[i * 4 + 2] = b; dst[i * 4 + 3] = aa;
            }
            return;
        }
    }
    for (i = 0; i < pixels; ++i) {
        dst[i * 4 + 0] = v; dst[i * 4 + 1] = v; dst[i * 4 + 2] = v; dst[i * 4 + 3] = alpha;
    }
}

static int cc_blit_tile_rgba(uint8_t *dst, size_t stride, uint32_t bitmap_w, uint32_t bitmap_h,
                             uint32_t grid_w, uint32_t tile_index,
                             uint32_t tile_w, uint32_t tile_h,
                             const uint8_t *raw, size_t raw_size, uint32_t channels,
                             char *error, size_t error_capacity)
{
    uint32_t tx, ty, x, y, copy_w, copy_h;
    size_t k, expected;
    if (channels == 0 || tile_w == 0 || tile_h == 0 || grid_w == 0) return CSP_CLIP_ERR_FORMAT;
    if (!cc_mul_size((size_t)tile_w, (size_t)tile_h, &k) ||
        !cc_mul_size(k, channels, &expected) || expected > raw_size) {
        cc_set_error(error, error_capacity, "decoded tile is smaller than its declared dimensions");
        return CSP_CLIP_ERR_DECODE;
    }
    tx = (tile_index % grid_w) * tile_w;
    ty = (tile_index / grid_w) * tile_h;
    if (tx >= bitmap_w || ty >= bitmap_h) return CSP_CLIP_OK;
    copy_w = tile_w < bitmap_w - tx ? tile_w : bitmap_w - tx;
    copy_h = tile_h < bitmap_h - ty ? tile_h : bitmap_h - ty;

    if (channels == 5) {
        /* CSP common packing: planar alpha + interleaved B,G,R,unused. */
        for (y = 0; y < copy_h; ++y) {
            for (x = 0; x < copy_w; ++x) {
                size_t si = (size_t)y * tile_w + x;
                const uint8_t *bgra = raw + k + si * 4;
                uint8_t *d = dst + (size_t)(ty + y) * stride + (size_t)(tx + x) * 4;
                d[0] = bgra[2]; d[1] = bgra[1]; d[2] = bgra[0]; d[3] = raw[si];
            }
        }
    } else if (channels == 2) {
        /* Observed GrayAlpha packing is planar alpha followed by value. */
        for (y = 0; y < copy_h; ++y) {
            for (x = 0; x < copy_w; ++x) {
                size_t si = (size_t)y * tile_w + x;
                uint8_t v = raw[k + si];
                uint8_t *d = dst + (size_t)(ty + y) * stride + (size_t)(tx + x) * 4;
                d[0] = v; d[1] = v; d[2] = v; d[3] = raw[si];
            }
        }
    } else if (channels == 1) {
        for (y = 0; y < copy_h; ++y) {
            for (x = 0; x < copy_w; ++x) {
                size_t si = (size_t)y * tile_w + x;
                uint8_t v = raw[si];
                uint8_t *d = dst + (size_t)(ty + y) * stride + (size_t)(tx + x) * 4;
                d[0] = v; d[1] = v; d[2] = v; d[3] = 255;
            }
        }
    } else {
        cc_set_error(error, error_capacity, "unsupported CSP tile channel count %u", channels);
        return CSP_CLIP_ERR_UNSUPPORTED;
    }
    return CSP_CLIP_OK;
}

static int cc_blit_tile_gray(uint8_t *dst, size_t stride, uint32_t bitmap_w, uint32_t bitmap_h,
                             uint32_t grid_w, uint32_t tile_index,
                             uint32_t tile_w, uint32_t tile_h,
                             const uint8_t *raw, size_t raw_size, uint32_t channels,
                             char *error, size_t error_capacity)
{
    uint32_t tx, ty, x, y, copy_w, copy_h;
    size_t k, expected;
    if (channels == 0 || tile_w == 0 || tile_h == 0 || grid_w == 0) return CSP_CLIP_ERR_FORMAT;
    if (!cc_mul_size((size_t)tile_w, (size_t)tile_h, &k) ||
        !cc_mul_size(k, channels, &expected) || expected > raw_size) {
        cc_set_error(error, error_capacity, "decoded mask tile is smaller than declared dimensions");
        return CSP_CLIP_ERR_DECODE;
    }
    tx = (tile_index % grid_w) * tile_w;
    ty = (tile_index / grid_w) * tile_h;
    if (tx >= bitmap_w || ty >= bitmap_h) return CSP_CLIP_OK;
    copy_w = tile_w < bitmap_w - tx ? tile_w : bitmap_w - tx;
    copy_h = tile_h < bitmap_h - ty ? tile_h : bitmap_h - ty;

    for (y = 0; y < copy_h; ++y) {
        for (x = 0; x < copy_w; ++x) {
            size_t si = (size_t)y * tile_w + x;
            uint8_t v;
            if (channels == 1) v = raw[si];
            else if (channels == 2) v = raw[k + si];
            else if (channels == 5) v = raw[si]; /* alpha is the useful mask-like plane */
            else {
                cc_set_error(error, error_capacity, "unsupported mask channel count %u", channels);
                return CSP_CLIP_ERR_UNSUPPORTED;
            }
            dst[(size_t)(ty + y) * stride + tx + x] = v;
        }
    }
    return CSP_CLIP_OK;
}

static int cc_decode_offscreen(const csp_clip_document *doc, int64_t mipmap_id,
                               cc_decode_mode mode,
                               uint8_t **out_pixels, int *out_w, int *out_h, size_t *out_stride,
                               char *error, size_t error_capacity)
{
    char *ext_id = NULL;
    size_t ext_id_len = 0;
    uint8_t *attr_data = NULL, *body = NULL;
    size_t attr_size = 0, body_size = 0;
    cc_offscreen_attr attr;
    const cc_external *ext;
    uint8_t *pixels = NULL;
    size_t stride, total, pos = 0, pixel_count;
    int rc;

    if (!doc || !out_pixels || !out_w || !out_h || !out_stride) return CSP_CLIP_ERR_ARGUMENT;
    *out_pixels = NULL; *out_w = 0; *out_h = 0; *out_stride = 0;

    rc = cc_resolve_offscreen(doc, mipmap_id, &ext_id, &ext_id_len,
                              &attr_data, &attr_size, error, error_capacity);
    if (rc != CSP_CLIP_OK) return rc;
    rc = cc_parse_offscreen_attr(attr_data, attr_size, &attr, error, error_capacity);
    if (rc != CSP_CLIP_OK) goto done;
    if ((uint64_t)attr.width * attr.height > doc->options.max_pixels) {
        cc_set_error(error, error_capacity, "Offscreen bitmap exceeds configured pixel limit");
        rc = CSP_CLIP_ERR_LIMIT; goto done;
    }
    if (attr.bit_packed) {
        cc_set_error(error, error_capacity, "1-bit packed CSP raster data is not yet supported");
        rc = CSP_CLIP_ERR_UNSUPPORTED; goto done;
    }
    if (doc->info.channel_bytes > 1) {
        cc_set_error(error, error_capacity, "%d-byte CSP color channels are not yet supported",
                     doc->info.channel_bytes);
        rc = CSP_CLIP_ERR_UNSUPPORTED; goto done;
    }

    ext = cc_find_external(doc, ext_id, ext_id_len);
    if (!ext) {
        cc_set_error(error, error_capacity, "external object '%.*s' was not found in CSFCHUNK envelope",
                     (int)ext_id_len, ext_id);
        rc = CSP_CLIP_ERR_NOT_FOUND; goto done;
    }
    rc = cc_read_external_body(doc, ext, &body, &body_size, error, error_capacity);
    if (rc != CSP_CLIP_OK) goto done;

    if (mode == CC_DECODE_RGBA) {
        if (!cc_mul_size((size_t)attr.width, 4, &stride) ||
            !cc_mul_size(stride, attr.height, &total)) {
            rc = CSP_CLIP_ERR_LIMIT; cc_set_error(error, error_capacity, "RGBA allocation size overflow"); goto done;
        }
    } else {
        stride = attr.width;
        if (!cc_mul_size(stride, attr.height, &total)) {
            rc = CSP_CLIP_ERR_LIMIT; cc_set_error(error, error_capacity, "mask allocation size overflow"); goto done;
        }
    }
    if (!cc_mul_size((size_t)attr.width, (size_t)attr.height, &pixel_count)) {
        rc = CSP_CLIP_ERR_LIMIT; goto done;
    }
    pixels = (uint8_t *)malloc(total ? total : 1);
    if (!pixels) { rc = CSP_CLIP_ERR_MEMORY; cc_set_error(error, error_capacity, "out of memory allocating decoded bitmap"); goto done; }
    if (mode == CC_DECODE_RGBA) cc_fill_default_rgba(pixels, pixel_count, &attr);
    else memset(pixels, attr.default_fill ? 255 : 0, total);

    while (pos + 8 <= body_size) {
        uint32_t block_total, label_chars;
        size_t end, q;
        uint32_t tile_index, channels, tile_w, tile_h, has_data;

        block_total = cc_be32(body + pos);
        label_chars = cc_be32(body + pos + 4);
        if (label_chars != 19 || pos + 8 + 38 > body_size ||
            !cc_utf16_label_at(body + pos + 8, body_size - pos - 8, "BlockDataBeginChunk")) {
            /* BlockStatus / BlockCheckSum follow data blocks; they are not needed for reading. */
            break;
        }
        if (block_total < 8 + 38 + 4 + 12 + 4 + 4 + 34 || block_total > body_size - pos) {
            cc_set_error(error, error_capacity, "invalid BlockDataBeginChunk length");
            rc = CSP_CLIP_ERR_FORMAT; goto done;
        }
        end = pos + block_total;
        q = pos + 8 + 38;
        if (q + 20 > end) { rc = CSP_CLIP_ERR_FORMAT; cc_set_error(error, error_capacity, "truncated tile block header"); goto done; }
        tile_index = cc_be32(body + q); q += 4;
        channels = cc_be16(body + q);
        /* q+2: reserved */
        tile_w = cc_be32(body + q + 4);
        tile_h = cc_be32(body + q + 8);
        q += 12;
        has_data = cc_be32(body + q); q += 4;
        if (has_data > 1) { rc = CSP_CLIP_ERR_FORMAT; cc_set_error(error, error_capacity, "invalid tile has-data flag"); goto done; }

        if (has_data) {
            uint32_t outer_len, comp_len;
            size_t expected, k;
            uLongf dst_len;
            uint8_t *raw;
            int zrc;
            if (q + 8 > end) { rc = CSP_CLIP_ERR_FORMAT; cc_set_error(error, error_capacity, "truncated compressed tile header"); goto done; }
            outer_len = cc_be32(body + q);
            comp_len = cc_le32(body + q + 4);
            q += 8;
            if (outer_len < 4 || comp_len > outer_len - 4 || comp_len > end - q) {
                rc = CSP_CLIP_ERR_FORMAT; cc_set_error(error, error_capacity, "invalid compressed tile length"); goto done;
            }
            if (channels == 0 || tile_w == 0 || tile_h == 0 ||
                !cc_mul_size((size_t)tile_w, tile_h, &k) || !cc_mul_size(k, channels, &expected) ||
                expected > doc->options.max_tile_bytes) {
                rc = CSP_CLIP_ERR_LIMIT; cc_set_error(error, error_capacity, "tile dimensions/channel count exceed configured limit"); goto done;
            }
            raw = (uint8_t *)malloc(expected ? expected : 1);
            if (!raw) { rc = CSP_CLIP_ERR_MEMORY; cc_set_error(error, error_capacity, "out of memory decompressing tile"); goto done; }
            dst_len = (uLongf)expected;
            zrc = uncompress(raw, &dst_len, body + q, (uLong)comp_len);
            if (zrc != Z_OK || dst_len != expected) {
                free(raw);
                rc = CSP_CLIP_ERR_DECODE;
                cc_set_error(error, error_capacity, "zlib tile decode failed (%d, got %lu expected %lu)",
                             zrc, (unsigned long)dst_len, (unsigned long)expected);
                goto done;
            }
            if (mode == CC_DECODE_RGBA) {
                rc = cc_blit_tile_rgba(pixels, stride, attr.width, attr.height,
                                       attr.grid_width, tile_index, tile_w, tile_h,
                                       raw, expected, channels, error, error_capacity);
            } else {
                rc = cc_blit_tile_gray(pixels, stride, attr.width, attr.height,
                                       attr.grid_width, tile_index, tile_w, tile_h,
                                       raw, expected, channels, error, error_capacity);
            }
            free(raw);
            if (rc != CSP_CLIP_OK) goto done;
        }

        /* Validate the closing label when present. */
        if (end >= pos + 38) {
            size_t e = end - (4 + 34);
            if (e >= pos && cc_be32(body + e) == 17 &&
                !cc_utf16_label_at(body + e + 4, end - e - 4, "BlockDataEndChunk")) {
                rc = CSP_CLIP_ERR_FORMAT; cc_set_error(error, error_capacity, "tile block has invalid end label"); goto done;
            }
        }
        pos = end;
    }

    *out_pixels = pixels; pixels = NULL;
    *out_w = (int)attr.width; *out_h = (int)attr.height; *out_stride = stride;
    rc = CSP_CLIP_OK;

done:
    free(ext_id); free(attr_data); free(body); free(pixels);
    return rc;
}

int csp_clip_decode_layer_rgba(const csp_clip_document *document, size_t layer_index,
                               uint8_t **out_rgba, int *out_width, int *out_height,
                               size_t *out_stride, int *out_x, int *out_y,
                               char *error, size_t error_capacity)
{
    const csp_clip_layer *layer;
    int rc;
    cc_clear_error(error, error_capacity);
    if (!document || layer_index >= document->layer_count || !out_rgba ||
        !out_width || !out_height || !out_stride) {
        cc_set_error(error, error_capacity, "invalid layer decode arguments");
        return CSP_CLIP_ERR_ARGUMENT;
    }
    layer = &document->layers[layer_index];
    if (layer->render_mipmap_id == 0) {
        cc_set_error(error, error_capacity, "layer has no cached/rendered mipmap");
        return CSP_CLIP_ERR_NOT_FOUND;
    }
    rc = cc_decode_offscreen(document, layer->render_mipmap_id, CC_DECODE_RGBA,
                             out_rgba, out_width, out_height, out_stride,
                             error, error_capacity);
    if (rc == CSP_CLIP_OK) {
        if (out_x) *out_x = layer->render_offset_x;
        if (out_y) *out_y = layer->render_offset_y;
    }
    return rc;
}

int csp_clip_decode_layer_mask_gray8(const csp_clip_document *document, size_t layer_index,
                                     uint8_t **out_gray, int *out_width, int *out_height,
                                     size_t *out_stride, int *out_x, int *out_y,
                                     char *error, size_t error_capacity)
{
    const csp_clip_layer *layer;
    int rc;
    cc_clear_error(error, error_capacity);
    if (!document || layer_index >= document->layer_count || !out_gray ||
        !out_width || !out_height || !out_stride) {
        cc_set_error(error, error_capacity, "invalid mask decode arguments");
        return CSP_CLIP_ERR_ARGUMENT;
    }
    layer = &document->layers[layer_index];
    if (layer->mask_mipmap_id == 0) {
        cc_set_error(error, error_capacity, "layer has no layer mask mipmap");
        return CSP_CLIP_ERR_NOT_FOUND;
    }
    rc = cc_decode_offscreen(document, layer->mask_mipmap_id, CC_DECODE_GRAY,
                             out_gray, out_width, out_height, out_stride,
                             error, error_capacity);
    if (rc == CSP_CLIP_OK) {
        if (out_x) *out_x = layer->mask_offset_x;
        if (out_y) *out_y = layer->mask_offset_y;
    }
    return rc;
}

int csp_clip_read_preview(const csp_clip_document *document,
                          uint8_t **out_data, size_t *out_size,
                          int *out_width, int *out_height,
                          char *error, size_t error_capacity)
{
    sqlite3_stmt *st = NULL;
    const void *p;
    int n, rc, ci, cw, ch;
    uint8_t *copy;
    cc_clear_error(error, error_capacity);
    if (!document || !out_data || !out_size) return CSP_CLIP_ERR_ARGUMENT;
    *out_data = NULL; *out_size = 0;
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    rc = sqlite3_prepare_v2(document->db, "SELECT * FROM CanvasPreview LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        cc_set_error(error, error_capacity, "cannot query CanvasPreview: %s", sqlite3_errmsg(document->db));
        return CSP_CLIP_ERR_SQLITE;
    }
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    ci = cc_stmt_col(st, "ImageData");
    cw = cc_stmt_col(st, "ImageWidth");
    ch = cc_stmt_col(st, "ImageHeight");
    if (ci < 0 || sqlite3_column_type(st, ci) == SQLITE_NULL) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    p = sqlite3_column_blob(st, ci); n = sqlite3_column_bytes(st, ci);
    if (!p || n <= 0) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    copy = (uint8_t *)malloc((size_t)n);
    if (!copy) { sqlite3_finalize(st); return CSP_CLIP_ERR_MEMORY; }
    memcpy(copy, p, (size_t)n);
    if (out_width) *out_width = cc_col_int(st, cw, 0);
    if (out_height) *out_height = cc_col_int(st, ch, 0);
    *out_data = copy; *out_size = (size_t)n;
    sqlite3_finalize(st);
    return CSP_CLIP_OK;
}

int csp_clip_read_blob_by_main_id(const csp_clip_document *document,
                                  const char *table, const char *column, int64_t main_id,
                                  uint8_t **out_data, size_t *out_size,
                                  char *error, size_t error_capacity)
{
    char sql[512];
    sqlite3_stmt *st = NULL;
    const void *p;
    int n, rc;
    uint8_t *copy;
    cc_clear_error(error, error_capacity);
    if (!document || !table || !column || !out_data || !out_size ||
        !cc_ident_ok(table) || !cc_ident_ok(column)) {
        cc_set_error(error, error_capacity, "invalid table/column/blob arguments");
        return CSP_CLIP_ERR_ARGUMENT;
    }
    *out_data = NULL; *out_size = 0;
    if (snprintf(sql, sizeof(sql), "SELECT \"%s\" FROM \"%s\" WHERE MainId=?1 LIMIT 1", column, table) >= (int)sizeof(sql)) {
        return CSP_CLIP_ERR_ARGUMENT;
    }
    rc = sqlite3_prepare_v2(document->db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        cc_set_error(error, error_capacity, "blob query failed: %s", sqlite3_errmsg(document->db));
        return CSP_CLIP_ERR_SQLITE;
    }
    sqlite3_bind_int64(st, 1, main_id);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    if (sqlite3_column_type(st, 0) == SQLITE_NULL) { sqlite3_finalize(st); return CSP_CLIP_ERR_NOT_FOUND; }
    p = sqlite3_column_blob(st, 0); n = sqlite3_column_bytes(st, 0);
    if (n < 0 || (!p && n != 0)) { sqlite3_finalize(st); return CSP_CLIP_ERR_FORMAT; }
    copy = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!copy) { sqlite3_finalize(st); return CSP_CLIP_ERR_MEMORY; }
    if (n) memcpy(copy, p, (size_t)n);
    *out_data = copy; *out_size = (size_t)n;
    sqlite3_finalize(st);
    return CSP_CLIP_OK;
}

const char *csp_clip_layer_kind_name(csp_clip_layer_kind kind)
{
    switch (kind) {
        case CSP_CLIP_LAYER_RASTER: return "raster";
        case CSP_CLIP_LAYER_GROUP: return "group";
        case CSP_CLIP_LAYER_VECTOR: return "vector";
        case CSP_CLIP_LAYER_TEXT: return "text";
        case CSP_CLIP_LAYER_FILL: return "fill/gradient";
        case CSP_CLIP_LAYER_CORRECTION: return "correction";
        case CSP_CLIP_LAYER_PAPER: return "paper";
        case CSP_CLIP_LAYER_ROOT: return "root";
        case CSP_CLIP_LAYER_OTHER: return "other";
        default: return "unknown";
    }
}

const char *csp_clip_blend_mode_name(int v)
{
    switch (v) {
        case 0: return "normal";
        case 1: return "darken";
        case 2: return "multiply";
        case 3: return "color burn";
        case 4: return "linear burn";
        case 5: return "subtract";
        case 6: return "darker color";
        case 7: return "lighten";
        case 8: return "screen";
        case 9: return "color dodge";
        case 10: return "color dodge variant";
        case 11: return "add";
        case 12: return "add glow";
        case 13: return "lighter color";
        case 14: return "overlay";
        case 15: return "soft light";
        case 16: return "hard light";
        case 17: return "vivid light";
        case 18: return "linear light";
        case 19: return "pin light";
        case 20: return "hard mix";
        case 21: return "difference";
        case 22: return "exclusion";
        case 23: return "hue";
        case 24: return "saturation";
        case 25: return "color";
        case 26: return "luminosity";
        case 30: return "pass through";
        case 36: return "divide";
        default: return "unknown";
    }
}

const char *csp_clip_result_string(int result)
{
    switch (result) {
        case CSP_CLIP_OK: return "ok";
        case CSP_CLIP_ERR_ARGUMENT: return "invalid argument";
        case CSP_CLIP_ERR_IO: return "I/O error";
        case CSP_CLIP_ERR_FORMAT: return "invalid or unsupported CLIP structure";
        case CSP_CLIP_ERR_MEMORY: return "out of memory";
        case CSP_CLIP_ERR_SQLITE: return "embedded SQLite error";
        case CSP_CLIP_ERR_NOT_FOUND: return "requested data not found";
        case CSP_CLIP_ERR_UNSUPPORTED: return "feature not supported";
        case CSP_CLIP_ERR_DECODE: return "pixel decode failed";
        case CSP_CLIP_ERR_LIMIT: return "configured resource limit exceeded";
        default: return "unknown error";
    }
}

void csp_clip_free(void *memory)
{
    free(memory);
}
