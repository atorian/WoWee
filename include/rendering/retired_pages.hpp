#pragma once

// The pages of a pool of GPU buffers - SkeletonPool's blobs, ParticlePool's
// records - let go whole: each page's buffer destroyed once no frame in
// flight can still read it.

#include "rendering/vk_utils.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace wowee {
namespace rendering {

/// Takes `pages`, leaving it empty; a page is anything holding an
/// AllocatedBuffer `buffer`, and a null one is skipped. `Context` is the
/// VkContext, a parameter here so this header needs none of it.
template <class Context, class Page>
void retirePages(Context& context, std::vector<std::shared_ptr<Page>>& pages) {
    context.deferAfterAllFrameFences([allocator = context.getAllocator(), retired = std::move(pages)] {
        for (const auto& page : retired) {
            if (page) destroyBuffer(allocator, page->buffer);
        }
    });
    pages.clear();
}

} // namespace rendering
} // namespace wowee
