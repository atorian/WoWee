#pragma once
#include "rendering/frames_in_flight.hpp"
#include "core/logger.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {
// Logical identities outlive physical row positions and GPU frame slots.
struct RenderRecordKey {
    uint64_t lifetime=0, publication=0;
    bool operator==(const RenderRecordKey&) const = default;
};
struct RenderRecordHash {
    size_t operator()(const RenderRecordKey& key) const {
        const auto a=std::hash<uint64_t>{}(key.lifetime),b=std::hash<uint64_t>{}(key.publication);
        return a^(b+0x9e3779b97f4a7c15ull+(a<<6)+(a>>2));
    }
};
// Render-owner cache of immutable captured values. Each acquired GPU slot
// catches up independently, including changes made while that slot was skipped.
// Rows unused by the view are reclaimed; the CPU scene remains authoritative.
template<class Value> class VersionedRenderTable {
    static_assert(std::is_trivially_copyable_v<Value>);
    struct Record {
        RenderRecordKey key;
        Value value{};
        uint64_t version=0, seen=0;
    };
    std::unordered_map<RenderRecordKey,uint32_t,RenderRecordHash> indices_;
    std::vector<Record> records_;
    std::vector<uint32_t> free_,previous_,current_;
    std::array<std::vector<uint64_t>,MAX_FRAMES_IN_FLIGHT> uploaded_;
    uint64_t epoch_=0,version_=0;
    size_t capacity_=0,limit_=0,writes_=0;
    uint32_t slot_=0;
    bool open_=false;
    static void require(bool ok,const char* text) {
        if(ok)return;
        LOG_ERROR("Versioned render table: ",text,"; frame aborted");
        throw std::runtime_error(text);
    }
public:
    size_t begin(uint32_t slot,size_t maximumWrites) {
        require(slot<uploaded_.size(),"invalid frame slot");
        // A cancelled recording may have copied bytes without flushing them.
        // Never treat those versions as resident on the next acquired slot.
        if(open_)invalidate(slot_);
        finish();
        if(!maximumWrites) {
            indices_.clear();records_.clear();free_.clear();previous_.clear();current_.clear();
            // Release a departed view's CPU storage as well as its identities.
            decltype(indices_){}.swap(indices_);decltype(records_){}.swap(records_);
            decltype(free_){}.swap(free_);decltype(previous_){}.swap(previous_);decltype(current_){}.swap(current_);
            invalidateAll();
        } else if(records_.size()>256 && indices_.size()<records_.size()/3) {
            std::vector<Record> compact;compact.reserve(indices_.size());previous_.clear();
            for(auto& [key,index]:indices_) {
                compact.push_back(records_[index]);index=uint32_t(compact.size()-1);previous_.push_back(index);
            }
            records_=std::move(compact);free_.clear();invalidateAll();
        }
        slot_=slot;limit_=maximumWrites;writes_=0;++epoch_;open_=true;
        const size_t newRows=maximumWrites>free_.size()?maximumWrites-free_.size():0;
        capacity_=records_.size()+newRows;
        records_.reserve(capacity_);indices_.reserve(indices_.size()+newRows);
        current_.clear();current_.reserve(maximumWrites);
        uploaded_[slot].resize(capacity_,0);
        return capacity_;
    }
    void invalidate(uint32_t slot) { std::fill(uploaded_[slot].begin(),uploaded_[slot].end(),0); }
    void invalidateAll() { for(auto& versions:uploaded_)std::vector<uint64_t>{}.swap(versions); }
    template<class Upload> uint32_t put(RenderRecordKey key,const Value& value,Upload&& upload) {
        require(open_ && writes_<limit_,"snapshot reservation exhausted");++writes_;
        require(key.lifetime!=0,"missing lifetime identity");
        auto at=indices_.find(key);uint32_t row;
        if(at==indices_.end()) {
            if(free_.empty()) {row=uint32_t(records_.size());records_.emplace_back();}
            else {row=free_.back();free_.pop_back();records_[row]=Record{};}
            indices_.emplace(key,row);records_[row].key=key;
        } else row=at->second;
        require(row<capacity_,"row exceeds reservation");
        auto& record=records_[row];
        if(!record.version || std::memcmp(&record.value,&value,sizeof(Value))) {
            require(record.seen!=epoch_,"one captured identity has conflicting draw values");
            record.value=value;record.version=++version_;
        }
        if(record.seen!=epoch_) {current_.push_back(row);record.seen=epoch_;}
        if(uploaded_[slot_][row]!=record.version) {
            upload(row,record.value);uploaded_[slot_][row]=record.version;
        }
        return row;
    }
    void finish() {
        if(!open_)return;
        for(const auto row:previous_)if(records_[row].seen!=epoch_) {
            indices_.erase(records_[row].key);free_.push_back(row);
        }
        previous_.swap(current_);current_.clear();open_=false;
    }
    size_t live() const { return indices_.size(); }
};
}
