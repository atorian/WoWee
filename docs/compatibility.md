# Compatibility: the reference platform

User decision (2026-11-03). The client must run on the machine below. A change
that breaks it there is broken, whatever it does elsewhere: a step proven only
on the Mac (MoltenVK) is not done until it has run here.

## The device

| | |
|---|---|
| GPU | AMD Radeon RX 7700 XT (RDNA 3), vendor `0x1002`, device `0x747e`, 12 GB |
| Driver | AMD Software (Adrenalin), Windows driver 32.0.32015.7029 of 2026-10-18; Vulkan driver 2.0.416 |
| Vulkan | instance and device API 1.4.359 |
| OS | Windows 11 Home 10.0.26200 |

The client logs the same line at start
(`Vulkan device: AMD Radeon RX 7700 XT vendor=0x1002 device=0x747e driver=2.0.416 api=1.4.359`),
so a report from another machine can be compared against this one.

What the renderer asks of a device is decided in `VkContext::selectPhysicalDevice`
and `createLogicalDevice` (`src/rendering/vk_context.cpp`). This device provides
everything there, ray queries included. It offers neither
`VK_NV_device_diagnostic_checkpoints` nor, by default, a validation layer, so a
lost device here is reported through `VK_AMD_buffer_marker`: the log names the
last pass the GPU started and the last it finished.

## The toolchain and libraries

The build on this machine is MSYS2 `mingw64` (see
[the build guide](../BUILD_INSTRUCTIONS.md)). The versions it is known to work
with:

| Package | Version |
|---|---|
| gcc (`mingw-w64-x86_64-gcc`) | 16.2.0 |
| CMake | 4.4.4 |
| Ninja | 1.13.2 |
| `mingw-w64-x86_64-vulkan-headers`, `-vulkan-loader` | 1.4.363.0 |
| `mingw-w64-x86_64-shaderc` (glslc) | 2026.4 |
| `mingw-w64-x86_64-glslang` | 16.6.0 |
| `mingw-w64-x86_64-spirv-tools`, `-spirv-cross` | 1.4.363.0 |
| `mingw-w64-x86_64-sdl3` | 3.4.18 |
| `mingw-w64-x86_64-glm` | 1.0.3 |
| `mingw-w64-x86_64-ffmpeg` | 9.0.2 |
| `mingw-w64-x86_64-openssl` | 3.6.5 |

The Vulkan loader the game runs against is the one the AMD driver installs;
the MSYS2 loader above is what the build links.

For diagnosing GPU faults, install the Khronos validation layer of the same SDK
version as the headers and run with `WOWEE_VULKAN_VALIDATION=1`:

```bash
pacman -S mingw-w64-x86_64-vulkan-validation-layers
```

It is not installed on the reference machine as of 2026-11-03.

## What is checked here

- A change to `wowee_render` or a shader runs the GPU tests that cover it
  (`ctest -C gpu -R <test>`) on this device before it is merged, as AGENTS.md
  "Verifying a change" says, and its commit says so.
- A newer driver is taken as it comes. When one changes what the client does,
  the driver version goes in this file with what changed.
