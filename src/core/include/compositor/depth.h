// Moving pixels between depths, and the 16-bit primitives the renderer and the file formats share
// (docs/high-bit-depth-plan.md, sections 3, 4 and 8).
//
// 16-bit samples use Photoshop's internal range 0..32768 (SampleTraits<U16>::one), so premultiplying and
// multiplying are an exact `x * a >> 15`. Files (PSD, PNG, TIFF) store 0..65535; `from65535` / `to65535` map
// between the two with rounding: a 15-bit value survives the trip out and back, a 16-bit file's odd values do
// not (PSD keeps them in a carried plane, psd_carry.h).
#pragma once
#include "colormodes.h"
#include "imaget.h"
#include "sampletype.h"
#include <algorithm>
#include <cstdint>
#include <memory>

namespace compositor {

constexpr uint32_t one16 = SampleTraits<SampleType::U16>::one;

/// 8-bit to 15-bit and back, rounded; narrow16(widen8(v)) == v for every byte.
constexpr uint16_t widen8(uint8_t v) { return uint16_t((uint32_t(v) * one16 + 127) / 255); }
constexpr uint8_t narrow16(uint32_t v) { return uint8_t((std::min(v, one16) * 255 + one16 / 2) >> 15); }
/// A file's 0..65535 to 0..32768 and back, rounded; from65535(to65535(v)) == v for every 15-bit value.
constexpr uint16_t from65535(uint32_t v) { return uint16_t((v * one16 + 32767) / 65535); }
constexpr uint16_t to65535(uint32_t v) { return uint16_t((std::min(v, one16) * 65535 + one16 / 2) >> 15); }
/// round(a * b / 32768) for 15-bit values (a product fits 31 bits).
constexpr uint32_t mul15(uint32_t a, uint32_t b) { return (a * b + one16 / 2) >> 15; }

/// Premultiplied 8-bit to 16-bit and back, sample by sample (colour stays within alpha either way).
std::shared_ptr<Image16> widenImage(const Image& image);
std::shared_ptr<Gray16> widenGray(const GrayImage& image);
std::shared_ptr<Image> narrowImage(const Image16& image);
std::shared_ptr<GrayImage> narrowGray(const Gray16& image);
/// `narrowImage` into an existing buffer of the same size: the canvas's display step for a 16-bit render
/// (toDisplay<U16>; no colour transform yet).
void narrowInto(const Image16& image, Image& out);
/// 8 bits with an ordered (4 x 4 Bayer) dither in place of rounding, for exports to 8-bit-only formats: smooth
/// 16-bit gradients stay smooth instead of banding.
std::shared_ptr<Image> ditherToEightBit(const Image16& image);

/// Straight <-> premultiplied at 15 bits.
void premultiply(Image16& image);
void unpremultiply(Image16& image);

/// A 16-bit buffer's 8-bit thumbnail (the Layers panel's): reduced, then narrowed.
std::shared_ptr<Image> makeThumbnail(const Image16& image, int maxSide = 96);
std::shared_ptr<GrayImage> makeGrayThumbnail(const Gray16& image, int maxSide = 96);

/// Box-filtered halvings, rounded means as halveImage takes them.
std::shared_ptr<Image16> halveImage(const Image16& image);
/// The same for 8-bit CMYK (and a 5-channel Image16, which halveImage takes too): every sample, alpha included.
std::shared_ptr<ImageC8> halveImage(const ImageC8& image);
/// A box-filtered reduction to `width` x `height` (thumbnails) for 4- or 5-channel buffers.
std::shared_ptr<ImageC8> boxResizeImage(const ImageC8& image, int width, int height);
std::shared_ptr<Image16> boxResizeImage(const Image16& image, int width, int height);
std::shared_ptr<Gray16> halveGray(const Gray16& image);
std::shared_ptr<Image16> reduceImage(const Image16& image, int level);
std::shared_ptr<Gray16> reduceGray(const Gray16& image, int level);

/// The point samplers at 15 bits: bilinear and Catmull-Rom with the 8-bit samplers' taps (resample.h).
void sampleBilinear(const Image16& image, double x, double y, uint16_t out[4]);
void sampleBicubic(const Image16& image, double x, double y, uint16_t out[4]);
int sampleGrayBilinear(const Gray16& image, double x, double y);

/// Crops, as cropImage / cropGray do.
std::shared_ptr<Image16> cropImage(const Image16& image, int x, int y, int width, int height);
std::shared_ptr<Gray16> cropGray(const Gray16& image, int x, int y, int width, int height);

/// Half-open bounds of the nonzero samples, as nonzeroBounds does at 8 bits.
PixelBounds nonzeroBounds(const Gray16& image);
/// Half-open bounds of the pixels with any alpha, as alphaBounds does at 8 bits.
PixelBounds alphaBounds(const Image16& image);

/// A fingerprint of a 16-bit layer's pixels or mask (null: none), for the PSD carry's content binding.
uint64_t contentHash(const Image16* image);
uint64_t contentHash(const Gray16* image);

/// Any buffer at `type` (U8 or U16): converted when its depth differs, shared otherwise.
AnyImage imageAtDepth(const AnyImage& image, SampleType type);
AnyGray grayAtDepth(const AnyGray& image, SampleType type);
/// A colour buffer of `mode` at `type` (U8 or U16): 8-bit CMYK is `ImageC8`, 16-bit CMYK a 5-channel `Image16`; Lab
/// keeps a and b neutral across depths (128 at 8 bits is 16384 at 16; one 8-bit step of a or b is 128 16-bit steps).
/// Shared when it is already there; null when the buffer's channel count is not the mode's.
AnyImage imageAtFormat(const AnyImage& image, SampleType type, ColorMode mode);
/// 8-bit CMYK widened to a 5-channel 16-bit buffer, and back (each sample as widen8 / narrow16).
std::shared_ptr<Image16> widenImageC8(const ImageC8& image);
std::shared_ptr<ImageC8> narrowImageC8(const Image16& image);

} // namespace compositor
