#pragma once

/// What a shader include needs to compile as C++, so the renderer and the
/// tests run the very functions the shaders do rather than a copy of them:
/// GLSL's vector types and its built-in functions, in single precision as
/// the GPU has them. The include goes in a namespace inside this one -
/// `namespace wowee::rendering::glsl { namespace { #include ... } }` - so
/// these hide the C library's double ones, which a `using namespace` would
/// leave beside them.

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace wowee::rendering::glsl {

using uint = std::uint32_t;
using vec2 = glm::vec2;
using vec3 = glm::vec3;
using vec4 = glm::vec4;
using ivec2 = glm::ivec2;
using glm::dot;
using glm::length;
using glm::normalize;

// Float only, as GLSL's: a literal such as 0.56 is a double in C++, and with
// these alone in view it is taken as the float the shader has.
inline float max(float a, float b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float clamp(float x, float lo, float hi) { return min(max(x, lo), hi); }
inline float abs(float x) { return std::fabs(x); }
inline float floor(float x) { return std::floor(x); }
inline float sqrt(float x) { return std::sqrt(x); }
inline float exp(float x) { return std::exp(x); }
inline float log(float x) { return std::log(x); }
inline float pow(float x, float y) { return std::pow(x, y); }
inline float sin(float x) { return std::sin(x); }
inline float cos(float x) { return std::cos(x); }
inline float acos(float x) { return std::acos(x); }
inline float cosh(float x) { return std::cosh(x); }
inline float tanh(float x) { return std::tanh(x); }

}  // namespace wowee::rendering::glsl
