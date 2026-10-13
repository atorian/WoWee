// The asset manager decodes PNG overrides through stb_image, whose code each
// binary compiles once; the client's copy is in its loading screen.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
