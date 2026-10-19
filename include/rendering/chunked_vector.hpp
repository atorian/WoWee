#pragma once

#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace wowee::rendering {

/// A sequence that never moves what it holds: elements live in chunks of
/// `ChunkSize`, each reserved once, so adding one never relocates the others.
///
/// The M2 renderer's instance list was a std::vector of 656-byte instances,
/// a hundred and thirty thousand of them in a city zone. Each time it
/// outgrew its capacity it moved every one of them in a single push_back -
/// 11 ms at 65,536, twice that at the next doubling - on whichever frame a
/// tile's doodads happened to cross the line. Indexed access, swap-remove
/// (move the last into the hole, pop it) and a walk in order are what the
/// list is used for, and those are kept; contiguity is not, and nothing used
/// it.
///
/// A reference to an element stays valid until that element is removed.
template <class T, size_t ChunkSize = 1024>
class ChunkedVector {
public:
    ChunkedVector() = default;
    ChunkedVector(ChunkedVector&&) noexcept = default;
    ChunkedVector& operator=(ChunkedVector&&) noexcept = default;
    ChunkedVector(const ChunkedVector&) = delete;
    ChunkedVector& operator=(const ChunkedVector&) = delete;

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }

    T& operator[](size_t i) { return chunks_[i / ChunkSize][i % ChunkSize]; }
    const T& operator[](size_t i) const { return chunks_[i / ChunkSize][i % ChunkSize]; }
    T& front() { return (*this)[0]; }
    const T& front() const { return (*this)[0]; }
    T& back() { return (*this)[size_ - 1]; }
    const T& back() const { return (*this)[size_ - 1]; }

    void push_back(T&& value) { emplace_back(std::move(value)); }
    void push_back(const T& value) { emplace_back(value); }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ / ChunkSize == chunks_.size()) {
            // A new chunk, reserved whole, so it never reallocates either.
            chunks_.emplace_back();
            chunks_.back().reserve(ChunkSize);
        }
        T& placed = chunks_[size_ / ChunkSize].emplace_back(std::forward<Args>(args)...);
        ++size_;
        return placed;
    }

    void pop_back() {
        --size_;
        auto& chunk = chunks_[size_ / ChunkSize];
        chunk.pop_back();
        // Keep one empty chunk past those in use, so a list swinging across
        // a chunk boundary does not allocate and free on every step.
        const size_t inUse = (size_ + ChunkSize - 1) / ChunkSize;
        while (chunks_.size() > inUse + 1) chunks_.pop_back();
    }

    void clear() {
        chunks_.clear();
        size_ = 0;
    }

    /// The memory its storage holds - the chunks whole, filled or not - not
    /// what the elements own beyond themselves.
    [[nodiscard]] size_t storageBytes() const {
        size_t total = chunks_.capacity() * sizeof(std::vector<T>);
        for (const auto& chunk : chunks_) total += chunk.capacity() * sizeof(T);
        return total;
    }

    template <class Owner, class Value>
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = Value*;
        using reference = Value&;

        Iterator(Owner* owner, size_t index) : owner_(owner), index_(index) {}
        reference operator*() const { return (*owner_)[index_]; }
        pointer operator->() const { return &(*owner_)[index_]; }
        Iterator& operator++() {
            ++index_;
            return *this;
        }
        Iterator operator++(int) {
            Iterator before = *this;
            ++index_;
            return before;
        }
        bool operator==(const Iterator& other) const { return index_ == other.index_; }
        bool operator!=(const Iterator& other) const { return index_ != other.index_; }

    private:
        Owner* owner_;
        size_t index_;
    };
    using iterator = Iterator<ChunkedVector, T>;
    using const_iterator = Iterator<const ChunkedVector, const T>;

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, size_); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size_); }

private:
    std::vector<std::vector<T>> chunks_;
    size_t size_ = 0;
};

}  // namespace wowee::rendering
