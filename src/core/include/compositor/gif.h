// Animated GIF: every frame opens as its own layer, as the frame shows once the ones before it are drawn and
// disposed (so each layer is a whole picture), named "Frame N (D ms)". Frames stack in order, frame 1 at the
// bottom; only frame 1 is visible, so the canvas shows what a viewer shows first. Still GIFs keep opening as
// plain images (through Qt) and never come here. Written for NekoPhoto from the GIF89a specification (Patchy
// only writes GIFs). The document's frames (animation.h) are set up too: frame N shows layer N alone, with the GIF's
// delays and loop count.
#pragma once
#include "psd.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

/// How many frames (image descriptors) the GIF holds; 0 when it is not a GIF or is damaged before the first.
int gifFrameCount(const std::vector<uint8_t>& bytes);
int gifFrameCount(const std::string& path);

std::optional<PsdImport> importGifBytes(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
std::optional<PsdImport> importGif(const std::string& path, std::string* error = nullptr);

// Writing: encodeGif makes an animated GIF from flattened frames, each quantised to its own palette (the exact
// colours when there are at most 255, else a median cut) with alpha below 128 transparent. Its LZW encoder
// follows Patchy's (MIT, src/formats/gif_document_io.cpp).
struct GifEncodeFrame {
    const Image* image = nullptr;   // premultiplied RGBA; every frame the same size
    int delayMs = 100;              // rounded to hundredths of a second
};
/// An animated GIF (one frame makes a still one). loopCount as Animation::loopCount: 0 forever, 1 once (no
/// NETSCAPE2.0 block), n plays n times. Empty with `error` set when the frames cannot be written.
std::vector<uint8_t> encodeGif(const std::vector<GifEncodeFrame>& frames, int loopCount, std::string* error = nullptr);
/// The document's frames as a GIF, or its composite when it has no frames.
std::vector<uint8_t> encodeDocumentGif(const Document& document, std::string* error = nullptr);
/// encodeDocumentGif written to a file.
bool writeDocumentGif(const std::string& path, const Document& document, std::string* error = nullptr);

} // namespace compositor
