#include "compositor/raw.h"
#include "compositor/document.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>

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

std::vector<uint8_t> readRawFileBytes(const std::string& path, std::string* error, size_t limit) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { if (error) *error = "Couldn't read the file."; return {}; }
    const std::streamoff size = in.tellg();
    if (size <= 0 || uint64_t(size) > uint64_t(limit)) { if (error) *error = size <= 0 ? "The file is empty." : "The file is too large."; return {}; }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) { if (error) *error = "Couldn't read the file."; return {}; }
    return bytes;
}

#ifdef COMPOSITOR_HAVE_LIBRAW
namespace {

int cancelCallback(void* data, enum LibRaw_progress, int, int) {
    const auto* cancel = static_cast<const std::atomic<bool>*>(data);
    return cancel && cancel->load() ? 1 : 0;
}

void setError(std::string* error, const std::string& message) { if (error) *error = message; }

/// Opens `bytes` (which must outlive it) and checks the size against the canvas budget; null with `error` otherwise.
std::unique_ptr<LibRaw> openRaw(const std::vector<uint8_t>& bytes, std::string* error) {
    if (bytes.empty()) { setError(error, "The file is empty."); return nullptr; }
    auto raw = std::make_unique<LibRaw>();   // large: on the heap
    if (int r = raw->open_buffer(bytes.data(), bytes.size()); r != LIBRAW_SUCCESS) {
        setError(error, std::string("Couldn't read the RAW file: ") + libraw_strerror(r));
        return nullptr;
    }
    const auto& size = raw->imgdata.sizes;
    if (size.width > 30000 || size.height > 30000) { setError(error, "Images up to 30,000 pixels per side are supported."); return nullptr; }
    if ((long long)size.width * size.height > Document::pixelBudget || (long long)size.raw_width * size.raw_height > 2 * Document::pixelBudget) {
        setError(error, "The RAW image is larger than the 100-megapixel canvas budget.");
        return nullptr;
    }
    return raw;
}

/// Demosaiced with the camera's white balance into sRGB at `bits` (8 or 16); LibRaw's processed image, or null.
libraw_processed_image_t* process(LibRaw& raw, int bits, const RawDecodeOptions& options, std::string* error) {
    raw.imgdata.params.use_camera_wb = 1;
    raw.imgdata.params.output_color = 1;   // sRGB
    raw.imgdata.params.output_bps = bits;
    raw.imgdata.params.half_size = options.halfSize ? 1 : 0;
    if (options.cancel) raw.set_progress_handler(cancelCallback, const_cast<std::atomic<bool>*>(options.cancel));
    auto fail = [&](int code) -> libraw_processed_image_t* {
        setError(error, code == LIBRAW_CANCELLED_BY_CALLBACK ? std::string("Cancelled.") : std::string("Couldn't read the RAW file: ") + libraw_strerror(code));
        return nullptr;
    };
    if (int r = raw.unpack(); r != LIBRAW_SUCCESS) return fail(r);
    if (options.cancel && options.cancel->load()) return fail(LIBRAW_CANCELLED_BY_CALLBACK);
    if (int r = raw.dcraw_process(); r != LIBRAW_SUCCESS) return fail(r);
    int code = 0;
    libraw_processed_image_t* developed = raw.dcraw_make_mem_image(&code);
    if (!developed) return fail(code);
    if (developed->type != LIBRAW_IMAGE_BITMAP || developed->bits != bits || (developed->colors != 3 && developed->colors != 1)) {
        LibRaw::dcraw_clear_mem(developed);
        setError(error, "The RAW file developed into an unexpected format.");
        return nullptr;
    }
    return developed;
}

} // namespace
#endif

