// Particle and ribbon emitters: the per-emitter GPU resources every M2 model
// needs, and the emitters of models another renderer draws and poses.
//
// Creatures, mounts and the weapons they carry are drawn by CharacterRenderer,
// which draws meshes and nothing else. Their emitters - the riding phoenix's
// wing trails, a fire elemental's flames, a weapon's enchant sparks - run here
// instead, on a skeleton CharacterRenderer poses: it hands over the model
// matrix, the bones and the animation clock each frame, and these emitters
// emit and are drawn exactly as a doodad's would be.

#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <memory>

namespace wowee {
namespace rendering {

void M2Renderer::setupEmitterResources(M2ModelGPU& gpuModel, const pipeline::M2Model& model,
                                       const std::vector<VkTexture*>& allTextures) {
    // Copy particle emitter data and resolve textures
    gpuModel.particleEmitters = model.particleEmitters;
    gpuModel.particleTextures.resize(model.particleEmitters.size(), whiteTexture_.get());
    for (size_t ei = 0; ei < model.particleEmitters.size(); ei++) {
        uint16_t texIdx = model.particleEmitters[ei].texture;
        if (texIdx < allTextures.size() && allTextures[texIdx] != nullptr) {
            gpuModel.particleTextures[ei] = allTextures[texIdx];
        } else {
            LOG_WARNING("M2 '", model.name, "' particle emitter[", ei,
                        "] texture index ", texIdx, " out of range (", allTextures.size(),
                        " textures) - using white fallback");
        }
    }

    // Pre-allocate one stable descriptor set per particle emitter to avoid per-frame allocation.
    // This prevents materialDescPool_ exhaustion when many emitters are active each frame.
    if (particleTexLayout_ && materialDescPool_ && !model.particleEmitters.empty()) {
        VkDevice device = vkCtx_->getDevice();
        gpuModel.particleTexSets.resize(model.particleEmitters.size(), VK_NULL_HANDLE);
        for (size_t ei = 0; ei < model.particleEmitters.size(); ei++) {
            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(device, &ai, &gpuModel.particleTexSets[ei]) == VK_SUCCESS) {
                // Valid, not merely non-null: descriptorInfo() returns the
                // texture's handles as they are, so one whose upload failed
                // writes a null view and sampler into a live descriptor and
                // declares SHADER_READ_ONLY_OPTIMAL over it.
                VkTexture* tex = gpuModel.particleTextures[ei];
                if (!tex || !tex->isValid()) tex = whiteTexture_.get();
                if (!tex || !tex->isValid()) continue;
                // Wrapped as the model's slot says: the image is shared, and
                // its own sampler is the cache's, not this emitter's.
                const uint16_t slot = model.particleEmitters[ei].texture;
                const uint8_t wrap = slot < model.textures.size()
                    ? static_cast<uint8_t>(model.textures[slot].flags & 0x3) : 0;
                VkDescriptorImageInfo imgInfo = batchImageInfo(*tex, wrap);
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = gpuModel.particleTexSets[ei];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }
    }

    // Copy ribbon emitter data and resolve textures
    gpuModel.ribbonEmitters = model.ribbonEmitters;
    if (!model.ribbonEmitters.empty()) {
        VkDevice device = vkCtx_->getDevice();
        gpuModel.ribbonTextures.resize(model.ribbonEmitters.size(), whiteTexture_.get());
        gpuModel.ribbonTexSets.resize(model.ribbonEmitters.size(), VK_NULL_HANDLE);
        gpuModel.ribbonBlendModes.resize(model.ribbonEmitters.size(), 2);
        for (size_t ri = 0; ri < model.ribbonEmitters.size(); ri++) {
            // Ribbon textureIndex is a direct index into the model's texture
            // array (NOT through the textureLookup table).
            uint16_t texDirect = model.ribbonEmitters[ri].textureIndex;
            if (texDirect < allTextures.size() && allTextures[texDirect] != nullptr) {
                gpuModel.ribbonTextures[ri] = allTextures[texDirect];
            } else {
                LOG_WARNING("M2 '", model.name, "' ribbon emitter[", ri,
                            "] texture ", texDirect, " of ", allTextures.size(),
                            (texDirect < allTextures.size() ? " did not load" : " is out of range"),
                            " - ribbon drawn white");
            }
            // materialIndex is an index into the model's materials, not its
            // batches: the batch at that index belongs to some other submesh.
            uint16_t matIdx = model.ribbonEmitters[ri].materialIndex;
            if (matIdx < model.materials.size()) {
                gpuModel.ribbonBlendModes[ri] = model.materials[matIdx].blendMode;
            } else {
                LOG_WARNING("M2 '", model.name, "' ribbon emitter[", ri, "] material ", matIdx,
                            " of ", model.materials.size(), " is out of range - ribbon alpha-blended");
            }
            // Allocate descriptor set (reuse particleTexLayout_ = single sampler)
            if (particleTexLayout_ && materialDescPool_) {
                VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = materialDescPool_;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &particleTexLayout_;
                if (vkAllocateDescriptorSets(device, &ai, &gpuModel.ribbonTexSets[ri]) == VK_SUCCESS) {
                    VkTexture* tex = gpuModel.ribbonTextures[ri];
                    if (!tex || !tex->isValid()) tex = whiteTexture_.get();
                    if (!tex || !tex->isValid()) continue;
                    const uint8_t wrap = texDirect < model.textures.size()
                        ? static_cast<uint8_t>(model.textures[texDirect].flags & 0x3) : 0;
                    VkDescriptorImageInfo imgInfo = batchImageInfo(*tex, wrap);
                    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet = gpuModel.ribbonTexSets[ri];
                    write.dstBinding = 0;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &imgInfo;
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                }
            }
        }
        LOG_DEBUG("  Ribbon emitters loaded: ", model.ribbonEmitters.size());
    }
}

uint32_t M2Renderer::loadPosedEmitterModel(const pipeline::M2Model& model) {
    if (model.particleEmitters.empty() && model.ribbonEmitters.empty()) return 0;
    if (!vkCtx_) return 0;

    PosedEmitterModel entry;
    M2ModelGPU& gpu = entry.gpu;
    gpu.name = model.name;
    gpu.boundMin = model.boundMin;
    gpu.boundMax = model.boundMax;
    gpu.globalSequenceDurations = model.globalSequenceDurations;

    // The textures the emitters name, and only those: the model's skins are
    // its owner's business. A slot with no file of its own is one a display
    // fills in, which an emitter cannot be given; setupEmitterResources says
    // so for each emitter that names one.
    std::vector<VkTexture*> textures(model.textures.size(), nullptr);
    const auto load = [&](uint32_t index) {
        if (index >= model.textures.size() || textures[index]) return;
        std::string path = model.textures[index].filename;
        if (const size_t nul = path.find('\0'); nul != std::string::npos) path.resize(nul);
        if (path.empty()) return;
        TextureRef texture = loadTexture(path);
        textures[index] = texture.get();
        gpu.textureRefs.push_back(std::move(texture));
    };
    std::vector<uint32_t> bones;
    for (const auto& em : model.particleEmitters) {
        load(em.texture);
        bones.push_back(em.bone);
    }
    for (const auto& rib : model.ribbonEmitters) {
        load(rib.textureIndex);
        bones.push_back(rib.bone);
    }
    setupEmitterResources(gpu, model, textures);

    std::sort(bones.begin(), bones.end());
    bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
    for (uint32_t bone : bones) {
        if (bone < model.bones.size()) {
            entry.bones.push_back(bone);
        } else {
            // emitParticles leaves such an emitter at the model origin.
            LOG_WARNING("M2 '", model.name, "' has an emitter on bone ", bone, " of ",
                        model.bones.size(), " - it emits from the model origin");
        }
    }
    entry.boneCount = entry.bones.empty() ? 0 : entry.bones.back() + 1;

    const uint32_t handle = nextPosedEmitterModel_++;
    posedEmitterModels_.emplace(handle, std::move(entry));
    return handle;
}

void M2Renderer::unloadPosedEmitterModel(uint32_t handle) {
    auto it = posedEmitterModels_.find(handle);
    if (it == posedEmitterModels_.end()) return;
    for (auto posedIt = scene_->posedEmitters_.begin(); posedIt != scene_->posedEmitters_.end();) {
        if (posedIt->second.modelHandle == handle) {
            LOG_WARNING("M2 posed emitters ", posedIt->first, " outlived their model '",
                        it->second.gpu.name, "' - removed with it");
            posedIt = scene_->posedEmitters_.erase(posedIt);
        } else {
            ++posedIt;
        }
    }

    // The descriptor sets may still be bound by frames in flight. They go
    // back to the pool once every frame slot has been fenced - unless the
    // pool has been reset or destroyed by then, which frees them already and
    // makes freeing them again an error.
    std::vector<VkDescriptorSet> sets;
    for (VkDescriptorSet set : it->second.gpu.particleTexSets) if (set) sets.push_back(set);
    for (VkDescriptorSet set : it->second.gpu.ribbonTexSets) if (set) sets.push_back(set);
    posedEmitterModels_.erase(it);
    if (sets.empty() || !vkCtx_ || !materialDescPool_) return;
    vkCtx_->deferAfterAllFrameFences(
        [device = vkCtx_->getDevice(), pool = materialDescPool_,
         generation = materialPoolGeneration_, expected = *materialPoolGeneration_,
         sets = std::move(sets)]() {
            if (*generation != expected) return;
            vkFreeDescriptorSets(device, pool, static_cast<uint32_t>(sets.size()), sets.data());
        });
}

bool M2Renderer::hasPosedEmitterModel(uint32_t handle) const {
    return posedEmitterModels_.contains(handle);
}

uint32_t M2Renderer::createPosedEmitters(uint32_t modelHandle) {
    auto modelIt = posedEmitterModels_.find(modelHandle);
    if (modelIt == posedEmitterModels_.end()) return 0;
    const uint32_t id = nextPosedEmitters_++;
    PosedEmitters& posed = scene_->posedEmitters_[id];
    posed.modelHandle = modelHandle;
    posed.inst.id = id;
    posed.inst.random = InstanceRandom(id);
    posed.inst.cachedModel = &modelIt->second.gpu;
    posed.inst.boneMatrices.assign(modelIt->second.boneCount, glm::mat4(1.0f));
    return id;
}

void M2Renderer::removePosedEmitters(uint32_t id) {
    scene_->posedEmitters_.erase(id);
}

bool M2Renderer::hasPosedEmitters(uint32_t id) const {
    return scene_->posedEmitters_.contains(id);
}

void M2Renderer::setPosedEmittersPose(uint32_t id, const EmitterPose& pose) {
    auto it = scene_->posedEmitters_.find(id);
    if (it == scene_->posedEmitters_.end()) return;
    PosedEmitters& posed = it->second;
    posed.posed = pose.posed;
    if (!pose.posed) return;

    M2Instance& inst = posed.inst;
    inst.modelMatrix = pose.modelMatrix;
    inst.position = glm::vec3(pose.modelMatrix[3]);
    inst.scale = pose.scale;
    inst.currentSequenceIndex = std::max(pose.sequenceIndex, 0);
    inst.animTime = pose.animationTimeMs;
    inst.globalSequenceTime = pose.globalSequenceTimeMs;

    // Only the bones an emitter hangs from; the rest stay at rest and are
    // never read.
    const PosedEmitterModel& model = posedEmitterModels_.at(posed.modelHandle);
    for (uint32_t bone : model.bones) {
        inst.boneMatrices[bone] = (pose.bones && bone < pose.bones->size())
            ? (*pose.bones)[bone] : glm::mat4(1.0f);
    }
}

void M2Renderer::updatePosedEmitters(float dt) {
    for (auto& [id, posed] : scene_->posedEmitters_) {
        M2Instance& inst = posed.inst;
        const M2ModelGPU& gpu = *inst.cachedModel;
        if (posed.posed && !gpu.particleEmitters.empty()) emitParticles(inst, gpu, dt);
        updateParticles(inst, dt);
        if (!gpu.ribbonEmitters.empty()) updateRibbons(inst, gpu, dt, posed.posed);
    }
}

void M2Renderer::destroyPosedEmitterModels() {
    // Called with the device idle, before the material pool is reset or
    // destroyed: the sets go back to it now.
    if (vkCtx_ && materialDescPool_) {
        VkDevice device = vkCtx_->getDevice();
        for (auto& [handle, model] : posedEmitterModels_) {
            for (VkDescriptorSet set : model.gpu.particleTexSets)
                if (set) vkFreeDescriptorSets(device, materialDescPool_, 1, &set);
            for (VkDescriptorSet set : model.gpu.ribbonTexSets)
                if (set) vkFreeDescriptorSets(device, materialDescPool_, 1, &set);
        }
    }
    scene_->posedEmitters_.clear();
    posedEmitterModels_.clear();
    ++*materialPoolGeneration_;
}

} // namespace rendering
} // namespace wowee
