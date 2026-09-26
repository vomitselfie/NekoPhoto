#include "compositor/raw.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#ifdef COMPOSITOR_HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

namespace compositor {

bool rawSupported() {
#ifdef COMPOSITOR_HAVE_LIBRAW
    return true;
#else
    return false;
#endif
}

bool isRawPath(const std::string& path) {
    static const std::array<const char*, 26> extensions{"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "rwl",
                                                        "pef", "ptx", "dng", "3fr", "fff", "iiq", "erf", "kdc", "dcr", "mrw", "mos", "srw", "x3f", "raw"};
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::find_if(extensions.begin(), extensions.end(), [&](const char* e) { return ext == e; }) != extensions.end();
}

std::shared_ptr<Image> decodeRaw(const std::string& path, std::string* error) {
#ifdef COMPOSITOR_HAVE_LIBRAW
    auto raw = std::make_unique<LibRaw>();   // large: on the heap
    auto fail = [&](int code) { if (error) *error = std::string("Couldn't read the RAW file: ") + libraw_strerror(code); return nullptr; };
    raw->imgdata.params.use_camera_wb = 1;
    raw->imgdata.params.output_color = 1;   // sRGB
    raw->imgdata.params.output_bps = 8;
    if (int r = raw->open_file(path.c_str()); r != LIBRAW_SUCCESS) return fail(r);
    const auto& size = raw->imgdata.sizes;
    if (size.width > 30000 || size.height > 30000) { if (error) *error = "Images up to 30,000 pixels per side are supported."; return nullptr; }
    if (int r = raw->unpack(); r != LIBRAW_SUCCESS) return fail(r);
    if (int r = raw->dcraw_process(); r != LIBRAW_SUCCESS) return fail(r);
    int code = 0;
    libraw_processed_image_t* developed = raw->dcraw_make_mem_image(&code);
    if (!developed) return fail(code);
    std::shared_ptr<Image> out;
    if (developed->type == LIBRAW_IMAGE_BITMAP && developed->bits == 8 && (developed->colors == 3 || developed->colors == 1)) {
        out = std::make_shared<Image>(developed->width, developed->height);
        const int n = developed->colors;
        for (int y = 0; y < developed->height; y++) {
            const uint8_t* src = developed->data + size_t(y) * developed->width * n;
            uint8_t* dst = out->row(y);
            for (int x = 0; x < developed->width; x++, src += n, dst += 4) {
                dst[0] = src[0]; dst[1] = src[n == 3 ? 1 : 0]; dst[2] = src[n == 3 ? 2 : 0]; dst[3] = 255;
            }
        }
    } else if (error) *error = "The RAW file developed into an unexpected format.";
    LibRaw::dcraw_clear_mem(developed);
    return out;
#else
    (void)path;
    if (error) *error = "This build of NekoPhoto has no RAW support (LibRaw).";
    return nullptr;
#endif
}

} // namespace compositor
