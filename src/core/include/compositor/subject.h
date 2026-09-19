// Remove Background: a subject mask from a segmentation model (IS-Net / U2Net
// ONNX through OpenCV's DNN module), then the refinement the Mac panel offers
// (a guided filter for hair, matte contrast, edge shift). The model part is
// optional at build time (COMPOSITOR_HAVE_OPENCV); the refinement is plain
// pixel math and always available.
#pragma once
#include "image.h"
#include <memory>
#include <string>
#include <vector>

namespace compositor {

/// Whether this build can run a segmentation model.
bool subjectModelSupported();

/// The subject, white, at `image`'s size, from the ONNX model at `modelPath` (rembg's isnet-general-use or a
/// u2net variant, told apart by name). Null with `error` set when the model can't be loaded or run.
/// With `flipAverage` the model also runs on the mirrored image and the two predictions are averaged: twice
/// the time, and on AIM-500 a lower error on most subjects (portraits and furniture most of all).
std::shared_ptr<GrayImage> subjectMask(const Image& image, const std::string& modelPath, std::string* error, bool flipAverage = false);

/// The detail pass: `subjectMask` (or `coarse`, when given) and then the same network again on windows of its
/// own input size cut from the layer at native resolution (half, above four times the input) wherever the
/// coarse mask is uncertain, at most `maxWindows` of them. Each window's output is calibrated to the coarse
/// mask where that is confident, the windows are blended with Hann weights, and the result is fused with
/// the coarse mask: its low frequencies, the windows' high ones, inside the uncertain band only. The coarse
/// mask comes back unchanged when the layer is not much larger than the model's input.
std::shared_ptr<GrayImage> subjectMaskDetailed(const Image& image, const std::string& modelPath, const GrayImage* coarse, int maxWindows, std::string* error, bool flipAverage = false);

/// A prompt for a click-to-select model: a point in image pixels with SAM's label, 1 for "part of the subject",
/// 0 for "not the subject", and 2 and 3 for the top-left and bottom-right corners of a box.
struct PointPrompt { double x = 0, y = 0; int label = 1; };

/// Whether the model file is a prompt (click-to-select) model rather than a whole-image segmenter, by name.
bool promptModelPath(const std::string& modelPath);

/// The subject the prompts point at, white, at `image`'s size, from a prompt model (EfficientSAM as OpenCV's
/// model zoo exports it: the image at 1024 px, up to six prompt slots, three candidate masks scored by the
/// model, the best one taken). Null with `error` when the model can't be loaded or run, or there is no prompt.
std::shared_ptr<GrayImage> subjectFromPrompts(const Image& image, const std::string& modelPath, const std::vector<PointPrompt>& prompts, std::string* error);

/// A window of the detail pass: `size` layer pixels square with its top-left corner at (x, y).
struct DetailWindow { int x = 0, y = 0, size = 0; };

/// At most `maxWindows` windows of `size` from a half-window grid, covering the nonzero pixels of `uncertain`
/// most-first; a window is only added while it covers at least `minPixels` still uncovered ones. Empty when
/// the image is smaller than a window.
std::vector<DetailWindow> detailWindows(const GrayImage& uncertain, int size, int maxWindows, int minPixels);

/// `coarse` with `local`'s detail, blended in by `weight` (0..255) and `coarse` alone where the weight is 0.
/// With `sigma` > 0 the detail is the high frequencies of `local` over the low ones of `coarse` (a Gaussian
/// of `sigma`). With `sigma` 0 the two are arbitrated by confidence: `local` where it is sure (near 0 or 1),
/// `coarse` where only the coarse mask is sure, `local` where neither is; a window's soft floor around the
/// subject then yields to the coarse pass's background, and the fur it found stays.
std::shared_ptr<GrayImage> fuseDetail(const GrayImage& coarse, const GrayImage& local, const GrayImage& weight, double sigma);

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
std::shared_ptr<GrayImage> guidedRefine(const GrayImage& mask, const Image& guide, double radius, int limit);
/// Matting within `band` pixels of the edge, by global sampling (He, Rhemann, Rother, Tang & Sun 2011): every
/// sure pixel near the band is a candidate, each unknown pixel searches the (foreground, background) pairs
/// PatchMatch-style for the one that explains its colour best and lies nearby, and the opacities are smoothed
/// by confidence and colour. The band is cut around `trimapFrom`'s edge when given (the model's own mask,
/// whose interior has no dips), else around `matte`'s; the values outside it come from `matte`.
struct MatteDebug;
std::shared_ptr<GrayImage> matteBand(const GrayImage& matte, const Image& guide, double band, int limit, const GrayImage* trimapFrom = nullptr, MatteDebug* debug = nullptr);

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
