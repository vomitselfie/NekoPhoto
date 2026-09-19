// Remove Background: a subject mask from a segmentation model (IS-Net / U2Net
// ONNX through OpenCV's DNN module), then the refinement the Mac panel offers
// (a guided filter for hair, matte contrast, edge shift). The model part is
// optional at build time (COMPOSITOR_HAVE_OPENCV); the refinement is plain
// pixel math and always available.
#pragma once
#include "image.h"
#include <memory>
#include <string>

namespace compositor {

/// Whether this build can run a segmentation model.
bool subjectModelSupported();

/// The subject, white, at `image`'s size, from the ONNX model at `modelPath` (rembg's isnet-general-use or a
/// u2net variant, told apart by name). Null with `error` set when the model can't be loaded or run.
std::shared_ptr<GrayImage> subjectMask(const Image& image, const std::string& modelPath, std::string* error);

struct MatteSettings {
    /// How far the mask is pulled onto the image's own edges (0 off, in layer pixels), 0..40.
    double refineEdges = 12;
    /// Pushes the mask's grays toward black and white, 0..100.
    double contrast = 25;
    /// Contracts (negative) or expands (positive) the edge, in layer pixels, -10..10.
    double shiftEdge = 0;
    /// Solves the true opacity of hair and fur in a band this wide (layer pixels) around the edge from
    /// foreground and background colour samples; 0 off, 0..40.
    double matting = 0;
    MatteSettings normalized() const;
};

/// Guided filtering (He, Sun & Tang): `mask` pulled onto the edges of `guide` (the layer's pixels), on a copy no
/// larger than `limit` on its longest side (0 for full size).
std::shared_ptr<GrayImage> guidedRefine(const GrayImage& mask, const Image& guide, double radius, int limit);
/// Matting within `band` pixels of the matte's edge (Gastal & Oliveira's shared sampling): each pixel's
/// opacity is solved from foreground and background colours found along rays into the sure regions, the
/// best-explaining pairs are shared between neighbours, and the result is smoothed by confidence and colour.
std::shared_ptr<GrayImage> matteBand(const GrayImage& matte, const Image& guide, double band, int limit);
/// The panel's controls applied in order: refine, matting, shift edge, contrast.
std::shared_ptr<GrayImage> refineMatte(const GrayImage& mask, const Image& guide, const MatteSettings& settings, int limit);

} // namespace compositor
