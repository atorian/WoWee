# One texture cache under one video memory budget

Branch `worktree-agent-acb177fa37d9e48fd`, on top of the resource telemetry
and the Ctrl+` panel (not yet on master).

## Why

The client crashed twice in Dalaran (map 571):

1. `std::bad_alloc` with 10.8 GB of private commit; thirty seconds before, a
   thread could not be started ("Resource temporarily unavailable").
2. After a login the depth buffer was not created, the other frame targets
   were built at 0x0, and the AMD driver crashed in `vkCmdBeginRenderPass`
   from `Renderer::beginFrame`.

The card is an RX 7700 XT with 12 GB. Four texture caches had budgets
adding to 20 GB (M2 4096 MB, WMO 8192 MB, terrain 4096 MB, characters
4096 MB) and nothing read VK_EXT_memory_budget. Every WMO and character
texture got an RGBA8 normal/height map with mips; the character renderer
started a detached thread per texture to derive it. The asset file cache was
given three quarters of half the free RAM, up to 12 GB.

## Measurements

All offline, with `scene_render` around Dalaran (`tools/scene_render/scenes/
dalaran.json`, `WOWEE_LOG_LEVEL=info`, `WOWEE_GPU_MEMORY_DUMP=<file>` for the
allocator's map; `build-dev/scratch/vma_breakdown.py` groups it by allocating
source line). No realm, so no characters, players or creatures: these are the
world's share.

| Dalaran, device-local VMA | 3x3 tiles before | 3x3 after | 9x9 before |
|---|---:|---:|---:|
| in use (driver's figure) | 1070 MB | 809 MB | 1588 MB |
| textures | 590 MB | 434 MB | 1029 MB |
| - files as stored (BC1/2/3) | 366 MB | 362 MB | 599 MB |
| - WMO normal maps | 212 MB (RGBA8) | 55 MB (BC7) | 413 MB (RGBA8) |
| - terrain alpha maps in VRAM | 0.8 MB | 0.8 MB | - |
| geometry (vertex/index) | 252 MB | 252 MB | 448 MB |
| render targets | 75 MB | 75 MB | 75 MB |
| other buffers (bones, UBOs, staging) | 195 MB | 195 MB | 195 MB |

The terrain's 7,500 alpha maps (64x64 R8, 4 KB each, 29 MB) are mostly not
in VRAM at all: 7,291 of them sit in a host-visible memory type, in system
memory, where the host image copy path puts them. The texture cache charges
them against its budget all the same. Stormwind (`stormwind.json`): WMO
normal maps 86 MB against 14 MB of diffuse.

Budget: the driver gives the client 11075 MB of the 12016 MB heap.

### What was wrong with the old accounting

The WMO cache charged a texture with a normal map twice its diffuse size
(`approxBytes *= 2`); an RGBA8 map next to a DXT1 diffuse is eight times it.
In the 9x9 run the WMO cache reported 142 MB while its normal maps alone held
413 MB. The cache now charges every entry its VMA allocation's size.

### The normal map decision

Normal maps were 40% of texture memory around Dalaran and six times the WMO
diffuse in Stormwind - compress them. They are sampled as `.rgb` normal and
`.a` height from one binding, and parallax is on by default, so the height is
needed. BC7 keeps all four channels at one byte a texel; BC5 + BC4 would be
1.5 bytes and a second binding and shader change; BC3 would put the normal
in 5:6:5 colour endpoints. BC7 mode 6 (one subset, 7-bit RGBA endpoints with
parity, 4-bit indices) is encoded on the worker that derives the map, with a
CPU mip chain. Result: 212 -> 55 MB around Dalaran (3.8x), the scenes the
same to the eye (`build-dev/scratch/after/stormwind_crop.png`). A device
without BC7 keeps RGBA8 and says so once.

Not built everywhere: composited character skins never had one (luminance is
not height on skin); characters ask only for textures that are drawn, nearest
first, and none when normal mapping is off.

## What changed

1. **Telemetry** - VK_EXT_memory_budget enabled where present; the resource
   sample, log line and Ctrl+` panel show video memory against the driver's
   budget, broken down by usage flags from VMA's detailed map every 10 s
   (render targets, textures, geometry, ray tracing, other buffers), and the
   texture cache by form. Over 95% of the budget warns once a crossing.
2. **`rendering::TextureCache`** (owned by `VkContext`) - one entry per
   normalized path and form (stored file, decoded, normal map at a strength,
   composite, owned); `TextureRef` counted references held by terrain
   chunks, WMO and M2 models and character models/instances; LRU eviction of
   unreferenced entries, retired through `deferAfterAllFrameFences`; one
   budget, 4096 MB (`WOWEE_TEXTURE_CACHE_MB`) and at most half the driver's
   heap budget; a texture that does not fit is tried at a smaller mip, then
   refused (logged once a stretch) and drawn with the fallback. A texture's
   normal map is held by its entry (`setNormalMap`). Samplers: the cache's
   image carries a linear/repeat/anisotropic sampler; M2 batches, particles
   and ribbons bind their own from the slot's wrap flags and the sky's mip
   bias, so a path shared between renderers is one image. The character
   renderer's decoded+magenta-keyed form is its own key.
3. **Normal maps** - `NormalMapBuilder`: two workers per character renderer,
   a queue of 64, nearest first (a nearer request displaces the farthest);
   no thread per texture. WMO maps are made on the terrain workers as before.
   All BC7.
4. **Frame targets** - `FrameTargets` in `VkContext`: until the depth buffer,
   MSAA colour, depth resolve and framebuffers all exist, `beginFrame` gives
   no command buffer; failures log VkResult, size and heap budget once a
   stretch; `Renderer::beginFrame` returns when the rebuild fails.
5. **Asset file cache** - textures and DBC tables are read past it (their
   result is cached elsewhere); a fixed 1 GB budget (`WOWEE_FILE_CACHE_MB`).

## Still to do

- **Measure in the client, in Dalaran**: run this branch's
  `build-dev\bin\wowee.exe` with `WOW_DATA_PATH=D:\games\WoWee\Data`, stand
  five minutes in Dalaran with Ctrl+`, and read the video memory, private
  commit and thread lines (or the resource line in the log). The offline
  numbers have no characters; the character side (decoded RGBA8 diffuse,
  composites, normal maps) is what only the client shows.
- **M2 and WMO models (next step)** - geometry is 252-448 MB around Dalaran
  and grows with the streamed radius. What it needs: an owner like the
  texture cache for model GPU data (vertex/index buffers, bone and material
  buffers) keyed by model path, counted references from instances/tiles, one
  budget, retirement through the frame fences; the M2 `modelCacheLimit_`
  (6000) and WMO (4000) counts replaced by bytes; `cleanupUnusedModels`
  folded into it. The 16k+ small vertex buffers per 9x9 area are also worth
  sub-allocating from shared buffers.
- **Character textures by appearance** - character diffuse textures are kept
  decoded RGBA8 (four bytes a texel) because the magenta key and the
  composites are built from pixels. Uploading the files that need no keying
  as blocks, and composites as BC7 encoded on the worker that composites
  them, would cut them 4-8x. Composites are keyed by their full layer list,
  so two players dressed alike already share one.
- The terrain streaming radius (load 8 / unload 11 tiles, up to 529 resident)
  is what most of the geometry and the terrain alpha maps scale with; the
  texture budget now bounds the textures but not those.
