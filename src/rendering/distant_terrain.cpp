#include "rendering/distant_terrain.hpp"
#include "rendering/camera.hpp"
#include "rendering/view_distance.hpp"
#include "rendering/frustum.hpp"
#include <algorithm>
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <sstream>
#include <unordered_map>
#include <cstddef>
#include <cctype>
namespace wowee::rendering {
struct DistantTerrain::Prepared {
    struct Tile { int x,y; uint32_t vertex; std::array<pipeline::DistantMesh::Range,4> lods; glm::vec3 min,max; };
    std::vector<pipeline::DistantVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Tile> tiles;
};
struct DistantTerrain::Gpu {
    VkContext* ctx;
    AllocatedBuffer vertices,indices;
    std::vector<Prepared::Tile> tiles;
    ~Gpu() { DeferredResources retire(*ctx); retire.buffer(vertices);retire.buffer(indices); }
};
DistantTerrain::~DistantTerrain(){shutdown();}
bool DistantTerrain::initialize(VkContext& ctx,VkDescriptorSetLayout perFrame) {
    ctx_=&ctx;
    layout_=createPipelineLayout(ctx.getDevice(),{perFrame},{{VK_SHADER_STAGE_FRAGMENT_BIT,0,32}});
    recreatePipeline();
    if (!layout_ || !pipeline_) { LOG_ERROR("DistantTerrain: pipeline initialization failed - distant terrain unavailable");return false; }
    return true;
}
void DistantTerrain::recreatePipeline() {
    if(!ctx_ || !layout_)return;
    DeferredResources retire(*ctx_);retire.destroy(pipeline_);
    auto shader=loadShaderPair(ctx_->getDevice(),"assets/shaders/distant_terrain.vert.spv","assets/shaders/distant_terrain.frag.spv","distant terrain");
    if(!shader)return;
    using V=pipeline::DistantVertex;
    pipeline_=PipelineBuilder().setShaders(shader.vertStage,shader.fragStage)
        .setVertexInput({{0,sizeof(V),VK_VERTEX_INPUT_RATE_VERTEX}}, {
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(V,position)},
            {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(V,normal)},
            {2,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(V,color)},
            {3,0,VK_FORMAT_R32G32_SFLOAT,offsetof(V,uv)}})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL,VK_CULL_MODE_NONE)
        .setDepthTest(true,true,VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(ctx_->getMsaaSamples()).setLayout(layout_).setRenderPass(ctx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic()).build(ctx_->getDevice(),ctx_->getPipelineCache());
    if(!pipeline_) LOG_ERROR("DistantTerrain: graphics pipeline creation failed - distant ground not drawn");
}
void DistantTerrain::setMap(pipeline::AssetManager* assets,const std::string& map) {
    if(map_==map)return;
    if(jobs_) { jobs_->cancel();jobs_->wait(); }
    pending_={};gpu_.reset();map_=map;source_=false;
    if(!assets || map.empty())return;
    const auto path="World\\Maps\\"+map+"\\"+map+".wdl";
    if(!assets->fileExists(path)) {
        LOG_WARNING("DistantTerrain: ",path," missing - this map uses detailed terrain streaming at the requested distance");return;
    }
    source_=true;jobs_=std::make_unique<core::JobGroup>();
    pending_=jobs_->submit([assets,map,path](const core::JobGroup::Token& token)->std::shared_ptr<Prepared> {
        auto raw=assets->readFileShared(path);
        if(!raw) { LOG_ERROR("DistantTerrain: cannot read ",path," - distant terrain unavailable");return {}; }
        auto wdl=pipeline::readDistantMap(*raw);
        if(!wdl.valid) { LOG_ERROR("DistantTerrain: invalid ",path," - distant terrain unavailable");return {}; }
        if(wdl.tiles.empty()) { LOG_WARNING("DistantTerrain: ",path," contains no far terrain - detailed streaming retained"); return {}; }
        auto result=std::make_shared<Prepared>();
        const auto lookupData=assets->readFile("Textures\\Minimap\\md5translate.trs");
        std::unordered_map<std::string,std::string> lookup;
        std::istringstream lines(std::string(lookupData.begin(),lookupData.end()));std::string line;
        while(std::getline(lines,line)) {
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            auto tab=line.find('\t');if(tab==std::string::npos)continue;
            auto key=line.substr(0,tab),hash=line.substr(tab+1);
            if(key.ends_with(".blp"))key.resize(key.size()-4);
            if(hash.ends_with(".blp"))hash.resize(hash.size()-4);
            std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return std::tolower(c);});
            lookup.emplace(std::move(key),std::move(hash));
        }
        if(lookupData.empty())LOG_WARNING("DistantTerrain: minimap lookup missing - distant terrain uses neutral albedo");
        for(const auto& tile:wdl.tiles) {
            if(!token.current())return {};
            auto mesh=pipeline::distantTileMesh(tile);
            auto key=map+"\\map"+std::to_string(tile.x)+"_"+std::to_string(tile.y);
            std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return std::tolower(c);});
            auto found=lookup.find(key);
            pipeline::BLPImage image;
            if(found!=lookup.end()) {
                auto bytes=assets->readFile("Textures\\Minimap\\"+found->second+".blp");
                if(!bytes.empty())image=pipeline::BLPLoader::load(bytes,true);
            }
            if(image.isValid()) {
                if(image.isBlockCompressed()) {
                    size_t mip=0;
                    while(mip+1<image.mipmaps.size() && std::max(image.width,image.height)>32) {
                        ++mip;image.width=std::max(1,image.width/2);image.height=std::max(1,image.height/2);
                    }
                    auto blocks=std::move(image.mipmaps[mip]);image.mipmaps.clear();image.mipmaps.push_back(std::move(blocks));
                    image.data=pipeline::BLPLoader::decodeBaseLevel(image);
                }
                if(!image.data.empty())for(auto& vertex:mesh.vertices) {
                    int x=std::clamp(int(vertex.uv.x*image.width),0,image.width-1);
                    int y=std::clamp(int(vertex.uv.y*image.height),0,image.height-1);
                    size_t p=(y*image.width+x)*4;
                    vertex.color=glm::vec3(image.data[p],image.data[p+1],image.data[p+2])/255.f;
                }
            } else LOG_WARNING("DistantTerrain: no minimap colour for ",key," - neutral distant albedo used");
            Prepared::Tile out{tile.x,tile.y,static_cast<uint32_t>(result->vertices.size()),mesh.lods,mesh.min,mesh.max};
            for(auto& range:out.lods) range.first+=static_cast<uint32_t>(result->indices.size());
            result->vertices.insert(result->vertices.end(),mesh.vertices.begin(),mesh.vertices.end());
            result->indices.insert(result->indices.end(),mesh.indices.begin(),mesh.indices.end());
            result->tiles.push_back(std::move(out));
        }
        LOG_INFO("DistantTerrain: ",path," prepared ",result->tiles.size()," coarse tiles, ",result->vertices.size()," vertices");
        return result;
    });
}
void DistantTerrain::update() {
    if(!pending_.valid() || pending_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
    auto data=pending_.get();
    if(!data) { source_=false;return; }
    auto gpu=std::make_shared<Gpu>();gpu->ctx=ctx_;
    gpu->vertices=uploadBuffer(*ctx_,data->vertices.data(),data->vertices.size()*sizeof(pipeline::DistantVertex),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    gpu->indices=uploadBuffer(*ctx_,data->indices.data(),data->indices.size()*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if(!gpu->vertices.buffer || !gpu->indices.buffer) { LOG_ERROR("DistantTerrain: ",map_," GPU upload failed - distant terrain unavailable");source_=false;return; }
    gpu->tiles=std::move(data->tiles);gpu_=std::move(gpu);
    LOG_INFO("DistantTerrain: ",map_," GPU residency ",(data->vertices.size()*sizeof(pipeline::DistantVertex)+data->indices.size()*4)/1024," KiB");
}
DistantTerrain::Frame DistantTerrain::extract(const Camera& camera,float distance,const pipeline::TerrainCoverage& coverage) const {
    Frame result;result.data=gpu_;if(!gpu_)return result;
    Frustum frustum;frustum.extractFromMatrix(camera.getViewProjectionMatrix());
    const auto eye=camera.getPosition();
    float focal=std::abs(camera.getProjectionMatrix()[1][1])*ctx_->getSwapchainExtent().height*.5f;
    for(const auto& tile:gpu_->tiles) {
        const auto centre=(tile.min+tile.max)*.5f;float radius=glm::length(tile.max-tile.min)*.5f;
        float nearest=std::max(1.f,glm::length(centre-eye)-radius);
        if(glm::length(glm::vec2(centre-eye))>distance+radius || !frustum.intersectsSphere(centre,radius))continue;
        const auto mask=coverage[tile.y*64+tile.x];
        if(glm::length(glm::vec2(centre-eye))+radius < kDistantTerrainStart && std::all_of(mask.begin(),mask.end(),[](uint32_t m){return m==0xffffffffu;})) continue;
        int level=0;
        for(int i=1;i<4;++i) {
            if(!tile.lods[i].count || tile.lods[i].error*focal/nearest>1.f)break;
            level=i;
        }
        auto range=tile.lods[level];
        result.draws.push_back({range.first,range.count,static_cast<int32_t>(tile.vertex),mask});
    }
    return result;
}
void DistantTerrain::record(VkCommandBuffer cmd,VkDescriptorSet perFrame,const Frame& frame) {
    if(!frame.data || frame.draws.empty())return;
    if(!pipeline_)return;
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&perFrame,0,nullptr);
    VkDeviceSize offset=0;vkCmdBindVertexBuffers(cmd,0,1,&frame.data->vertices.buffer,&offset);
    vkCmdBindIndexBuffer(cmd,frame.data->indices.buffer,0,VK_INDEX_TYPE_UINT32);
    for(const auto& draw:frame.draws) {
        vkCmdPushConstants(cmd,layout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,32,draw.coverage.data());
        vkCmdDrawIndexed(cmd,draw.count,1,draw.first,draw.vertex,0);
    }
}
void DistantTerrain::shutdown() {
    if(jobs_){jobs_->cancel();jobs_->wait();jobs_.reset();}pending_={};gpu_.reset();
    if(ctx_) { DeferredResources retire(*ctx_);retire.destroy(pipeline_);retire.destroy(layout_); }
    ctx_=nullptr;
}
}
