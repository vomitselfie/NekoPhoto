// Camera RAW files (CR2, CR3, NEF, ARW, RAF, ORF, RW2, DNG, ...) developed through LibRaw (LGPL 2.1 / CDDL 1.0):
// demosaiced with the camera's white balance into sRGB, 8 bits a channel, turned upright. Optional: builds without
// LibRaw refuse them. Filter ▸ Camera Raw then works on the developed pixels.
#pragma once
#include "image.h"
#include <memory>
#include <string>

namespace compositor {

/// Whether this build reads RAW files.
bool rawSupported();
/// Whether `path`'s extension is a camera RAW format LibRaw reads.
bool isRawPath(const std::string& path);
/// The developed image (premultiplied RGBA, opaque); null with `error` when the file cannot be read.
std::shared_ptr<Image> decodeRaw(const std::string& path, std::string* error);

} // namespace compositor
