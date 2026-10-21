#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
namespace wowee::rendering {
// An ADT placement may be mentioned by several tiles, including tiles at
// different detail levels. The last resident reference owns its destruction.
class TerrainPlacementRefs {
    struct Entry { uint32_t uid, refs; bool detailed=false; std::vector<uint32_t> children; };
    std::unordered_map<uint32_t,uint32_t> byUid_;
    std::unordered_map<uint32_t,Entry> byInstance_;
public:
    size_t count(uint32_t uid) const { return byUid_.count(uid); }
    uint32_t at(uint32_t uid) const { return byUid_.at(uid); }
    void insert(uint32_t uid,uint32_t instance) {
        byInstance_.emplace(instance,Entry{uid,1,false,{}});
        if(uid) byUid_.emplace(uid,instance);
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
        if(found->second.uid) byUid_.erase(found->second.uid);
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
