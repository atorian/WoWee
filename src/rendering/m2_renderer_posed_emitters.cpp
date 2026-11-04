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
#include "rendering/texture_cache.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "core/logger.hpp"
#include "core/thread_pool.hpp"
#include "pipeline/asset_manager.hpp"
#include <algorithm>
#include <cmath>
#include <memory>

namespace wowee {
namespace rendering {

void M2Renderer::setupEmitterResources(M2ModelGPU& gpuModel, const pipeline::M2Model& model,
                                       const std::vector<VkTexture*>& allTextures) {
    gpuModel.particleEmitters = model.particleEmitters;
    for (size_t ei = 0; ei < model.particleEmitters.size(); ei++) {
        const uint16_t texIdx = model.particleEmitters[ei].texture;
        if (texIdx >= allTextures.size() || allTextures[texIdx] == nullptr) {
            LOG_WARNING("M2 '", model.name, "' particle emitter[", ei,
                        "] texture index ", texIdx, " out of range (", allTextures.size(),
                        " textures) - using white fallback");
        }
    }

    gpuModel.ribbonEmitters = model.ribbonEmitters;
    gpuModel.ribbonBlendModes.assign(model.ribbonEmitters.size(), 2);
    for (size_t ri = 0; ri < model.ribbonEmitters.size(); ri++) {
        // Ribbon textureIndex is a direct index into the model's texture
        // array (NOT through the textureLookup table).
        const uint16_t texDirect = model.ribbonEmitters[ri].textureIndex;
        if (texDirect >= allTextures.size() || allTextures[texDirect] == nullptr) {
            LOG_WARNING("M2 '", model.name, "' ribbon emitter[", ri,
                        "] texture ", texDirect, " of ", allTextures.size(),
                        (texDirect < allTextures.size() ? " did not load" : " is out of range"),
                        " - ribbon drawn white");
        }
        // materialIndex is an index into the model's materials, not its
        // batches: the batch at that index belongs to some other submesh.
        const uint16_t matIdx = model.ribbonEmitters[ri].materialIndex;
        if (matIdx < model.materials.size()) {
            gpuModel.ribbonBlendModes[ri] = model.materials[matIdx].blendMode;
        } else {
            LOG_WARNING("M2 '", model.name, "' ribbon emitter[", ri, "] material ", matIdx,
                        " of ", model.materials.size(), " is out of range - ribbon alpha-blended");
        }
    }

    // Wrapped as the model's slot says: the image is shared, and its own
    // sampler is the cache's, not this emitter's.
    std::vector<uint8_t> wrap(model.textures.size());
    for (size_t slot = 0; slot < model.textures.size(); ++slot)
        wrap[slot] = static_cast<uint8_t>(model.textures[slot].flags & 0x3);
    bindEmitterTextures(gpuModel, allTextures, wrap);
}

void M2Renderer::bindEmitterTextures(M2ModelGPU& gpu, const std::vector<VkTexture*>& slotTextures,
                                     const std::vector<uint8_t>& slotWrap) {
    const auto textureAt = [&](size_t slot) {
        return slot < slotTextures.size() && slotTextures[slot] ? slotTextures[slot] : whiteTexture_.get();
    };
    const auto wrapAt = [&](size_t slot) -> uint8_t { return slot < slotWrap.size() ? slotWrap[slot] : 0; };
    // Each emitter's texture set, shared with every emitter of any model that
    // draws the same texture with the same wrap mode: a set per emitter held
    // the same image as its neighbours' and competed with the materials for
    // materialDescPool_. The uses held before are given back after the new
    // ones are taken, so a set both name is not freed and made again.
    const std::vector<M2EmitterTexKey> before = std::exchange(gpu.emitterTexKeys, {});
    gpu.particleTextures.resize(gpu.particleEmitters.size());
    gpu.particleTexSets.resize(gpu.particleEmitters.size());
    for (size_t ei = 0; ei < gpu.particleEmitters.size(); ei++) {
        const uint16_t slot = gpu.particleEmitters[ei].texture;
        gpu.particleTextures[ei] = textureAt(slot);
        gpu.particleTexSets[ei] = acquireEmitterTexSet(gpu, gpu.particleTextures[ei], wrapAt(slot));
    }
    gpu.ribbonTextures.resize(gpu.ribbonEmitters.size());
    gpu.ribbonTexSets.resize(gpu.ribbonEmitters.size());
    for (size_t ri = 0; ri < gpu.ribbonEmitters.size(); ri++) {
        const uint16_t slot = gpu.ribbonEmitters[ri].textureIndex;
        gpu.ribbonTextures[ri] = textureAt(slot);
        gpu.ribbonTexSets[ri] = acquireEmitterTexSet(gpu, gpu.ribbonTextures[ri], wrapAt(slot));
    }
    releaseEmitterTexKeys(before);
}

namespace {

// The textures a model's emitters name, and only those, by their slot: the
// model's skins are its owner's business. A slot with no file of its own is
// one a display fills in, which an emitter cannot be given;
// setupEmitterResources says so for each emitter that names one.
std::vector<std::pair<uint32_t, std::string>> emitterTexturePaths(const pipeline::M2Model& model) {
    std::vector<std::pair<uint32_t, std::string>> paths;
    const auto want = [&](uint32_t index) {
        if (index >= model.textures.size()) return;
        for (const auto& [slot, path] : paths)
            if (slot == index) return;
        std::string path = model.textures[index].filename;
        if (const size_t nul = path.find('\0'); nul != std::string::npos) path.resize(nul);
        if (!path.empty()) paths.emplace_back(index, std::move(path));
    };
    for (const auto& em : model.particleEmitters) want(em.texture);
    for (const auto& rib : model.ribbonEmitters) want(rib.textureIndex);
    return paths;
}

} // namespace

bool M2Renderer::posedEmitterTexturesReady(const pipeline::M2Model& model) {
    // Every one asked for, so they are made side by side.
    bool ready = true;
    for (const auto& [slot, path] : emitterTexturePaths(model)) {
        bool loading = false;
        (void)loadTexture(path, &loading);
        ready = ready && !loading;
    }
    return ready;
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

    // Its textures, made on a worker and in the cache by now
    // (posedEmitterTexturesReady), and its emitters, which go to the GPU in
    // one batch submitted without waiting for it: this runs on the main
    // thread, in the frame, as a creature comes into view.
    std::vector<VkTexture*> textures(model.textures.size(), nullptr);
    vkCtx_->beginUploadBatch();
    for (const auto& [slot, path] : emitterTexturePaths(model)) {
        bool loading = false;
        TextureRef texture = loadTexture(path, &loading);
        if (loading)
            LOG_WARNING("M2 '", model.name, "' emitter texture '", path,
                        "' left the cache before its emitters were made - they draw it white");
        textures[slot] = texture.get();
        gpu.textureRefs.push_back(std::move(texture));
    }
    // m2_particles.comp and m2_ribbons.comp leave an emitter on a bone the
    // skeleton does not have at the model origin.
    for (const auto& em : model.particleEmitters) {
        if (em.bone >= model.bones.size())
            LOG_WARNING("M2 '", model.name, "' has a particle emitter on bone ", em.bone, " of ",
                        model.bones.size(), " - it emits from the model origin");
    }
    for (const auto& rib : model.ribbonEmitters) {
        if (rib.bone >= model.bones.size())
            LOG_WARNING("M2 '", model.name, "' has a ribbon emitter on bone ", rib.bone, " of ",
                        model.bones.size(), " - it trails from the model origin");
    }
    setupEmitterResources(gpu, model, textures);
    // They run on the GPU, on the owner's skeleton as posed there.
    gpu.emitters = skeletons_.placeEmitters(model, model.name.c_str());
    vkCtx_->endUploadBatch();

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
            releaseParticles(posedIt->second.inst);
            posedIt = scene_->posedEmitters_.erase(posedIt);
        } else {
            ++posedIt;
        }
    }

