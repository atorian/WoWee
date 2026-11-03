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

/// Lets go of the pages `empty` says hold nothing, but for one shared one (a
/// page made to the size of one blob, `fitted`, is not kept as the spare):
/// each is taken out of `pages` now and `destroy(allocator, page)` called on
/// it once no frame can read it. Returns whether any went.
///
/// Not at once, though every range in the page was already handed back
/// behind the frame fences: a frame binds a page whole - a bucket of draws
/// over its geometry, a pass's set of every page - so a frame extracted
/// after the last range went back still binds it. Freeing it then destroyed
/// buffers a submitted command buffer was using.
template <class Context, class Page, class Empty, class Destroy>
bool retireEmptyPages(Context& context, std::vector<std::shared_ptr<Page>>& pages, Empty empty, Destroy destroy) {
    std::vector<std::shared_ptr<Page>> retired;
    bool spareKept = false;
    for (auto& page : pages) {
        if (!page || !empty(*page)) continue;
        if (!spareKept && !page->fitted) {
            spareKept = true;
            continue;
        }
        retired.push_back(std::move(page));
    }
    while (!pages.empty() && !pages.back()) pages.pop_back();
    if (retired.empty()) return false;
    context.deferAfterAllFrameFences(
        [allocator = context.getAllocator(), retired = std::move(retired), destroy] {
            for (const auto& page : retired) destroy(allocator, *page);
        });
    return true;
}

} // namespace rendering
} // namespace wowee
