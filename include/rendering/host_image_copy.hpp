#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {

/// Whether an image made for host image copy (VK_EXT_host_image_copy) may still
/// live in the device memory the same image without it would be given.
///
/// The usage flag narrows the memory types an image may bind to. Where the
/// driver can only write from the host into memory the host can map, the
/// types left are system memory and, without resizable BAR, the 256 MB window
/// of video memory the CPU can see: textures uploaded that way were sampled
/// from system memory over the bus every frame, and once the small window
/// filled, every 4 KB terrain alpha map cost an allocation of its own.
///
/// The masks are memoryTypeBits, one bit per memory type: what the image may
/// bind to with the host-transfer usage and without it, and which types are
/// device-local. Host copy is worth having only if it gives up none of the
/// device-local types the image could otherwise use.
[[nodiscard]] constexpr bool hostCopyKeepsDeviceMemory(uint32_t typesWithHostTransfer,
                                                       uint32_t typesWithout,
                                                       uint32_t deviceLocalTypes) {
    const uint32_t wanted = typesWithout & deviceLocalTypes;
    return (typesWithHostTransfer & wanted) == wanted;
}

} // namespace rendering
} // namespace wowee
