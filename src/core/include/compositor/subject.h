// Remove Background and Quick Select's click engine: subject masks from segmentation and prompt models
// (IS-Net / U2Net / EfficientSAM ONNX through OpenCV's DNN module), with the native-resolution detail pass.
// Optional at build time (COMPOSITOR_HAVE_OPENCV); the refinement that follows is in matte.h.
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

} // namespace compositor
