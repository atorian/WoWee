// CPU instructions whose specified approximation depends on the actual CPU.
// Used only by the offline emulator, never by the client or expected C++ math.
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>
#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif
EXPORT uint32_t original_rsqrtss(uint32_t input) {
    float value;
    std::memcpy(&value, &input, sizeof(value));
    value = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(value)));
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
