#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace wowee::rendering {

/// Producer writes copy-on-write pages; snapshots pin only the versions they
/// name. Each GPU slot tracks its own uploaded pages, so skipping a producer
/// frame never loses the dirty ranges the slot has not seen yet.
/// The caller waits for the destination slot's GPU readers before upload.
template<class T, size_t PageSize = 256>
class PagedFrameBuffer {
    struct Page {
        uint64_t version = 0;
        std::array<T,PageSize> values;
    };
public:
    struct Snapshot {
        uint64_t identity = 0;
        size_t size = 0;
        std::vector<std::shared_ptr<const Page>> pages;
        void clear() { pages.clear(); identity=0; size=0; }
    };
    struct Uploaded {
        uint64_t identity = 0;
        std::vector<uint64_t> versions;
        void clear() { identity=0; versions.clear(); }
    };

    explicit PagedFrameBuffer(T initial = {}) : initial_(std::move(initial)) {}
    PagedFrameBuffer(const PagedFrameBuffer&) = delete;
    PagedFrameBuffer& operator=(const PagedFrameBuffer&) = delete;
    PagedFrameBuffer(PagedFrameBuffer&& other) : PagedFrameBuffer(other.initial_) { swap(other); }
    PagedFrameBuffer& operator=(PagedFrameBuffer&& other) {
        if(this!=&other) { PagedFrameBuffer moved(std::move(other)); swap(moved); }
        return *this;
    }

    void write(size_t offset, std::span<const T> values) {
        if(values.empty()) return;
        size_=std::max(size_,offset+values.size());
        pages_.resize((size_+PageSize-1)/PageSize);
        while(!values.empty()) {
            const size_t pageIndex=offset/PageSize, within=offset%PageSize;
            auto& page=pages_[pageIndex];
            if(!page) { page=std::make_shared<Page>(); page->values.fill(initial_); }
            else if(!page.unique()) page=std::make_shared<Page>(*page);
            const size_t count=std::min(PageSize-within,values.size());
            std::copy_n(values.begin(),count,page->values.begin()+within);
            page->version=++version_;
            values=values.subspan(count); offset+=count;
        }
    }
    void publish(Snapshot& snapshot) const {
        snapshot.identity=identity_; snapshot.size=size_;
        snapshot.pages.assign(pages_.begin(),pages_.end());
    }

    /// Reports exactly the ranges that differ from this slot's last contents.
    /// Callbacks copy data while the snapshot is alive. Empty pages have never
    /// been written and no draw may address them.
    template<class Copy>
    static size_t upload(const Snapshot& snapshot, Uploaded& slot, Copy&& copy) {
        if(slot.identity!=snapshot.identity) {
            slot.versions.clear(); slot.identity=snapshot.identity;
        }
        slot.versions.resize(snapshot.pages.size());
        size_t elements=0;
        for(size_t i=0;i<snapshot.pages.size();++i) {
            const auto& page=snapshot.pages[i];
            if(!page || slot.versions[i]==page->version) continue;
            const size_t count=std::min(PageSize,snapshot.size-i*PageSize);
            copy(i*PageSize,std::span<const T>(page->values.data(),count));
            slot.versions[i]=page->version;
            elements+=count;
        }
        return elements;
    }
private:
    inline static std::atomic<uint64_t> nextIdentity_{1};
    void swap(PagedFrameBuffer& other) {
        using std::swap;
        swap(identity_,other.identity_); swap(version_,other.version_);
        swap(size_,other.size_); swap(initial_,other.initial_); swap(pages_,other.pages_);
    }
    uint64_t identity_=nextIdentity_.fetch_add(1,std::memory_order_relaxed);
    uint64_t version_=0;
    size_t size_=0;
    T initial_;
    std::vector<std::shared_ptr<Page>> pages_;
};

} // namespace wowee::rendering
