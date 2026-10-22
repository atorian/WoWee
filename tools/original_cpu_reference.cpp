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

// FSINCOS is also CPU-defined, so let the movement oracle check Unicorn's
// result against the host instruction. Preserve the caller's control word.
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
EXPORT uint64_t original_fsincos(uint32_t input) {
    float angle, sine, cosine;
    std::memcpy(&angle,&input,sizeof(angle));
    uint16_t saved, original=0x27f;
    asm volatile("fnstcw %0" : "=m"(saved));
    asm volatile("fldcw %2\n\tflds %3\n\tfsincos\n\tfstps %0\n\tfstps %1\n\tfldcw %4"
        : "=m"(cosine),"=m"(sine) : "m"(original),"m"(angle),"m"(saved) : "memory","st","st(1)");
    uint32_t s,c;
    std::memcpy(&s,&sine,sizeof(s));std::memcpy(&c,&cosine,sizeof(c));
    return uint64_t(s)|(uint64_t(c)<<32);
}
#endif
