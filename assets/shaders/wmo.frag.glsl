#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif
#include "wmo_frame.glsli"
#include "wmo_forward.glsli"
