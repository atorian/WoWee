#pragma once

// Rows of a table on the GPU (GpuSlotTable) as the main thread keeps them: a
// row given out, written, and handed back. A written row rides in every frame
// extracted after the write until one that carried it is on the GPU - the
// owner says which (applied) - and a row handed back is given out again only
// after the frames that may still read it.
//
// The M2 and WMO material tables are two: a batch's material is a row, read
// by its draws.

#include "rendering/vk_context.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace wowee {
namespace rendering {

template <class Row>
class GpuRowStore {
public:
    static constexpr uint32_t kNoRow = UINT32_MAX;

    /// A row for the caller; the table grows when none is free.
    uint32_t place() {
        {
            std::lock_guard lock(returned_->mutex);
            free_.insert(free_.end(), returned_->rows.begin(), returned_->rows.end());
            returned_->rows.clear();
        }
        if (!free_.empty()) {
            const uint32_t row = free_.back();
            free_.pop_back();
            return row;
        }
        data_.emplace_back();
        pendingSerial_.push_back(0);
        return static_cast<uint32_t>(data_.size() - 1);
    }

    /// The row as its readers will see it, from the next extracted frame on.
    void write(uint32_t row, const Row& value) {
        data_[row] = value;
        if (pendingSerial_[row] == 0) pending_.push_back(row);
        // Carried by the next extracted frame, whatever an earlier one carried.
        pendingSerial_[row] = kUncarried;
    }

    /// The row at `row`, a place its owner hands out itself (an instance's
    /// slot, a block of its own): written as write() writes, the store grown
    /// to hold it.
    void writeAt(uint32_t row, const Row& value) {
        if (row >= data_.size()) {
            data_.resize(row + 1);
            pendingSerial_.resize(row + 1, 0);
        }
        write(row, value);
    }

    /// The row back once no frame in flight can read it.
    void release(VkContext& context, uint32_t row) {
        if (row == kNoRow) return;
        context.deferAfterAllFrameFences([returned = returned_, row] {
            std::lock_guard lock(returned->mutex);
            returned->rows.push_back(row);
        });
    }

    [[nodiscard]] const Row& at(uint32_t row) const { return data_[row]; }
    /// How many rows the table needs room for.
    [[nodiscard]] uint32_t count() const { return static_cast<uint32_t>(data_.size()); }

    /// The rows the frame extracted now as `serial` carries, given that every
    /// frame up to `applied` is on the GPU: those written since, and those
    /// carried by a frame not yet applied.
    void carry(uint32_t serial, uint32_t applied, std::vector<uint32_t>& slots, std::vector<Row>& rows) {
        std::erase_if(pending_, [&](uint32_t row) {
            uint32_t& carried = pendingSerial_[row];
            if (carried == kUncarried) carried = serial;
            if (carried > applied) return false;
            carried = 0;
            return true;
        });
        slots.assign(pending_.begin(), pending_.end());
        rows.clear();
        rows.reserve(pending_.size());
        for (const uint32_t row : pending_) rows.push_back(data_[row]);
    }

    /// Every row forgotten, as when the table itself is.
    void reset() {
        data_.clear();
        free_.clear();
        pending_.clear();
        pendingSerial_.clear();
        returned_ = std::make_shared<Returned>();
    }

private:
    /// A row's pending serial while no extracted frame has carried it yet.
    static constexpr uint32_t kUncarried = UINT32_MAX;
    struct Returned {
        std::mutex mutex;
        std::vector<uint32_t> rows;
    };
    std::vector<Row> data_;                 ///< by row, as last written
    std::vector<uint32_t> free_;
    std::vector<uint32_t> pending_;         ///< rows written and not yet known on the GPU
    std::vector<uint32_t> pendingSerial_;   ///< by row: the frame that carries it, 0 if none
    std::shared_ptr<Returned> returned_ = std::make_shared<Returned>();
};

} // namespace rendering
} // namespace wowee
