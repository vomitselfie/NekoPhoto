// The blurs at 32 bits (blur.h): the 8-bit kernels' templates instantiated for premultiplied linear float, nothing
// rounded between the passes; colour above 1 is blurred as it is.
#include "blur_impl.h"

namespace compositor {

void gaussianBlur(ImageF& image, double sigma) { gaussianBlurImpl(image, sigma); }
void gaussianBlur(GrayF& image, double sigma) { gaussianBlurImpl(image, sigma); }
void motionBlur(ImageF& image, double distance, double angleDegrees) { motionBlurImpl(image, distance, angleDegrees); }

} // namespace compositor
