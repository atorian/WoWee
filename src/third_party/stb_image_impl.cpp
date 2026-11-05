// stb_image's code, compiled once in wowee_image_codec. Asset loaders and
// the SDL window icon share it. Every binary that linked
// wowee_pipeline had to compile its own copy - the client in its loading
// screen, the editor, scene_render and blp_convert in one each - and one that
// forgot did not link.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
