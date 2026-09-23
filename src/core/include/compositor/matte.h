// The refinement of a subject mask that the Remove Background panel offers: a guided filter for hair,
// matting in a band around the edge, speckle cleanup, edge shift and contrast, and foreground colour
// estimation. Plain pixel math, always available (the models that make the mask are in subject.h).
#pragma once
#include "image.h"
#include <memory>

namespace compositor {

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
    // Variants judged by the harness (docs/background-removal-review.md items 13 to 15); off unless adopted.
    bool highPass = false;     // the guided filter on Gaussian high-passed signals (Zhao & He 2025)
    bool sideWindows = false;  // side-window coefficients where the mask is soft (Yin, Gong & Qiu 2019)
    bool narrowBand = false;   // band pixels whose colour clearly belongs to one side are decided before matting
    MatteSettings normalized() const;
};

/// Speckle cleanup, in place: a half-transparent region must touch the matte's edge. One enclosed by
/// foreground becomes opaque, one floating in the background transparent; regions touching both stay.
void cleanMatte(GrayImage& matte);

/// `image` with the colour of every half-transparent matte pixel (and its transparent neighbours) replaced by
/// the estimated pure subject colour, from Germer et al.'s multi-level foreground estimation: per pixel the
/// colour is explained as alpha * F + (1 - alpha) * B with F and B smooth where alpha is, coarse to fine.
/// Composited with the matte over any background, no rim of the old one shows. Opaque pixels keep their
/// colours, and the image's own alpha is kept.
std::shared_ptr<Image> estimateForeground(const Image& image, const GrayImage& matte);

/// Guided filtering (He, Sun & Tang): `mask` pulled onto the edges of `guide` (the layer's pixels), on a copy no
/// larger than `limit` on its longest side (0 for full size).
std::shared_ptr<GrayImage> guidedRefine(const GrayImage& mask, const Image& guide, double radius, int limit, bool highPass = false, bool sideWindows = false);
/// Matting within `band` pixels of the edge, by global sampling (He, Rhemann, Rother, Tang & Sun 2011): every
/// sure pixel near the band is a candidate, each unknown pixel searches the (foreground, background) pairs
/// PatchMatch-style for the one that explains its colour best and lies nearby, and the opacities are smoothed
/// by confidence and colour. The band is cut around `trimapFrom`'s edge when given (the model's own mask,
/// whose interior has no dips), else around `matte`'s; the values outside it come from `matte`.
struct MatteDebug;
std::shared_ptr<GrayImage> matteBand(const GrayImage& matte, const Image& guide, double band, int limit, const GrayImage* trimapFrom = nullptr, MatteDebug* debug = nullptr, bool narrow = false);

/// What `matteBand` saw and chose, at its working size, for the matte tool: the trimap (0 background, 128
/// unknown, 255 foreground), the foreground and background colours chosen for every band pixel, and the
/// opacity those pairs imply before smoothing.
struct MatteDebug {
    std::shared_ptr<GrayImage> trimap, pairAlpha;
    std::shared_ptr<Image> chosenF, chosenB;
};
/// The panel's controls applied in order: refine, matting, cleanup, shift edge, contrast.
std::shared_ptr<GrayImage> refineMatte(const GrayImage& mask, const Image& guide, const MatteSettings& settings, int limit);

} // namespace compositor
