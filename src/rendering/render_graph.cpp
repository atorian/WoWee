#include "rendering/render_graph.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <queue>
#include <stdexcept>

namespace wowee::rendering {
namespace {
bool writes(VkAccessFlags2 access) {
    constexpr VkAccessFlags2 mask = VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
        VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    return (access & mask) != 0;
}
bool contains(const std::vector<RGResource>& list, uint32_t id) {
    return std::any_of(list.begin(),list.end(),[&](auto r) { return r.id==id; });
}
}
void RenderGraph::reset() {
    passes_.clear(); executionOrder_.clear(); compiled_=false; error_.clear();
    for(auto& r:resources_) r.binding.reset();
}
RGResource RenderGraph::registerResource(const std::string& name) {
    const auto existing=findResource(name);
    if(existing.valid()) return existing;
    resources_.push_back({name}); compiled_=false;
    return {static_cast<uint32_t>(resources_.size()-1)};
}
RGResource RenderGraph::versionResource(RGResource previous, const std::string& name) {
    if(previous.id>=resources_.size()) throw std::invalid_argument("RenderGraph: unknown previous resource");
    auto result=findResource(name);
    if(result.valid()) {
        if(resources_[result.id].previous!=previous.id)
            throw std::invalid_argument("RenderGraph: resource version has a different predecessor: "+name);
        return result;
    }
    result=registerResource(name);
    resources_[result.id].previous=previous.id;
    return result;
}
RGResource RenderGraph::findResource(const std::string& name) const {
    for(uint32_t i=0;i<resources_.size();++i) if(resources_[i].name==name) return {i};
    return {};
}
uint32_t RenderGraph::root(uint32_t id) const {
    while(resources_[id].previous!=UINT32_MAX) id=resources_[id].previous;
    return id;
}
void RenderGraph::importImage(RGResource r, VkImage image, RGState initial, VkImageSubresourceRange range) {
    if(r.id>=resources_.size() || resources_[r.id].previous!=UINT32_MAX || !image)
        throw std::invalid_argument("RenderGraph: image import needs a root resource and a live image");
    Import value; value.image=image; value.initial=initial; value.range=range;
    resources_[r.id].binding=value; compiled_=false;
}
void RenderGraph::importBuffer(RGResource r, ::VkBuffer buffer, RGState initial, VkDeviceSize offset, VkDeviceSize size) {
    if(r.id>=resources_.size() || resources_[r.id].previous!=UINT32_MAX || !buffer)
        throw std::invalid_argument("RenderGraph: buffer import needs a root resource and a live buffer");
    Import value; value.buffer=buffer; value.initial=initial; value.offset=offset; value.size=size;
    resources_[r.id].binding=value; compiled_=false;
}
uint32_t RenderGraph::addPass(const std::string& name, const std::vector<RGResource>& inputs,
                            const std::vector<RGResource>& outputs,
                            std::function<void(VkCommandBuffer)> execute) {
    RGPass pass; pass.name=name; pass.inputs=inputs; pass.outputs=outputs; pass.execute=std::move(execute);
    passes_.push_back(std::move(pass)); compiled_=false;
    return static_cast<uint32_t>(passes_.size()-1);
}
void RenderGraph::use(uint32_t pass, RGResource resource, RGState state) {
    if(pass>=passes_.size()) throw std::invalid_argument("RenderGraph: unknown pass");
    passes_[pass].uses.push_back({resource,state}); compiled_=false;
}
void RenderGraph::setPassEnabled(const std::string& name, bool enabled) {
    for(auto& pass:passes_) if(pass.name==name) { pass.enabled=enabled; compiled_=false; return; }
}
bool RenderGraph::fail(std::string message) {
    error_=std::move(message); executionOrder_.clear();
    if (error_ != reportedError_) {
        LOG_ERROR("RenderGraph: ",error_,"; frame graph will not be recorded");
        reportedError_ = error_;
    }
    return false;
}
bool RenderGraph::compile() {
    error_.clear(); executionOrder_.clear();
    for(auto& pass:passes_) { pass.imageBarriers.clear(); pass.bufferBarriers.clear(); }
    compiled_=true;
    return topologicalSort() && buildBarriers();
}
bool RenderGraph::topologicalSort() {
    const uint32_t n=static_cast<uint32_t>(passes_.size());
    std::vector<uint32_t> producer(resources_.size(),UINT32_MAX);
    std::vector<std::vector<uint32_t>> readers(resources_.size());
    std::vector<std::vector<uint32_t>> edges(n);
    std::vector<uint32_t> degree(n,0);
    auto edge=[&](uint32_t from,uint32_t to) {
        if(from==UINT32_MAX || from==to) return;
        auto& list=edges[from];
        if(std::find(list.begin(),list.end(),to)==list.end()) { list.push_back(to); ++degree[to]; }
    };
    for(uint32_t i=0;i<n;++i) {
        const auto& pass=passes_[i];
        if(!pass.enabled) continue;
        for(auto out:pass.outputs) {
            if(out.id>=resources_.size()) return fail(pass.name+": unknown output");
            if(producer[out.id]!=UINT32_MAX)
                return fail(resources_[out.id].name+": multiple writers; declare the next resource version");
            producer[out.id]=i;
        }
        for(auto in:pass.inputs) {
            if(in.id>=resources_.size()) return fail(pass.name+": unknown input");
            if(contains(pass.outputs,in.id)) return fail(pass.name+": reads and writes one version; declare the next version");
            readers[in.id].push_back(i);
        }
    }
    for(uint32_t r=0;r<resources_.size();++r) {
        for(auto reader:readers[r]) edge(producer[r],reader);
        const auto previous=resources_[r].previous;
        if(previous==UINT32_MAX || producer[r]==UINT32_MAX) continue;
        edge(producer[previous],producer[r]);
        for(auto reader:readers[previous]) edge(reader,producer[r]);
        // A physical resource has one linear chain of versions, not branches.
        for(uint32_t other=0;other<r;++other)
            if(resources_[other].previous==previous && producer[other]!=UINT32_MAX)
                return fail(resources_[previous].name+": concurrent successor versions");
    }
    std::priority_queue<uint32_t,std::vector<uint32_t>,std::greater<uint32_t>> ready;
    size_t active=0;
    for(uint32_t i=0;i<n;++i) if(passes_[i].enabled) { ++active; if(!degree[i]) ready.push(i); }
    while(!ready.empty()) {
        const auto i=ready.top(); ready.pop(); executionOrder_.push_back(i);
        for(auto next:edges[i]) if(--degree[next]==0) ready.push(next);
    }
    if(executionOrder_.size()!=active) return fail("dependency cycle");
    return true;
}
bool RenderGraph::buildBarriers() {
    struct State {
        RGState writer, readers;
        std::vector<RGState> visible;
        VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
        bool initialized=false;
    };
    std::vector<State> states(resources_.size());
    std::vector<bool> produced(resources_.size(),false);
    for(uint32_t r=0;r<resources_.size();++r) if(resources_[r].binding) {
        const auto& binding=*resources_[r].binding;
        for(uint32_t other=0;other<r;++other) if(resources_[other].binding) {
            const auto& b=*resources_[other].binding;
            if((binding.image && b.image==binding.image) || (binding.buffer && b.buffer==binding.buffer))
                return fail(resources_[r].name+": physical resource imported twice; use versions");
        }
        auto& state=states[r]; state.layout=binding.initial.layout;
        state.initialized=binding.image ? state.layout!=VK_IMAGE_LAYOUT_UNDEFINED : binding.initial.stage!=0;
        if(writes(binding.initial.access)) state.writer=binding.initial;
        else state.readers=binding.initial;
    }
    for(auto id:executionOrder_) {
        auto& pass=passes_[id];
        std::vector<std::pair<uint32_t,RGState>> uses;
        for(const auto& use:pass.uses) {
            const auto r=use.resource.id;
            if(r>=resources_.size()) return fail(pass.name+": unknown physical resource");
            const auto physical=root(r);
            if(!resources_[physical].binding) return fail(pass.name+": resource was not imported for this frame: "+resources_[r].name);
            if(!contains(pass.inputs,r) && !contains(pass.outputs,r)) return fail(pass.name+": undeclared resource access: "+resources_[r].name);
            if(!use.state.stage) return fail(pass.name+": resource access has no pipeline stage");
            if(writes(use.state.access) && !contains(pass.outputs,r)) return fail(pass.name+": writes an input version");
            if(contains(pass.inputs,r) && !produced[r] &&
                (resources_[r].previous!=UINT32_MAX || !states[physical].initialized))
                return fail(pass.name+": reads an uninitialized version: "+resources_[r].name);
            auto it=std::find_if(uses.begin(),uses.end(),[&](const auto& u) { return u.first==physical; });
            if(it==uses.end()) uses.emplace_back(physical,use.state);
            else {
                if(it->second.layout!=use.state.layout) return fail(pass.name+": incompatible layouts within one pass");
                it->second.stage|=use.state.stage; it->second.access|=use.state.access;
            }
        }
        for(const auto& [r,use]:uses) {
            auto& state=states[r]; const auto& binding=*resources_[r].binding;
            const bool write=writes(use.access);
            const bool transition=binding.image && state.layout!=use.layout;
            if(binding.image && (use.layout==VK_IMAGE_LAYOUT_UNDEFINED || use.layout==VK_IMAGE_LAYOUT_PREINITIALIZED))
                return fail(pass.name+": image use has no usable layout");
            const bool visible=std::any_of(state.visible.begin(),state.visible.end(),[&](const auto& scope) {
                return (scope.stage & use.stage)==use.stage && (scope.access & use.access)==use.access;
            });
            if(write || transition || (state.writer.stage && !visible)) {
                RGState source=state.writer;
                if(write || transition) { source.stage|=state.readers.stage; source.access|=state.readers.access; }
                if(binding.image) pass.imageBarriers.push_back({binding.image,state.layout,use.layout,
                    source.access,use.access,source.stage,use.stage,binding.range});
                else if(source.stage) pass.bufferBarriers.push_back({binding.buffer,binding.offset,binding.size,
                    source.access,use.access,source.stage,use.stage});
                if(transition) { state.readers={}; state.visible.clear(); }
                state.visible.push_back(use);
            }
            state.layout=use.layout;
            if(write) { state.writer=use; state.readers={}; state.visible.clear(); state.initialized=true; }
            else { state.readers.stage|=use.stage; state.readers.access|=use.access; }
        }
        for(auto out:pass.outputs) {
            if(resources_[root(out.id)].binding && !std::any_of(pass.uses.begin(),pass.uses.end(),[&](const auto& u) {
                return u.resource.id==out.id && writes(u.state.access);
            })) return fail(pass.name+": physical output has no declared write: "+resources_[out.id].name);
            produced[out.id]=true;
        }
        for(auto in:pass.inputs) {
            if(resources_[root(in.id)].binding && !std::any_of(pass.uses.begin(),pass.uses.end(),[&](const auto& u) {
                return u.resource.id==in.id;
            })) return fail(pass.name+": physical input has no declared access: "+resources_[in.id].name);
        }
    }
    return true;
}
void RenderGraph::recordPass(VkCommandBuffer cmd, const RGPass& pass) {
        // Insert image barriers declared for this pass
        if (!pass.imageBarriers.empty()) {
            // Each barrier carries its own stages. The legacy call took one
            // pair for the whole batch, so every barrier in it waited on the
            // union of all of them; here a depth transition no longer waits on
            // whatever a colour one needed.
            std::vector<VkImageMemoryBarrier2> barriers;
            barriers.reserve(pass.imageBarriers.size());

            for (const auto& b : pass.imageBarriers) {
                VkImageMemoryBarrier2 ib{};
                ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                ib.srcStageMask = b.srcStage;
                ib.dstStageMask = b.dstStage;
                ib.oldLayout = b.oldLayout;
                ib.newLayout = b.newLayout;
                ib.srcAccessMask = b.srcAccess;
                ib.dstAccessMask = b.dstAccess;
                ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.image = b.image;
                ib.subresourceRange = b.range;
                barriers.push_back(ib);
            }

            VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dep.pImageMemoryBarriers = barriers.data();
            cmdPipelineBarrier2(cmd, dep);
        }

        // Insert buffer barriers declared for this pass
        if (!pass.bufferBarriers.empty()) {
            std::vector<VkBufferMemoryBarrier2> barriers;
            barriers.reserve(pass.bufferBarriers.size());

            for (const auto& b : pass.bufferBarriers) {
                VkBufferMemoryBarrier2 bb{};
                bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                bb.srcStageMask = b.srcStage;
                bb.dstStageMask = b.dstStage;
                bb.srcAccessMask = b.srcAccess;
                bb.dstAccessMask = b.dstAccess;
                bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.buffer = b.buffer;
                bb.offset = b.offset;
                bb.size = b.size;
                barriers.push_back(bb);
            }

            VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dep.pBufferMemoryBarriers = barriers.data();
            cmdPipelineBarrier2(cmd, dep);
        }

    pass.execute(cmd);
}
void RenderGraph::record(VkCommandBuffer cmd) {
    if(!compiled_ && !compile()) return;
    for(auto id:executionOrder_) recordPass(cmd,passes_[id]);
}
void RenderGraph::execute(VkCommandBuffer cmd, VkContext& context, std::string_view stageGroup) {
    if(!compiled_ && !compile()) return;
    for(auto id:executionOrder_) {
        const auto& pass=passes_[id];
        const uint32_t marksBefore=context.gpuMarksRecorded();
        {
            CpuStage stage(context.cpuStages(),stageGroup,pass.name);
            GpuScope scope(context,cmd,pass.name.c_str());
            recordPass(cmd,pass);
        }
        if(context.gpuMarksRecorded()==marksBefore) context.gpuMark(cmd,context.gpuLabel(pass.name));
    }
}
} // namespace wowee::rendering