    // Its texture sets may still be bound by frames in flight; the last use
    // of a shared one gives it back after the fences, and its emitters' words
    // come back after them too.
    releaseEmitterTexSets(it->second.gpu);
    skeletons_.release(it->second.gpu.emitters);
    posedEmitterModels_.erase(it);
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
    return id;
}

void M2Renderer::removePosedEmitters(uint32_t id) {
    auto it = scene_->posedEmitters_.find(id);
    if (it == scene_->posedEmitters_.end()) return;
    releaseParticles(it->second.inst);
    scene_->posedEmitters_.erase(it);
}

bool M2Renderer::hasPosedEmitters(uint32_t id) const {
    return scene_->posedEmitters_.contains(id);
}

void M2Renderer::setPosedEmittersPose(uint32_t id, const EmitterPose& pose) {
    auto it = scene_->posedEmitters_.find(id);
    if (it == scene_->posedEmitters_.end()) return;
    PosedEmitters& posed = it->second;
    posed.posed = pose.posed;
    posed.skeletonOwner = pose.skeletonOwner;
    if (!pose.posed) return;

    M2Instance& inst = posed.inst;
    inst.modelMatrix = pose.modelMatrix;
    inst.position = glm::vec3(pose.modelMatrix[3]);
    inst.scale = pose.scale;
    inst.currentSequenceIndex = std::max(pose.sequenceIndex, 0);
    setAnimTime(inst, pose.animationTimeMs);
    inst.globalSequenceStartMs = animClockMs_ - pose.globalSequenceTimeMs;
}

