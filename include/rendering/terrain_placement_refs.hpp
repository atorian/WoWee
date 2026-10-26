#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
namespace wowee::rendering {
// An ADT placement may be mentioned by several tiles, including tiles at
// different detail levels. The last resident reference owns its destruction.
class TerrainPlacementRefs {
    // Every uniqueId that resolved to the instance: the renderer hands back an
    // instance it already has for the same model at the same spot, so two
    // placements from neighbouring tiles can share one.
    struct Entry { std::vector<uint32_t> uids; uint32_t refs; bool detailed=false; std::vector<uint32_t> children; };
    std::unordered_map<uint32_t,uint32_t> byUid_;
    std::unordered_map<uint32_t,Entry> byInstance_;
public:
    size_t count(uint32_t uid) const { return byUid_.count(uid); }
    uint32_t at(uint32_t uid) const { return byUid_.at(uid); }
    // A tile's reference to a placement it created. The instance may be one
    // already held - the renderer deduplicated it - and is then one more
    // reference to it, under one more uniqueId. Before, the second placement
    // was mapped to the instance without being counted: the first tile to go
    // destroyed the instance and left the other uniqueId pointing at nothing,
    // which the next tile to retain it read through .at() and threw.
    void insert(uint32_t uid,uint32_t instance) {
        auto& entry=byInstance_.try_emplace(instance,Entry{{},0,false,{}}).first->second;
        ++entry.refs;
        if(uid && byUid_.emplace(uid,instance).second) entry.uids.push_back(uid);
    }
    uint32_t retain(uint32_t uid) {
        const uint32_t instance=byUid_.at(uid);
        ++byInstance_.at(instance).refs;
        return instance;
    }
    void attach(uint32_t instance,uint32_t child) { byInstance_.at(instance).children.push_back(child); }
    bool hasDetail(uint32_t instance) const {
        auto found=byInstance_.find(instance);
        return found!=byInstance_.end() && found->second.detailed;
    }
    void markDetail(uint32_t instance) {
        if(auto found=byInstance_.find(instance);found!=byInstance_.end()) found->second.detailed=true;
    }
    bool release(uint32_t instance) {
        auto found=byInstance_.find(instance);
        if(found==byInstance_.end()) return true; // a non-ADT doodad owned solely by this tile
        if(--found->second.refs) return false;
        for(auto uid:found->second.uids) byUid_.erase(uid);
        byInstance_.erase(found);
        return true;
    }
    std::vector<uint32_t> release(const std::vector<uint32_t>& instances, std::vector<uint32_t>* orphaned=nullptr) {
        std::vector<uint32_t> dead;
        for(auto id:instances) {
            const auto found=byInstance_.find(id);
            if(orphaned && found!=byInstance_.end() && found->second.refs==1)
                orphaned->insert(orphaned->end(),found->second.children.begin(),found->second.children.end());
            if(release(id)) dead.push_back(id);
        }
        return dead;
    }
};
}
