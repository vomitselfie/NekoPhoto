// The refinement of a subject mask that the Remove Background panel offers: a guided filter for hair,
// matting in a band around the edge, speckle cleanup, edge shift and contrast, and foreground colour
// estimation. Plain pixel math, always available (the models that make the mask are in subject.h).
#pragma once
#include "image.h"
#include "imaget.h"
#include <memory>
#include <vector>

namespace compositor {

/// A float opacity plane, 0..1, that the refinement stages hand one another so that no stage quantises
/// another's work. It lives only inside the pipeline: the result is committed to a mask once, at the end,
/// at the document's depth.
struct AlphaPlane {
    int width = 0, height = 0;
    std::vector<float> values;
    AlphaPlane() = default;
    AlphaPlane(int w, int h, float value = 0) : width(w), height(h), values(size_t(w) * size_t(h), value) {}
    explicit AlphaPlane(const GrayImage& mask);
    explicit AlphaPlane(const Gray16& mask);
    bool isEmpty() const { return width <= 0 || height <= 0; }
    float at(int x, int y) const { return values[size_t(y) * size_t(width) + size_t(x)]; }
    float& at(int x, int y) { return values[size_t(y) * size_t(width) + size_t(x)]; }
    /// Rounded once to 0..255, or to 16-bit's 0..32768.
    std::shared_ptr<GrayImage> toGray() const;
    std::shared_ptr<Gray16> toGray16() const;
    /// The mask at a document's depth: 8 or 16 bits (float documents get the plane's values clamped to 0..1).
    AnyGray commit(SampleType depth) const;
};

/// How stored colour values encode light, so that the matting solve can work in linear light: decode after
/// unpremultiplying, solve, encode. sRGB for now; colour management will hand the document's own curve.
/// `Identity` solves on the stored values as they are.
struct TransferCurve {
    enum class Kind : uint8_t { Srgb, Gamma, Identity };
    Kind kind = Kind::Srgb;
    float gamma = 2.2f;   // Kind::Gamma: encoded = linear^(1/gamma)
    static TransferCurve srgb() { return {}; }
    static TransferCurve identity() { return {Kind::Identity, 1.0f}; }
    static TransferCurve power(float g) { return {Kind::Gamma, g}; }
    bool isIdentity() const { return kind == Kind::Identity; }
};
/// Encoded 0..1 to linear light, and back; both clamp to 0..1.
float toLinear(const TransferCurve& curve, float encoded);
float fromLinear(const TransferCurve& curve, float linear);

struct MatteSettings {
    /// How far the mask is pulled onto the image's own edges (0 off, in layer pixels), 0..40.
    double refineEdges = 12;
    /// Pushes the mask's grays toward black and white, 0..100.
    double contrast = 25;
    /// Contracts (negative) or expands (positive) the edge, in layer pixels, -10..10.
    double shiftEdge = 0;
    /// Solves the true opacity of hair and fur in a band this wide (layer pixels) around the edge from
    /// foreground and background colour samples; 0 off, 0..400 (a few percent of the short side is usual).
    double matting = 0;
    /// Half-transparent regions that touch no edge are speckle: the ones enclosed by the subject become
    /// opaque, the ones floating in the background transparent (`cleanMatte`).
    bool cleanup = true;
    /// The edge pixels' colours are replaced by the subject's own colour, so no rim of the old background
    /// tints them over a new one (`estimateForeground`; Photoshop's Decontaminate Colors).
    bool decontaminate = true;
    // Variants judged by the matte_tool harness; off unless adopted.
    bool highPass = false;     // the guided filter on Gaussian high-passed signals (Zhao & He 2025)
    bool sideWindows = false;  // side-window coefficients where the mask is soft (Yin, Gong & Qiu 2019)
    bool narrowBand = false;   // band pixels whose colour clearly belongs to one side are decided before matting
    /// Matting and foreground estimation decode the colours with this curve and solve the compositing equation in
    /// linear light (the guided filter stays on the stored values). Identity, the default, solves on the stored
    /// values: linear light won every synthetic composite made in linear light but lost on AIM-500's photographs
    /// (mean SAD 60.17 against 59.63; matte_tool eval --linear), so it waits for evidence on real images.
    TransferCurve decode = TransferCurve::identity();
    MatteSettings normalized() const;
};

/// Speckle cleanup, in place: a half-transparent region must touch the matte's edge. One enclosed by
/// foreground becomes opaque, one floating in the background transparent; regions touching both stay.
void cleanMatte(GrayImage& matte);
void cleanMatte(AlphaPlane& matte);

/// `image` with the colour of every half-transparent matte pixel (and its transparent neighbours) replaced by
/// the estimated pure subject colour, from Germer et al.'s multi-level foreground estimation: per pixel the
/// colour is explained as alpha * F + (1 - alpha) * B with F and B smooth where alpha is, coarse to fine.
/// Composited with the matte over any background, no rim of the old one shows. Opaque pixels keep their
/// colours, and the image's own alpha is kept.
/// The solve runs in linear light when `decode` is a real curve (see TransferCurve); on the stored values by default.
std::shared_ptr<Image> estimateForeground(const Image& image, const GrayImage& matte, const TransferCurve& decode = TransferCurve::identity());
std::shared_ptr<Image> estimateForeground(const Image& image, const AlphaPlane& matte, const TransferCurve& decode = TransferCurve::identity());

/// Guided filtering (He, Sun & Tang): `mask` pulled onto the edges of `guide` (the layer's pixels), on a copy no
/// larger than `limit` on its longest side (0 for full size).
std::shared_ptr<GrayImage> guidedRefine(const GrayImage& mask, const Image& guide, double radius, int limit, bool highPass = false, bool sideWindows = false);
AlphaPlane guidedRefine(const AlphaPlane& mask, const Image& guide, double radius, int limit, bool highPass = false, bool sideWindows = false);
/// Matting within `band` pixels of the edge, by global sampling (He, Rhemann, Rother, Tang & Sun 2011): every
/// sure pixel near the band is a candidate, each unknown pixel searches the (foreground, background) pairs
/// PatchMatch-style for the one that explains its colour best and lies nearby, and the opacities are smoothed
/// by confidence and colour. The band is cut around `trimapFrom`'s edge when given (the model's own mask,
/// whose interior has no dips), else around `matte`'s; the values outside it come from `matte`.
/// The colours are decoded with `decode` first, so the pairs are mixed in linear light.
struct MatteDebug;
std::shared_ptr<GrayImage> matteBand(const GrayImage& matte, const Image& guide, double band, int limit, const GrayImage* trimapFrom = nullptr, MatteDebug* debug = nullptr, bool narrow = false, const TransferCurve& decode = TransferCurve::identity());
AlphaPlane matteBand(const AlphaPlane& matte, const Image& guide, double band, int limit, const AlphaPlane* trimapFrom = nullptr, MatteDebug* debug = nullptr, bool narrow = false, const TransferCurve& decode = TransferCurve::identity());

/// What `matteBand` saw and chose, at its working size, for the matte tool: the trimap (0 background, 128
/// unknown, 255 foreground), the foreground and background colours chosen for every band pixel, and the
/// opacity those pairs imply before smoothing.
/// `uncertainty` (full size, 0 outside the band) is how badly the two-colour model explains each band pixel
/// with its final opacity: |C - (aF + (1 - a)B)| / (|F - B| + eps), in the solve's space. High where the
/// samples are wrong, at three-region junctions, over translucency and spill. Not an opacity.
struct MatteDebug {
    std::shared_ptr<GrayImage> trimap, pairAlpha;
    std::shared_ptr<Image> chosenF, chosenB;
    AlphaPlane uncertainty;
};
/// The epsilon of the uncertainty's normalisation (linear-light units).
constexpr float uncertaintyEpsilon = 0.05f;
/// The panel's controls applied in order: refine, matting, cleanup, shift edge, contrast.
/// Every stage works on the float plane; the 8-bit form rounds once at the end.
std::shared_ptr<GrayImage> refineMatte(const GrayImage& mask, const Image& guide, const MatteSettings& settings, int limit);
AlphaPlane refineMatte(const AlphaPlane& mask, const Image& guide, const MatteSettings& settings, int limit, MatteDebug* debug = nullptr);

} // namespace compositor
