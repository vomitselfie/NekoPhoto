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
    MatteSettings normalized() const;
};

/// Guided filtering (He, Sun & Tang): `mask` pulled onto the edges of `guide` (the layer's pixels), on a copy no
/// larger than `limit` on its longest side (0 for full size).
std::shared_ptr<GrayImage> guidedRefine(const GrayImage& mask, const Image& guide, double radius, int limit);
/// The panel's three controls applied in order: refine, shift edge, contrast.
std::shared_ptr<GrayImage> refineMatte(const GrayImage& mask, const Image& guide, const MatteSettings& settings, int limit);

} // namespace compositor
