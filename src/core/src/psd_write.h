// What the PSD writer's parts share (psd_writer.cpp: the document's layers as records and the file's sections;
// psd_encode.cpp: the bytes, from channels and layer records to the merged image). Internal to the core.
#pragma once
#include "nekophoto/document.h"
#include "nekophoto/psd_carry.h"
#include "nekophoto/psd_writer.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nekophoto::psd_write {

/// Big-endian fields appended to a byte vector.
struct Out {
    std::vector<uint8_t> b;
    void u8(unsigned v) { b.push_back(uint8_t(v)); }
    void u16(unsigned v) { u8((v >> 8) & 0xff); u8(v & 0xff); }
    void i16(int v) { u16(unsigned(uint16_t(int16_t(v)))); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    /// A length: 64 bits in a PSB where Photoshop widens it, else 32.
    void length(uint64_t v, bool wide) { if (wide) u64(v); else u32(uint32_t(v)); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    void str(const char* s) { b.insert(b.end(), s, s + std::strlen(s)); }
    void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};

/// A layer record, ready to encode.
struct Record {
    int top = 0, left = 0, bottom = 0, right = 0;
    std::string name;
    std::string blend = "norm";
    uint8_t opacity = 255;
    bool clipping = false, hidden = false;
    uint8_t flags = 0;                     // besides hidden (bit 1) and bit 3, which are always written
    int section = 0;                                     // lsct: 1 folder, 3 its end marker
    std::vector<std::pair<int, std::vector<uint8_t>>> channels;   // id, encoded data
    bool mask = false;
    int maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
    uint8_t maskDefault = 255, maskFlags = 0;
    const char* adjustmentKey = nullptr;
    std::vector<uint8_t> adjustmentData;
    /// Carried blocks a freshly written adjustment replaces (a Brightness/Contrast's 'CgEd' beside its 'brit').
    std::set<std::string> replaces;
    // Carried from the PSD the layer came from (psd_carry.h).
    std::vector<PsdBlock> carried;
    std::vector<uint8_t> blendingRanges;
    std::vector<uint8_t> rawMask;          // the mask section as stored; its channels are in `channels`
    uint8_t fill = 255;                    // written as 'iOpa' below 255
    uint32_t layerId = 0;                  // 'lyid': assigned for every record
    bool carriedPixels = false;            // the channels are the file's own, as read (PsdLayerCarry::planes)
};

/// PackBits: runs of three or more equal bytes as repeats, the rest as literals of up to 128.
void packBits(const uint8_t* row, int n, std::vector<uint8_t>& out);
/// The tagged blocks whose length is 64 bits in a PSB.
bool longKey(const std::string& key);
/// A PackBits row made even in length, decoding the same (Photoshop rejects a smart object's embedded file whose
/// merged image has an odd row when the document keeps Smart Filter caches).
void makeRowEven(std::vector<uint8_t>& row, const uint8_t* raw, int w);

/// One channel's data as a layer record stores it: the compression word, then the rows.
std::vector<uint8_t> encodeChannel(const std::vector<uint8_t>& plane, int w, int h, bool compress, bool large = false);
/// A 16-bit channel (0..65535 samples): zip with prediction, as Photoshop writes 16-bit layers, or raw.
std::vector<uint8_t> encodeChannel16(const std::vector<uint16_t>& plane, int w, int h, bool compress);
/// A 32-bit channel (linear floats): zip with prediction, as Photoshop writes 32-bit layers, or raw big-endian floats.
std::vector<uint8_t> encodeChannel32(const std::vector<float>& plane, int w, int h, bool compress);

/// Straight linear float planes from a premultiplied 32-bit image (colour above 1 kept).
std::array<std::vector<float>, 4> straightPlanesF(const ImageF& image);
/// Straight 0..65535 planes from a premultiplied 16-bit image, as straightPlanes does at 8 bits.
std::array<std::vector<uint16_t>, 4> straightPlanes16(const Image16& image);
/// Straight (not premultiplied) planes from premultiplied RGBA: PSD channels hold straight colour.
std::array<std::vector<uint8_t>, 4> straightPlanes(const Image& image);
/// Straight planes of a CMYK or Lab buffer (Image, ImageC8 or Image16), `n` samples a pixel (alpha last).
template <class Img>
std::vector<std::vector<uint16_t>> straightPlanesMode(const Img& image, int n, bool lab);

/// Photoshop's key for a blend mode.
const char* blendKey(BlendMode mode);
/// UTF-8 to UTF-16 code units (invalid bytes become U+FFFD).
std::vector<uint16_t> utf16(const std::string& s);
/// The legacy name field: ASCII only (anything else becomes '_'), at most 255 bytes; luni carries the real one.
std::string asciiName(const std::string& s);

/// One layer record, its channels' data written separately after all the records.
void writeRecord(Out& o, const Record& r, bool large);

/// The merged image section: the document rendered at its depth and layout, matted against white where it is
/// transparent as Photoshop stores it, then the alpha and spot channels.
void writeMergedImage(Out& f, const Document& document, const PsdExportOptions& options);

} // namespace nekophoto::psd_write
