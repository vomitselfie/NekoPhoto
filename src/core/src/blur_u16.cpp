// The blurs at 16 bits (blur.h): the 8-bit kernels' templates instantiated for 0..32768 samples.
#include "blur_impl.h"

namespace compositor {

void gaussianBlur(Image16& image, double sigma) { gaussianBlurImpl(image, sigma); }
void gaussianBlur(Gray16& image, double sigma) { gaussianBlurImpl(image, sigma); }
void motionBlur(Image16& image, double distance, double angleDegrees) { motionBlurImpl(image, distance, angleDegrees); }

} // namespace compositor
