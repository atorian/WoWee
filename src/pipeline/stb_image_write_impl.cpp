// stb_image_write's code, compiled once, beside stb_image's. The renderer
// writes screenshots through it, the extractor and the asset manager their
// PNGs, the editor and blp_convert theirs. Each of those compiled its own
// copy, and the client, which has two of them, had to be told by a macro
// which one was the copy.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