std::shared_ptr<Image> decodeRaw(const std::string& path, std::string* error) {
#ifdef COMPOSITOR_HAVE_LIBRAW
    const std::vector<uint8_t> bytes = readRawFileBytes(path, error);
    if (bytes.empty()) return nullptr;
    auto raw = openRaw(bytes, error);
    if (!raw) return nullptr;
    libraw_processed_image_t* developed = process(*raw, 8, {}, error);
    if (!developed) return nullptr;
    auto out = std::make_shared<Image>(developed->width, developed->height);
    const int n = developed->colors;
    for (int y = 0; y < developed->height; y++) {
        const uint8_t* src = developed->data + size_t(y) * developed->width * n;
        uint8_t* dst = out->row(y);
        for (int x = 0; x < developed->width; x++, src += n, dst += 4) {
            dst[0] = src[0]; dst[1] = src[n == 3 ? 1 : 0]; dst[2] = src[n == 3 ? 2 : 0]; dst[3] = 255;
        }
    }
    LibRaw::dcraw_clear_mem(developed);
    return out;
#else
    (void)path;
    if (error) *error = "This build of NekoPhoto has no RAW support (LibRaw).";
    return nullptr;
#endif
}

bool readRawInfo(const std::vector<uint8_t>& bytes, RawInfo& info, std::string* error) {
#ifdef COMPOSITOR_HAVE_LIBRAW
    auto raw = openRaw(bytes, error);
    if (!raw) return false;
    const auto& id = raw->imgdata.idata;
    const auto& size = raw->imgdata.sizes;
    const auto& other = raw->imgdata.other;
    info.make = id.make;
    info.model = id.model;
    const bool turned = size.flip == 5 || size.flip == 6;   // a quarter turn either way
    info.width = turned ? size.height : size.width;
    info.height = turned ? size.width : size.height;
    info.iso = other.iso_speed;
    info.shutter = other.shutter;
    info.aperture = other.aperture;
    info.focalLength = other.focal_len;
    return true;
#else
    (void)bytes; (void)info;
    if (error) *error = "This build of NekoPhoto has no RAW support (LibRaw).";
    return false;
#endif
}

std::shared_ptr<Image16> decodeRaw16(const std::vector<uint8_t>& bytes, const RawDecodeOptions& options, std::string* error) {
#ifdef COMPOSITOR_HAVE_LIBRAW
    auto raw = openRaw(bytes, error);
    if (!raw) return nullptr;
    libraw_processed_image_t* developed = process(*raw, 16, options, error);
    if (!developed) return nullptr;
    auto out = std::make_shared<Image16>(developed->width, developed->height);
    const int n = developed->colors;
    // LibRaw's 0..65535 onto the 15-bit scale, rounded.
    auto level = [](uint16_t v) { return uint16_t((uint32_t(v) * 32768u + 32767u) / 65535u); };
    for (int y = 0; y < developed->height; y++) {
        const auto* src = reinterpret_cast<const uint16_t*>(developed->data) + size_t(y) * size_t(developed->width) * size_t(n);
        uint16_t* dst = out->row(y);
        for (int x = 0; x < developed->width; x++, src += n, dst += 4) {
            dst[0] = level(src[0]); dst[1] = level(src[n == 3 ? 1 : 0]); dst[2] = level(src[n == 3 ? 2 : 0]); dst[3] = 32768;
        }
    }
    LibRaw::dcraw_clear_mem(developed);
    return out;
#else
    (void)bytes; (void)options;
    if (error) *error = "This build of NekoPhoto has no RAW support (LibRaw).";
    return nullptr;
#endif
}

std::shared_ptr<Image16> developRaw(const std::vector<uint8_t>& bytes, const CameraRawSettings& settings, const RawDecodeOptions& options,
                                    std::string* error, uint32_t seed) {
    auto image = decodeRaw16(bytes, options, error);
    if (!image) return nullptr;
    if (options.cancel && options.cancel->load()) { if (error) *error = "Cancelled."; return nullptr; }
    const CameraRawSettings normalized = settings.normalized();
    if (!normalized.isIdentity() && !applyCameraRaw(*image, normalized, options.halfSize ? 0.5 : 1.0, seed)) {
        if (error) *error = "The Camera Raw settings are not valid.";
        return nullptr;
    }
    return image;
}

} // namespace compositor
