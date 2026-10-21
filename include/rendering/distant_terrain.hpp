#pragma once
#include "core/job_group.hpp"
#include "pipeline/distant_terrain.hpp"
#include "rendering/vk_utils.hpp"
#include <memory>
#include <string>
namespace wowee::pipeline { class AssetManager; }
namespace wowee::rendering {
class VkContext; class Camera;
class DistantTerrain {
    struct Gpu;
    struct Prepared;
public:
    struct Draw {
        uint32_t first, count;
        int32_t vertex;
        std::array<uint32_t,8> coverage{};
    };
    struct Frame { std::shared_ptr<const Gpu> data; std::vector<Draw> draws; };
    ~DistantTerrain();
    bool initialize(VkContext& ctx,VkDescriptorSetLayout perFrame);
    void shutdown();
    void setMap(pipeline::AssetManager* assets,const std::string& map);
    void update();
    bool hasSource() const { return source_; }
    bool ready() const { return bool(gpu_); }
    Frame extract(const Camera& camera,float distance,const pipeline::TerrainCoverage& coverage) const;
    void record(VkCommandBuffer cmd,VkDescriptorSet perFrame,const Frame& frame);
    void recreatePipeline();
private:
    VkContext* ctx_=nullptr;
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    bool source_=false;
    std::string map_;
    std::unique_ptr<core::JobGroup> jobs_;
    core::JobGroup::Future<std::shared_ptr<Prepared>> pending_;
    std::shared_ptr<Gpu> gpu_;
};
}