void M2Renderer::updatePosedEmitters() {
    // Their particles and ribbons run on the GPU over the time update() gave
    // the world's (particleDt_); here they only get room the first time.
    for (auto& [id, posed] : scene_->posedEmitters_) {
        if (placeParticles(posed.inst)) posed.inst.particlesRanAt = particleSerial_;
    }
}

void M2Renderer::destroyPosedEmitterModels() {
    // Called with the device idle, before the material pool is reset or
    // destroyed: the sets go back to it now.
    for (auto& [id, posed] : scene_->posedEmitters_) releaseParticles(posed.inst);
    for (auto& [handle, model] : posedEmitterModels_) {
        releaseEmitterTexSets(model.gpu);
        skeletons_.release(model.gpu.emitters);
    }
    scene_->posedEmitters_.clear();
    posedEmitterModels_.clear();
    ++*materialPoolGeneration_;
}

VkDescriptorSet M2Renderer::acquireEmitterTexSet(M2ModelGPU& gpu, VkTexture* texture, uint8_t wrap) {
    // Valid, not merely non-null: descriptorInfo() returns the texture's
    // handles as they are, so one whose upload failed writes a null view and
    // sampler into a live descriptor and declares SHADER_READ_ONLY_OPTIMAL
    // over it.
    if (!texture || !texture->isValid()) texture = whiteTexture_.get();
    if (!texture || !texture->isValid() || !particleTexLayout_ || !materialDescPool_) return VK_NULL_HANDLE;

    const M2EmitterTexKey key{texture, wrap};
    SharedEmitterSet& shared = emitterTexSets_[key];
    if (!shared.set) {
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = materialDescPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &particleTexLayout_;
        if (vkCtx_->allocateDescriptorSets(ai, &shared.set) != VK_SUCCESS) {
            LOG_WARNING("M2 emitter texture set: materialDescPool_ is exhausted");
            emitterTexSets_.erase(key);
            return VK_NULL_HANDLE;
        }
        VkDescriptorImageInfo imgInfo = batchImageInfo(*texture, wrap);
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = shared.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
    }
    shared.users++;
    gpu.emitterTexKeys.push_back(key);
    return shared.set;
}

void M2Renderer::releaseEmitterTexSets(M2ModelGPU& gpu) {
    // The per-emitter set vectors stay as they are: a frame still being
    // recorded may read them, and the sets they name are freed only after
    // the frames in flight.
    releaseEmitterTexKeys(std::exchange(gpu.emitterTexKeys, {}));
}

void M2Renderer::releaseEmitterTexKeys(const std::vector<M2EmitterTexKey>& keys) {
    std::vector<VkDescriptorSet> unused;
    for (const M2EmitterTexKey& key : keys) {
        auto it = emitterTexSets_.find(key);
        if (it == emitterTexSets_.end()) continue;
        if (--it->second.users > 0) continue;
        // Out of the map at once, so a texture later made at this address
        // gets a set of its own; the set itself after the frames in flight.
        unused.push_back(it->second.set);
        emitterTexSets_.erase(it);
    }
    if (unused.empty() || !vkCtx_ || !materialDescPool_) return;
    vkCtx_->deferAfterAllFrameFences([ctx = vkCtx_, pool = materialDescPool_, sets = std::move(unused)]() {
        ctx->freeDescriptorSets(pool, static_cast<uint32_t>(sets.size()), sets.data());
    });
}

} // namespace rendering
} // namespace wowee
