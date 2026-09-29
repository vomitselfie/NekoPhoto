// The Camera Raw kernels over premultiplied RGBA8 (CameraRawPixelsBody.inc).
#include "LensPixels.h"
#include <stdint.h>
#define CR_PIXEL uint8_t
#define CR_NAME(name) name
#define CR_ROUND(value) round(value)
#define CR_LENS_DISTORT lens_distort
#define CR_DECISION(value, alpha) ((void)(alpha), (value))
#include "CameraRawPixelsBody.inc"
