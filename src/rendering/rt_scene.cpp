#include "rendering/rt_scene.hpp"

#include "core/logger.hpp"
#include "pipeline/heap_bytes.hpp"

#include <chrono>
#include <algorithm>
#include <cstring>

namespace wowee::rendering {

namespace {

constexpr uint32_t kBindingTriangles = 0;
constexpr uint32_t kBindingNodes = 1;
constexpr uint32_t kBindingMeshes = 2;
constexpr uint32_t kBindingInstances = 3;
constexpr uint32_t kBindingTlasNodes = 4;
constexpr uint32_t kBindingTlas = 5;

constexpr uint32_t kInitialTriangles = 1u << 18;
constexpr uint32_t kInitialNodes = 1u << 17;
// Triangles built and uploaded per frame. A binned-SAH build runs at a few
// million triangles a second on one core, so this keeps a frame's share to a
// few milliseconds while the world streams in after the switch is turned on.
constexpr uint64_t kBuildBudgetTriangles = 150000;
// CPU time the per-frame share may take converting sources, whatever its
// triangle budget (the software backend also builds a BVH here).
constexpr int kBuildCpuBudgetMs = 4;
// A scene update slower than this on the GPU is logged with what it built.
constexpr double kSlowBuildMs = 8.0;

uint32_t grownCapacity(uint32_t current, uint32_t needed) {
    uint32_t c = std::max(current, 1024u);
    while (c < needed) c += c / 2;
    return c;
}

RtAabb transformBounds(const RtAabb& b, const glm::mat4& m) {
    RtAabb out;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? b.bmax.x : b.bmin.x, (i & 2) ? b.bmax.y : b.bmin.y,
                               (i & 4) ? b.bmax.z : b.bmin.z);
        out.grow(glm::vec3(m * glm::vec4(corner, 1.0f)));
    }
    return out;
}

}  // namespace

RtScene::~RtScene() { shutdown(); }

bool RtScene::initialize(VkContext* ctx) {
    ctx_ = ctx;
    alive_ = std::make_shared<bool>(true);
    backend_ = ctx->hardwareRayQueryEnabled() ? Backend::Hardware : Backend::Software;
    if (backend_ == Backend::Hardware && !loadHardwareEntryPoints()) {
        LOG_ERROR("RtScene: enabled hardware ray queries lack required entry points; renderer initialization failed");
        return false;
    }

    const VkBufferUsageFlags poolUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    triPool_.elementSize = sizeof(RtTriangle);
    triPool_.usage = poolUsage;
    if (backend_ == Backend::Hardware) {
        triPool_.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    nodePool_.elementSize = sizeof(RtBvhNode);
    nodePool_.usage = poolUsage;

    placeholder_ = createBuffer(ctx_->getAllocator(), 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                VMA_MEMORY_USAGE_CPU_TO_GPU);
    if (!placeholder_.buffer) return false;
    std::memset(placeholder_.info.pMappedData, 0, 64);

    if (!createDescriptors()) return false;
    // Even an RT-disabled portrait has statically used shader descriptors.
    // Keep a tiny, built empty TLAS independent of streamed scene residency.
    if (backend_ == Backend::Hardware) {
        ctx_->immediateSubmit([&](VkCommandBuffer cmd) { buildTlas(cmd, emptyReceiver_); });
        if (!emptyReceiver_.tlas) return false;
    }
    LOG_INFO("RtScene: ", backend_ == Backend::Hardware ? "hardware" : "software", " backend");
    return true;
}

bool RtScene::loadHardwareEntryPoints() {
    VkDevice dev = ctx_->getDevice();
    auto load = [&](const char* name) { return vkGetDeviceProcAddr(dev, name); };
    pfnGetBufferDeviceAddress_ =
        reinterpret_cast<PFN_vkGetBufferDeviceAddress>(load("vkGetBufferDeviceAddress"));
    pfnCreateAs_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        load("vkCreateAccelerationStructureKHR"));
    pfnDestroyAs_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        load("vkDestroyAccelerationStructureKHR"));
    pfnGetAsBuildSizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        load("vkGetAccelerationStructureBuildSizesKHR"));
    pfnCmdBuildAs_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        load("vkCmdBuildAccelerationStructuresKHR"));
    pfnGetAsAddress_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        load("vkGetAccelerationStructureDeviceAddressKHR"));
    if (!pfnGetBufferDeviceAddress_ || !pfnCreateAs_ || !pfnDestroyAs_ || !pfnGetAsBuildSizes_ ||
        !pfnCmdBuildAs_ || !pfnGetAsAddress_) {
        return false;
    }

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(ctx_->getPhysicalDevice(), &props);
    scratchAlignment_ = std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment, 1);
    return true;
}

bool RtScene::createDescriptors() {
    VkDevice dev = ctx_->getDevice();
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto addBinding = [&](uint32_t binding, VkDescriptorType type) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = binding;
        b.descriptorType = type;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings.push_back(b);
    };
    addBinding(kBindingTriangles, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingMeshes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingInstances, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    if (backend_ == Backend::Software) {
        addBinding(kBindingNodes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        addBinding(kBindingTlasNodes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    } else {
        addBinding(kBindingTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
    }

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &setLayout_) != VK_SUCCESS) return false;

    std::vector<VkDescriptorPoolSize> sizes;
    sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     (backend_ == Backend::Software ? 5u : 3u) * MAX_FRAMES_IN_FLIGHT});
    if (backend_ == Backend::Hardware) {
        sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_FRAMES_IN_FLIGHT});
    }
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &descriptorPool_) != VK_SUCCESS) return false;

    for (auto& fr : frames_) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptorPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        if (vkAllocateDescriptorSets(dev, &ai, &fr.set) != VK_SUCCESS) return false;
    }
    return true;
}

void RtScene::shutdown() {
    if (!ctx_) return;
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    ctx_->waitIdle();
    alive_.reset();

    for (auto& m : meshes_) destroyMeshGpu(m);
    meshes_.clear();
    changes_.clear();
    sourceMeshes_.clear();
    sourceInstances_.clear();
    publishedMeshes_.clear();
    publishedInstances_.clear();
    wantedInstances_.clear();
    sourceIndex_.clear();
    dirtyInstances_.clear(); instanceDirty_.clear();
    publishedIds_.clear(); publishedSlots_.clear();
    meshReferences_.clear(); touchedMeshes_.clear(); meshTouched_.clear();
    nextInstanceGeneration_ = 1;
    extractionStats_ = {};
    sourceBytes_.store(0);
    regionDirty_ = true;
    freeMeshes_.clear();
    freeInstances_.clear();
    nextChangeSerial_ = 1;
    appliedChangeSerial_.store(0);
    instances_.clear();
    for (auto& fr : frames_) {
        destroyBuffer(alloc, fr.instances);
        destroyBuffer(alloc, fr.tlasNodes);
        destroyBuffer(alloc, fr.asInstances);
        if (fr.tlas) pfnDestroyAs_(dev, fr.tlas, nullptr);
        destroyBuffer(alloc, fr.tlasBuffer);
        destroyBuffer(alloc, fr.scratch);
        fr = FrameResources{};
    }
    destroyBuffer(alloc, emptyReceiver_.asInstances);
    if (emptyReceiver_.tlas) pfnDestroyAs_(dev, emptyReceiver_.tlas, nullptr);
    destroyBuffer(alloc, emptyReceiver_.tlasBuffer);
    destroyBuffer(alloc, emptyReceiver_.scratch);
    emptyReceiver_ = {};
    destroyBuffer(alloc, triPool_.buffer);
    destroyBuffer(alloc, nodePool_.buffer);
    destroyBuffer(alloc, meshTable_);
    destroyBuffer(alloc, placeholder_);
    if (descriptorPool_) vkDestroyDescriptorPool(dev, descriptorPool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

void RtScene::destroyMeshGpu(Mesh& m) {
    DeferredResources retired(*ctx_);
    if (pfnDestroyAs_) retired.release(m.blas, pfnDestroyAs_);
    retired.buffer(m.blasBuffer);
}

// ---------------------------------------------------------------------------
// Registration

void RtScene::post(Change change) {
    change.serial = nextChangeSerial_++;
    changes_.push_back(std::move(change));
}

void RtScene::sourceChanged(InstanceId id) {
    if (instanceDirty_.size() <= id) instanceDirty_.resize(size_t(id) + 1);
    if (!instanceDirty_[id]) {
        instanceDirty_[id] = 1;
        dirtyInstances_.push_back(id);
    }
    regionDirty_ = true;
}

RtScene::Changes RtScene::extractChanges(const RtRegion& region) {
    const uint64_t applied = appliedChangeSerial_.load(std::memory_order_acquire);
    std::erase_if(changes_, [applied](const Change& change) { return change.serial <= applied; });
    extractionStats_ = {0, static_cast<uint32_t>(publishedIds_.size()),
                        static_cast<uint32_t>(sourceIndex_.cells())};
    if (!regionDirty_ && region == publishedRegion_) return changes_;
    const bool regionChanged = region != publishedRegion_;
    publishedMeshes_.resize(sourceMeshes_.size());
    publishedInstances_.resize(sourceInstances_.size());
    publishedSlots_.resize(sourceInstances_.size(), kInvalid);
    wantedInstances_.resize(sourceInstances_.size());
    meshReferences_.resize(sourceMeshes_.size());
    meshTouched_.resize(sourceMeshes_.size());

    if (regionChanged) {
        // Existing members must be considered for removal even if no longer
        // inside any queried cell. Hysteresis is applied per instance below.
        for (InstanceId id : publishedIds_) sourceChanged(id);
        if (region.enabled) sourceIndex_.query(
            [&](const RtAabb& bounds) { return region.keeps(bounds, true); },
            [&](InstanceId id) { sourceChanged(id); });
    }
    auto touchMesh = [&](MeshId mesh) {
        if (!meshTouched_[mesh]) { meshTouched_[mesh] = 1; touchedMeshes_.push_back(mesh); }
    };
    // One source read per affected placement. Remove old identities before
    // replacing meshes; even remove/recreate at an identical transform is new.
    for (InstanceId id : dirtyInstances_) {
        ++extractionStats_.testedInstances;
        const auto& source = sourceInstances_[id];
        auto& old = publishedInstances_[id];
        const bool wanted = source.live && sourceMeshes_[source.mesh].source &&
                            region.keeps(source.bounds, old.live);
        wantedInstances_[id] = wanted;
        if (old.live && (!wanted || old.generation != source.generation ||
                old.mesh != source.mesh || publishedMeshes_[old.mesh] != sourceMeshes_[old.mesh].source)) {
            post({.kind = Change::Kind::RemoveInstance, .id = id});
            --meshReferences_[old.mesh];
            touchMesh(old.mesh);
            const uint32_t moved = publishedIds_.back();
            publishedIds_[publishedSlots_[id]] = moved;
            publishedSlots_[moved] = publishedSlots_[id];
            publishedIds_.pop_back();
            publishedSlots_[id] = kInvalid;
            old = {};
        }
        if (wanted && !old.live) {
            ++meshReferences_[source.mesh];
            touchMesh(source.mesh);
        }
    }
    for (MeshId id : touchedMeshes_) {
        auto& old = publishedMeshes_[id];
        const auto& source = sourceMeshes_[id].source;
        if (old && (meshReferences_[id] == 0 || old != source)) {
            post({.kind = Change::Kind::RemoveMesh, .id = id});
            old.reset();
        }
        if (meshReferences_[id] != 0 && !old) {
            post({.kind = Change::Kind::AddMesh, .id = id, .source = source});
            old = source;
        }
        meshTouched_[id] = 0;
    }
    touchedMeshes_.clear();
    for (InstanceId id : dirtyInstances_) {
        instanceDirty_[id] = 0;
        if (!wantedInstances_[id]) continue;
        const auto& source = sourceInstances_[id];
        auto& old = publishedInstances_[id];
        if (!old.live) {
            post({.kind = Change::Kind::AddInstance, .id = id,
                  .mesh = source.mesh, .transform = source.objectToWorld});
            publishedSlots_[id] = static_cast<uint32_t>(publishedIds_.size());
            publishedIds_.push_back(id);
        } else if (old.objectToWorld != source.objectToWorld) {
            post({.kind = Change::Kind::MoveInstance, .id = id, .transform = source.objectToWorld});
        }
        old = source;
    }
    dirtyInstances_.clear();
    regionDirty_ = false;
    publishedRegion_ = region;
    extractionStats_.publishedInstances = static_cast<uint32_t>(publishedIds_.size());
    return changes_;
}

void RtScene::applyChanges(std::span<const Change> changes) {
    uint64_t applied = appliedChangeSerial_.load(std::memory_order_relaxed);
    for (const auto& c : changes) {
        if (c.serial <= applied) continue;
        switch (c.kind) {
        case Change::Kind::AddMesh: applyAddMesh(c.id, c.source); break;
        case Change::Kind::RemoveMesh: applyRemoveMesh(c.id); break;
        case Change::Kind::AddInstance: applyAddInstance(c.id, c.mesh, c.transform); break;
        case Change::Kind::MoveInstance: applyMoveInstance(c.id, c.transform); break;
        case Change::Kind::RemoveInstance: applyRemoveInstance(c.id); break;
        }
        applied = c.serial;
    }
    appliedChangeSerial_.store(applied, std::memory_order_release);
}

RtScene::MeshId RtScene::addMesh(MeshSource source) {
    if (source.indices.size() < 3 || source.surfaces.empty()) return kInvalid;
    const size_t triangles = source.indices.size() / 3;
    if (source.surfaces.size() != 1 && source.surfaces.size() != triangles) return kInvalid;
    RtAabb bounds;
    for (uint32_t index : source.indices) {
        if (index >= source.positions.size()) return kInvalid;
        bounds.grow(source.positions[index]);
    }
    MeshId id;
    if (freeMeshes_.empty()) { id = static_cast<MeshId>(sourceMeshes_.size()); sourceMeshes_.emplace_back(); }
    else { id = freeMeshes_.back(); freeMeshes_.pop_back(); }
    sourceBytes_.fetch_add(pipeline::vectorBytes(source.positions) + pipeline::vectorBytes(source.indices) +
                          pipeline::vectorBytes(source.surfaces), std::memory_order_relaxed);
    sourceMeshes_[id] = {std::make_shared<const MeshSource>(std::move(source)), bounds};
    regionDirty_ = true;
    return id;
}

void RtScene::removeMesh(MeshId id) {
    if (id >= sourceMeshes_.size() || !sourceMeshes_[id].source) return;
    const auto& source = *sourceMeshes_[id].source;
    sourceBytes_.fetch_sub(pipeline::vectorBytes(source.positions) + pipeline::vectorBytes(source.indices) +
                          pipeline::vectorBytes(source.surfaces), std::memory_order_relaxed);
    sourceMeshes_[id] = {};
    freeMeshes_.push_back(id);
    regionDirty_ = true;
}

RtScene::InstanceId RtScene::addInstance(MeshId mesh, const glm::mat4& transform) {
    if (mesh >= sourceMeshes_.size() || !sourceMeshes_[mesh].source) return kInvalid;
    InstanceId id;
    if (freeInstances_.empty()) { id = static_cast<InstanceId>(sourceInstances_.size()); sourceInstances_.emplace_back(); }
    else { id = freeInstances_.back(); freeInstances_.pop_back(); }
    sourceInstances_[id] = {true, mesh, transform, transformBounds(sourceMeshes_[mesh].bounds, transform),
                            nextInstanceGeneration_++};
    sourceIndex_.update(id, sourceInstances_[id].bounds);
    sourceChanged(id);
    return id;
}

void RtScene::setInstanceTransform(InstanceId id, const glm::mat4& transform) {
    if (id >= sourceInstances_.size() || !sourceInstances_[id].live ||
        sourceInstances_[id].objectToWorld == transform) return;
    auto& source = sourceInstances_[id];
    source.objectToWorld = transform;
    source.bounds = transformBounds(sourceMeshes_[source.mesh].bounds, transform);
    sourceIndex_.update(id, source.bounds);
    sourceChanged(id);
}

void RtScene::removeInstance(InstanceId id) {
    if (id >= sourceInstances_.size() || !sourceInstances_[id].live) return;
    sourceIndex_.remove(id);
    sourceInstances_[id] = {};
    freeInstances_.push_back(id);
    sourceChanged(id);
}

void RtScene::applyAddMesh(MeshId id, std::shared_ptr<const MeshSource> source) {
    if (meshes_.size() <= id) meshes_.resize(static_cast<size_t>(id) + 1);
    Mesh& m = meshes_[id];
    m = Mesh{};
    m.live = true;
    m.triCount = static_cast<uint32_t>(source->indices.size() / 3);
    for (uint32_t index : source->indices) m.bounds.grow(source->positions[index]);
    m.source = std::move(source);
    ++liveMeshes_;
    liveTriangles_ += m.triCount;
    if (active_) { m.queued = true; pendingMeshes_.push_back(id); }
}

void RtScene::buildMesh(Mesh& m) {
    const MeshSource& src = *m.source;
    m.tris.resize(m.triCount);
    m.hasTranslucent = false;
    for (uint32_t t = 0; t < m.triCount; ++t) {
        const float surface = src.surfaces.size() == 1 ? src.surfaces[0] : src.surfaces[t];
        RtTriangle& tri = m.tris[t];
        tri.v0 = glm::vec4(src.positions[src.indices[t * 3]], surface);
        tri.v1 = glm::vec4(src.positions[src.indices[t * 3 + 1]], 0.0f);
        tri.v2 = glm::vec4(src.positions[src.indices[t * 3 + 2]], 0.0f);
        if (unpackRtSurface(surface).a < 1.0f) m.hasTranslucent = true;
    }
    // Only the software tracer walks these nodes; the hardware backend builds
    // its BLAS on the GPU. Building them anyway cost the main thread a
    // binned-SAH pass over every triangle that streamed in.
    if (backend_ != Backend::Software) {
        m.nodeCount = 0;
        return;
    }
    RtBvh bvh = buildRtTriangleBvh(m.tris);
    m.nodeCount = static_cast<uint32_t>(bvh.nodes.size());
    m.nodes = std::move(bvh.nodes);
}

void RtScene::setActive(bool active) {
    if (active == active_ || !ctx_) return;
    active_ = active;
    if (!active) {
        releaseGpu();
        return;
    }
    for (MeshId id = 0; id < meshes_.size(); ++id) {
        Mesh& m = meshes_[id];
        if (m.live && !m.queued && !m.uploaded) {
            m.queued = true;
            pendingMeshes_.push_back(id);
        }
    }
    ++sceneVersion_;
}

void RtScene::releaseGpu() {
    DeferredResources retired(*ctx_);
    ++gpuEpoch_;
    for (auto& m : meshes_) {
        destroyMeshGpu(m);
        m.uploaded = false;
        m.queued = false;
        m.triOffset = m.nodeOffset = RtRangeAllocator::kFailed;
        m.tris.clear();
        m.tris.shrink_to_fit();
        m.nodes.clear();
        m.nodes.shrink_to_fit();
    }
    pendingMeshes_.clear();
    for (auto& fr : frames_) {
        retired.buffer(fr.instances);
        retired.buffer(fr.tlasNodes);
        retired.buffer(fr.asInstances);
        if (pfnDestroyAs_) retired.release(fr.tlas, pfnDestroyAs_);
        retired.buffer(fr.tlasBuffer);
        retired.buffer(fr.scratch);
        const VkDescriptorSet set = fr.set;
        fr = FrameResources{};
        fr.set = set;
    }
    for (GpuPool* p : {&triPool_, &nodePool_}) {
        retired.buffer(p->buffer);
        p->alloc = RtRangeAllocator{};
    }
    retired.buffer(meshTable_);
    meshTableCapacity_ = 0;
    meshTableDirty_ = true;
    builtVersion_ = ~0ull;
    ++sceneVersion_;
    ++descriptorGeneration_;
}

void RtScene::applyRemoveMesh(MeshId id) {
    if (id >= meshes_.size() || !meshes_[id].live) return;
    Mesh& m = meshes_[id];
    m.live = false;
    --liveMeshes_;
    liveTriangles_ -= m.triCount;
    pendingMeshes_.erase(std::remove(pendingMeshes_.begin(), pendingMeshes_.end(), id),
                         pendingMeshes_.end());

    // In-flight frames may still trace this mesh. Its pool ranges, BLAS and
    // slot are released only once every frame slot has been fenced.
    const uint32_t triOffset = m.triOffset, triCount = m.triCount;
    const uint32_t nodeOffset = m.nodeOffset, nodeCount = m.nodeCount;
    VkAccelerationStructureKHR blas = m.blas;
    AllocatedBuffer blasBuffer = m.blasBuffer;
    m = Mesh{};
    // The BLAS is destroyed whether or not the scene is still alive by then;
    // the pool ranges and the slot only matter to a scene that is.
    std::weak_ptr<bool> alive = alive_;
    const uint64_t epoch = gpuEpoch_;
    const uint64_t triEpoch = triPool_.epoch, nodeEpoch = nodePool_.epoch;
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    auto destroyAs = pfnDestroyAs_;
    ctx_->deferAfterAllFrameFences([this, alive, epoch, triEpoch, nodeEpoch, dev, alloc, destroyAs, triOffset,
                                    triCount, nodeOffset, nodeCount, blas,
                                    blasBuffer]() mutable {
        if (blas) destroyAs(dev, blas, nullptr);
        destroyBuffer(alloc, blasBuffer);
        if (alive.expired()) return;
        if (epoch == gpuEpoch_ && triEpoch == triPool_.epoch && triOffset != RtRangeAllocator::kFailed) triPool_.alloc.release(triOffset, triCount);
        if (epoch == gpuEpoch_ && nodeEpoch == nodePool_.epoch && nodeOffset != RtRangeAllocator::kFailed)
            nodePool_.alloc.release(nodeOffset, nodeCount);
    });
}

void RtScene::applyAddInstance(InstanceId id, MeshId mesh, const glm::mat4& transform) {
    if (instances_.size() <= id) instances_.resize(static_cast<size_t>(id) + 1);
    instances_[id] = Instance{true, mesh, transform, transformBounds(meshes_[mesh].bounds, transform)};
    ++liveInstances_;
    ++sceneVersion_;
}

void RtScene::applyMoveInstance(InstanceId id, const glm::mat4& transform) {
    if (id >= instances_.size() || !instances_[id].live) return;
    instances_[id].objectToWorld = transform;
    instances_[id].bounds = transformBounds(meshes_[instances_[id].mesh].bounds, transform);
    ++sceneVersion_;
}

void RtScene::applyRemoveInstance(InstanceId id) {
    if (id >= instances_.size() || !instances_[id].live) return;
    instances_[id] = Instance{};
    --liveInstances_;
    ++sceneVersion_;
}

// ---------------------------------------------------------------------------
// GPU upkeep

VkDeviceAddress RtScene::bufferAddress(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return pfnGetBufferDeviceAddress_(ctx_->getDevice(), &info);
}

void RtScene::stage(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize dstOffset, const void* data,
                    VkDeviceSize size) {
    if (size == 0) return;
    AllocatedBuffer staging = createBuffer(ctx_->getAllocator(), size,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           VMA_MEMORY_USAGE_CPU_ONLY);
    if (!staging.buffer) return;
    std::memcpy(staging.info.pMappedData, data, size);
    VkBufferCopy region{0, dstOffset, size};
    vkCmdCopyBuffer(cmd, staging.buffer, dst, 1, &region);
    VmaAllocator alloc = ctx_->getAllocator();
    ctx_->deferAfterAllFrameFences([alloc, staging]() mutable { destroyBuffer(alloc, staging); });
}

void RtScene::compactPools(VkCommandBuffer cmd) {
    for (bool nodes : {false, true}) {
        GpuPool& pool = nodes ? nodePool_ : triPool_;
        if (!pool.buffer.buffer || pool.compactionFailed) continue;
        uint32_t live = 0;
        for (const auto& m : meshes_) if (m.live && m.uploaded) live += nodes ? m.nodeCount : m.triCount;
        const uint32_t floor = nodes ? kInitialNodes : kInitialTriangles;
        const uint32_t capacity = live ? std::max(floor, grownCapacity(0, live + live / 2)) : 0;
        if (capacity && uint64_t(capacity) * 2 >= pool.alloc.capacity()) continue;
        AllocatedBuffer replacement{};
        if (capacity) {
            replacement = createBuffer(ctx_->getAllocator(), VkDeviceSize(capacity) * pool.elementSize,
                                        pool.usage, VMA_MEMORY_USAGE_GPU_ONLY);
            if (!replacement.buffer) {
                pool.compactionFailed = true;
                LOG_ERROR("RT ", nodes ? "node" : "triangle", " pool compaction failed; keeping its previous buffer");
                continue;
            }
        }
        RtRangeAllocator compacted(capacity);
        std::vector<VkBufferCopy> copies;
        for (auto& m : meshes_) {
            if (!m.live || !m.uploaded) continue;
            const uint32_t count = nodes ? m.nodeCount : m.triCount;
            if (!count) continue;
            uint32_t& offset = nodes ? m.nodeOffset : m.triOffset;
            const uint32_t next = compacted.allocate(count);
            copies.push_back({VkDeviceSize(offset) * pool.elementSize,
                              VkDeviceSize(next) * pool.elementSize, VkDeviceSize(count) * pool.elementSize});
            offset = next;
        }
        if (!copies.empty()) {
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &barrier;
            cmdPipelineBarrier2(cmd, dep);
            vkCmdCopyBuffer(cmd, pool.buffer.buffer, replacement.buffer,
                            static_cast<uint32_t>(copies.size()), copies.data());
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            cmdPipelineBarrier2(cmd, dep);
        }
        auto old = pool.buffer;
        pool.buffer = replacement;
        pool.alloc = std::move(compacted);
        ++pool.epoch;
        ctx_->deferAfterAllFrameFences([alloc = ctx_->getAllocator(), old]() mutable { destroyBuffer(alloc, old); });
        meshTableDirty_ = true;
        ++descriptorGeneration_;
        ++sceneVersion_;
    }
}

void RtScene::ensurePoolCapacity(VkCommandBuffer cmd, GpuPool& pool, uint32_t needed) {
    if (pool.buffer.buffer && pool.alloc.capacity() >= needed) return;
    const uint32_t newCap = grownCapacity(pool.alloc.capacity(), needed);
    AllocatedBuffer grown = createBuffer(ctx_->getAllocator(),
                                         VkDeviceSize(newCap) * pool.elementSize, pool.usage,
                                         VMA_MEMORY_USAGE_GPU_ONLY);
    if (!grown.buffer) return;
    if (pool.buffer.buffer && pool.alloc.capacity() > 0) {
        VkBufferCopy region{0, 0, VkDeviceSize(pool.alloc.capacity()) * pool.elementSize};
        vkCmdCopyBuffer(cmd, pool.buffer.buffer, grown.buffer, 1, &region);
        VmaAllocator alloc = ctx_->getAllocator();
        AllocatedBuffer old = pool.buffer;
        ctx_->deferAfterAllFrameFences([alloc, old]() mutable { destroyBuffer(alloc, old); });
    }
    pool.buffer = grown;
    pool.alloc.grow(newCap);
    pool.compactionFailed = false;
    ++descriptorGeneration_;
}

uint32_t RtScene::poolAllocate(VkCommandBuffer cmd, GpuPool& pool, uint32_t count) {
    uint32_t off = pool.alloc.allocate(count);
    if (off != RtRangeAllocator::kFailed) return off;
    ensurePoolCapacity(cmd, pool, pool.alloc.capacity() + count);
    return pool.alloc.allocate(count);
}

void RtScene::uploadPending(VkCommandBuffer cmd) {
    if (pendingMeshes_.empty()) return;
    if (!triPool_.buffer.buffer) ensurePoolCapacity(cmd, triPool_, kInitialTriangles);
    if (backend_ == Backend::Software && !nodePool_.buffer.buffer) {
        ensurePoolCapacity(cmd, nodePool_, kInitialNodes);
    }

    // Build this frame's share from the sources: as many triangles as the
    // measured cost lets the frame afford (RtBuildBudget), the fixed share
    // where the GPU cannot time itself, and never more CPU time than a few
    // milliseconds. The first mesh is always taken, however large, so the
    // queue moves even on a frame with no room.
    const uint64_t budget = ctx_->gpuTimingSupported() ? buildBudget_.budget() : kBuildBudgetTriangles;
    const auto cpuStart = std::chrono::steady_clock::now();
    std::vector<MeshId> batch;
    uint64_t built = 0;
    size_t consumed = 0;
    for (; consumed < pendingMeshes_.size() && built < budget; ++consumed) {
        if (!batch.empty() &&
            std::chrono::steady_clock::now() - cpuStart > std::chrono::milliseconds(kBuildCpuBudgetMs)) {
            break;
        }
        const MeshId id = pendingMeshes_[consumed];
        Mesh& m = meshes_[id];
        if (!m.live || !m.queued) continue;
        m.queued = false;
        buildMesh(m);
        built += m.triCount;
        batch.push_back(id);
        FrameBuild& fb = frameBuilds_[ctx_->getCurrentFrame()];
        fb.triangles += m.triCount;
        ++fb.meshes;
        fb.largest = std::max(fb.largest, m.triCount);
    }
    pendingMeshes_.erase(pendingMeshes_.begin(), pendingMeshes_.begin() + consumed);
    if (batch.empty()) return;

    // Allocate everything first: a pool growth copies the old contents, and
    // that copy has to precede the uploads into it.
    std::vector<MeshId> ready;
    for (MeshId id : batch) {
        Mesh& m = meshes_[id];
        m.triOffset = poolAllocate(cmd, triPool_, m.triCount);
        if (backend_ == Backend::Software) m.nodeOffset = poolAllocate(cmd, nodePool_, m.nodeCount);
        if (m.triOffset == RtRangeAllocator::kFailed ||
            (backend_ == Backend::Software && m.nodeOffset == RtRangeAllocator::kFailed)) {
            LOG_WARNING("RtScene: out of memory for a mesh of ", m.triCount, " triangles");
            if (m.triOffset != RtRangeAllocator::kFailed) triPool_.alloc.release(m.triOffset, m.triCount);
            if (m.nodeOffset != RtRangeAllocator::kFailed) nodePool_.alloc.release(m.nodeOffset, m.nodeCount);
            m.triOffset = m.nodeOffset = RtRangeAllocator::kFailed;
            m.tris.clear();
            m.nodes.clear();
            continue;
        }
        ready.push_back(id);
    }

    VkMemoryBarrier2 copyOrder{};
    copyOrder.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    copyOrder.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    copyOrder.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    copyOrder.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    copyOrder.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &copyOrder;
    cmdPipelineBarrier2(cmd, dep);

    for (MeshId id : ready) {
        Mesh& m = meshes_[id];
        stage(cmd, triPool_.buffer.buffer, VkDeviceSize(m.triOffset) * sizeof(RtTriangle),
              m.tris.data(), m.tris.size() * sizeof(RtTriangle));
        if (backend_ == Backend::Software) {
            // Child indices are stored relative to the mesh; the shader adds
            // the mesh's node offset, so nothing here needs rewriting.
            stage(cmd, nodePool_.buffer.buffer, VkDeviceSize(m.nodeOffset) * sizeof(RtBvhNode),
                  m.nodes.data(), m.nodes.size() * sizeof(RtBvhNode));
        }
    }

    VkMemoryBarrier2 toReaders{};
    toReaders.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    toReaders.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toReaders.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toReaders.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toReaders.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    if (backend_ == Backend::Hardware) {
        toReaders.dstStageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        toReaders.dstAccessMask |= VK_ACCESS_2_SHADER_READ_BIT;
    }
    dep.pMemoryBarriers = &toReaders;
    cmdPipelineBarrier2(cmd, dep);

    if (backend_ == Backend::Hardware) buildPendingBlas(cmd, ready);

    for (MeshId id : ready) {
        Mesh& m = meshes_[id];
        m.uploaded = true;
        m.tris.clear();
        m.tris.shrink_to_fit();
        m.nodes.clear();
        m.nodes.shrink_to_fit();
    }
    meshTableDirty_ = true;
    ++sceneVersion_;
}

void RtScene::buildPendingBlas(VkCommandBuffer cmd, const std::vector<MeshId>& ids) {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    const VkDeviceAddress poolAddress = bufferAddress(triPool_.buffer.buffer);

    for (MeshId id : ids) {
        Mesh& m = meshes_[id];

        VkAccelerationStructureGeometryKHR geom{};
        geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // Translucent meshes go through the candidate loop in the shader, which
        // reads the triangle's opacity from the pool.
        geom.flags = m.hasTranslucent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = geom.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = poolAddress + VkDeviceAddress(m.triOffset) * sizeof(RtTriangle);
        tri.vertexStride = sizeof(glm::vec4);
        tri.maxVertex = m.triCount * 3 - 1;
        tri.indexType = VK_INDEX_TYPE_NONE_KHR;

        VkAccelerationStructureBuildGeometryInfoKHR build{};
        build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geom;

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        pfnGetAsBuildSizes_(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                            &m.triCount, &sizes);

        m.blasBuffer = createBuffer(alloc, sizes.accelerationStructureSize,
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    VMA_MEMORY_USAGE_GPU_ONLY);
        AllocatedBuffer scratch = createBuffer(alloc, sizes.buildScratchSize + scratchAlignment_,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                               VMA_MEMORY_USAGE_GPU_ONLY);
        if (!m.blasBuffer.buffer || !scratch.buffer) {
            destroyBuffer(alloc, scratch);
            continue;
        }

        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = m.blasBuffer.buffer;
        ci.size = sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (pfnCreateAs_(dev, &ci, nullptr, &m.blas) != VK_SUCCESS) {
            destroyBuffer(alloc, scratch);
            continue;
        }

        const VkDeviceAddress scratchAddr = bufferAddress(scratch.buffer);
        build.dstAccelerationStructure = m.blas;
        build.scratchData.deviceAddress =
            (scratchAddr + scratchAlignment_ - 1) & ~(scratchAlignment_ - 1);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = m.triCount;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        pfnCmdBuildAs_(cmd, 1, &build, &ranges);
        ctx_->deferAfterAllFrameFences([alloc, scratch]() mutable { destroyBuffer(alloc, scratch); });

        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = m.blas;
        m.blasAddress = pfnGetAsAddress_(dev, &ai);
    }

    // BLAS builds must finish before the TLAS build that references them.
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    cmdPipelineBarrier2(cmd, dep);
}

void RtScene::ensureMapped(AllocatedBuffer& buf, uint32_t& capacity, uint32_t needed,
                           uint32_t elementSize, VkBufferUsageFlags usage) {
    if (buf.buffer && capacity >= needed && (capacity <= 4096 || needed >= capacity / 3)) return;
    VmaAllocator alloc = ctx_->getAllocator();
    if (buf.buffer) {
        // This frame slot's previous use has been fenced, but the descriptor
        // set of this slot still names the buffer until it is rewritten below.
        AllocatedBuffer old = buf;
        ctx_->deferAfterAllFrameFences([alloc, old]() mutable { destroyBuffer(alloc, old); });
    }
    capacity = grownCapacity(capacity > needed ? 0 : capacity, std::max(needed, 1u));
    buf = createBuffer(alloc, VkDeviceSize(capacity) * elementSize, usage,
                       VMA_MEMORY_USAGE_CPU_TO_GPU);
    ++descriptorGeneration_;
}

void RtScene::uploadFrame(VkCommandBuffer cmd, FrameResources& fr) {
    // Rebuild the CPU-side per-frame data once per scene version.
    if (builtVersion_ != sceneVersion_) {
        builtVersion_ = sceneVersion_;
        instanceData_.clear();
        asInstanceData_.clear();
        std::vector<RtAabb> bounds;
        for (const Instance& inst : instances_) {
            if (!inst.live) continue;
            const Mesh& m = meshes_[inst.mesh];
            if (!m.uploaded) continue;  // failed allocation, or not yet uploaded
            if (backend_ == Backend::Hardware && !m.blas) continue;
            InstanceGPU g{};
            const glm::mat4 inv = glm::inverse(inst.objectToWorld);
            const glm::mat4 invT = glm::transpose(inv);
            g.worldToObject[0] = invT[0];
            g.worldToObject[1] = invT[1];
            g.worldToObject[2] = invT[2];
            g.mesh = inst.mesh;
            instanceData_.push_back(g);
            bounds.push_back(inst.bounds);

            if (backend_ == Backend::Hardware) {
                VkAccelerationStructureInstanceKHR ai{};
                const glm::mat4 t = glm::transpose(inst.objectToWorld);
                std::memcpy(&ai.transform, &t, sizeof(ai.transform));
                ai.instanceCustomIndex = static_cast<uint32_t>(instanceData_.size() - 1);
                ai.mask = 0xFF;
                ai.accelerationStructureReference = m.blasAddress;
                asInstanceData_.push_back(ai);
            }
        }
        tlasNodes_.clear();
        if (backend_ == Backend::Software && !instanceData_.empty()) {
            RtBvh top = buildRtBvh(bounds, 1);
            // Leaves index the reordered instance array; reorder to match.
            std::vector<InstanceGPU> sorted(instanceData_.size());
            for (size_t i = 0; i < top.order.size(); ++i) sorted[i] = instanceData_[top.order[i]];
            instanceData_.swap(sorted);
            tlasNodes_ = std::move(top.nodes);
        }
    }

    if (fr.uploadedVersion == sceneVersion_) return;
    fr.uploadedVersion = sceneVersion_;
    fr.instanceCount = static_cast<uint32_t>(instanceData_.size());

    ensureMapped(fr.instances, fr.instanceCapacity, fr.instanceCount, sizeof(InstanceGPU),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!instanceData_.empty()) {
        std::memcpy(fr.instances.info.pMappedData, instanceData_.data(),
                    instanceData_.size() * sizeof(InstanceGPU));
    }
    if (backend_ == Backend::Software) {
        ensureMapped(fr.tlasNodes, fr.tlasNodeCapacity, static_cast<uint32_t>(tlasNodes_.size()),
                     sizeof(RtBvhNode), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!tlasNodes_.empty()) {
            std::memcpy(fr.tlasNodes.info.pMappedData, tlasNodes_.data(),
                        tlasNodes_.size() * sizeof(RtBvhNode));
        }
    } else {
        buildTlas(cmd, fr);
    }
}

void RtScene::buildTlas(VkCommandBuffer cmd, FrameResources& fr) {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    const uint32_t count = static_cast<uint32_t>(asInstanceData_.size());

    ensureMapped(fr.asInstances, fr.asInstanceCapacity, count,
                 sizeof(VkAccelerationStructureInstanceKHR),
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    if (count > 0) {
        std::memcpy(fr.asInstances.info.pMappedData, asInstanceData_.data(),
                    count * sizeof(VkAccelerationStructureInstanceKHR));
    }

    VkAccelerationStructureGeometryKHR geom{};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.data.deviceAddress = bufferAddress(fr.asInstances.buffer);

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    pfnGetAsBuildSizes_(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count, &sizes);

    if (!fr.tlas || fr.tlasSize < sizes.accelerationStructureSize ||
        fr.tlasSize > std::max<VkDeviceSize>(65536, sizes.accelerationStructureSize * 3)) {
        if (fr.tlas) {
            VkAccelerationStructureKHR oldAs = fr.tlas;
            AllocatedBuffer oldBuf = fr.tlasBuffer;
            auto destroyAs = pfnDestroyAs_;
            ctx_->deferAfterAllFrameFences([dev, alloc, oldAs, oldBuf, destroyAs]() mutable {
                destroyAs(dev, oldAs, nullptr);
                destroyBuffer(alloc, oldBuf);
            });
        }
        fr.tlasSize = sizes.accelerationStructureSize + sizes.accelerationStructureSize / 2;
        fr.tlasBuffer = createBuffer(alloc, fr.tlasSize,
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VMA_MEMORY_USAGE_GPU_ONLY);
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = fr.tlasBuffer.buffer;
        ci.size = fr.tlasSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        fr.tlas = VK_NULL_HANDLE;
        if (pfnCreateAs_(dev, &ci, nullptr, &fr.tlas) != VK_SUCCESS) {
            fr.tlas = VK_NULL_HANDLE;
            return;
        }
        ++descriptorGeneration_;
    }
    if (!fr.scratch.buffer || fr.scratchSize < sizes.buildScratchSize ||
        fr.scratchSize > std::max<VkDeviceSize>(65536, sizes.buildScratchSize * 3)) {
        if (fr.scratch.buffer) {
            AllocatedBuffer old = fr.scratch;
            ctx_->deferAfterAllFrameFences([alloc, old]() mutable { destroyBuffer(alloc, old); });
        }
        fr.scratchSize = sizes.buildScratchSize + sizes.buildScratchSize / 2;
        fr.scratch = createBuffer(alloc, fr.scratchSize + scratchAlignment_,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  VMA_MEMORY_USAGE_GPU_ONLY);
    }

    const VkDeviceAddress scratchAddr = bufferAddress(fr.scratch.buffer);
    build.dstAccelerationStructure = fr.tlas;
    build.scratchData.deviceAddress = (scratchAddr + scratchAlignment_ - 1) & ~(scratchAlignment_ - 1);
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    pfnCmdBuildAs_(cmd, 1, &build, &ranges);

    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    cmdPipelineBarrier2(cmd, dep);
}

void RtScene::writeDescriptors(FrameResources& fr) {
    if (fr.descriptorGeneration == descriptorGeneration_) return;
    fr.descriptorGeneration = descriptorGeneration_;

    auto info = [&](const AllocatedBuffer& b) {
        return VkDescriptorBufferInfo{b.buffer ? b.buffer : placeholder_.buffer, 0, VK_WHOLE_SIZE};
    };
    VkDescriptorBufferInfo tris = info(triPool_.buffer);
    VkDescriptorBufferInfo nodes = info(nodePool_.buffer);
    VkDescriptorBufferInfo meshes = info(meshTable_);
    VkDescriptorBufferInfo insts = info(fr.instances);
    VkDescriptorBufferInfo tlasNodes = info(fr.tlasNodes);

    std::vector<VkWriteDescriptorSet> writes;
    auto add = [&](uint32_t binding, const VkDescriptorBufferInfo* bi) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = fr.set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = bi;
        writes.push_back(w);
    };
    add(kBindingTriangles, &tris);
    add(kBindingMeshes, &meshes);
    add(kBindingInstances, &insts);
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    if (backend_ == Backend::Software) {
        add(kBindingNodes, &nodes);
        add(kBindingTlasNodes, &tlasNodes);
    } else if (fr.tlas) {
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &fr.tlas;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.pNext = &asWrite;
        w.dstSet = fr.set;
        w.dstBinding = kBindingTlas;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(ctx_->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void RtScene::appendReceiverLayout(std::vector<VkDescriptorSetLayoutBinding>& bindings) const {
    for (uint32_t i : {0u, 2u, 3u})
        bindings.push_back({11+i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    if (backend_ == Backend::Software) {
        for (uint32_t i : {1u, 4u})
            bindings.push_back({11+i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    } else bindings.push_back({16, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
}
void RtScene::appendReceiverPool(std::vector<VkDescriptorPoolSize>& sizes, uint32_t sets) const {
    sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * (backend_ == Backend::Hardware ? 3u : 5u)});
    if (backend_ == Backend::Hardware) sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, sets});
}
void RtScene::writeReceiverDescriptors(VkDescriptorSet target, bool neutral) const {
    const auto& fr = neutral || !active_ ? emptyReceiver_ : frames_[ctx_->getCurrentFrame()];
    const AllocatedBuffer* buffers[] = {&triPool_.buffer, &nodePool_.buffer, &meshTable_, &fr.instances, &fr.tlasNodes};
    VkDescriptorBufferInfo infos[5]{};
    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t i=0; i<5; ++i) {
        if (backend_ == Backend::Hardware && (i==1 || i==4)) continue;
        infos[i] = {neutral || !active_ || !buffers[i]->buffer ? placeholder_.buffer : buffers[i]->buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=target; write.dstBinding=11+i; write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&infos[i];
        writes.push_back(write);
    }
    const VkAccelerationStructureKHR tlas = fr.tlas ? fr.tlas : emptyReceiver_.tlas;
    VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    if (backend_ == Backend::Hardware) {
        as.accelerationStructureCount=1; as.pAccelerationStructures=&tlas;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.pNext=&as; write.dstSet=target; write.dstBinding=16;
        write.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; write.descriptorCount=1;
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(ctx_->getDevice(), uint32_t(writes.size()), writes.data(), 0, nullptr);
}

void RtScene::observeLastBuild() {
    // The timings VkContext holds are this frame slot's previous frame, read
    // once its fence was waited on; so are frameBuilds_[slot].
    const uint32_t slot = ctx_->getCurrentFrame();
    FrameBuild& fb = frameBuilds_[slot];
    if (ctx_->gpuTimingSupported()) {
        double buildMs = -1.0, frameMs = 0.0;
        for (const auto& [label, ms] : ctx_->gpuTimings()) {
            frameMs += ms;
            if (label && std::strcmp(label, kGpuMarkLabel) == 0) buildMs = ms;
        }
        if (buildMs >= 0.0) {
            buildBudget_.observe({fb.triangles, buildMs, frameMs});
            if (buildMs > kSlowBuildMs) {
                LOG_WARNING("RtScene: scene update took ", buildMs, " ms of a ", frameMs,
                            " ms GPU frame building ", fb.triangles, " triangles in ", fb.meshes,
                            " meshes (largest ", fb.largest, "); next share ", buildBudget_.budget(),
                            " triangles, ", pendingMeshes_.size(), " meshes queued");
            }
        }
    }
    fb = {};
}

void RtScene::update(VkCommandBuffer cmd) {
    if (!ctx_ || !active_) return;
    observeLastBuild();
    compactPools(cmd);
    uploadPending(cmd);

    if (meshTableDirty_) {
        meshTableDirty_ = false;
        std::vector<MeshGPU> table(meshes_.size());
        for (size_t i = 0; i < meshes_.size(); ++i) {
            table[i] = {meshes_[i].nodeOffset, meshes_[i].triOffset, 0, 0};
        }
        { // Immutable table version: older frames may still trace its mesh rows.
            if (meshTable_.buffer) {
                VmaAllocator alloc = ctx_->getAllocator();
                AllocatedBuffer old = meshTable_;
                ctx_->deferAfterAllFrameFences([alloc, old]() mutable { destroyBuffer(alloc, old); });
            }
            meshTableCapacity_ = grownCapacity(meshTableCapacity_, static_cast<uint32_t>(table.size()));
            meshTable_ = createBuffer(ctx_->getAllocator(),
                                      VkDeviceSize(meshTableCapacity_) * sizeof(MeshGPU),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_MEMORY_USAGE_GPU_ONLY);
            ++descriptorGeneration_;
        }
        // Pool compaction and identity reuse can change existing rows. Older
        // frame descriptors retain the previous table and pool until fenced.
        stage(cmd, meshTable_.buffer, 0, table.data(), table.size() * sizeof(MeshGPU));
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        cmdPipelineBarrier2(cmd, dep);
    }

    FrameResources& fr = frames_[ctx_->getCurrentFrame()];
    uploadFrame(cmd, fr);
    writeDescriptors(fr);
}

VkDescriptorSet RtScene::descriptorSet() const {
    return ctx_ ? frames_[ctx_->getCurrentFrame()].set : VK_NULL_HANDLE;
}

RtScene::Stats RtScene::stats() const {
    Stats s;
    s.sourceBytes = sourceBytes_.load(std::memory_order_relaxed);
    s.meshes = liveMeshes_;
    s.instances = liveInstances_;
    s.triangles = liveTriangles_;
    auto sz = [](const AllocatedBuffer& b) { return b.buffer ? uint64_t(b.info.size) : 0ull; };
    s.gpuBytes = sz(triPool_.buffer) + sz(nodePool_.buffer) + sz(meshTable_);
    for (const auto& m : meshes_) {
        s.gpuBytes += sz(m.blasBuffer);
        if (!m.source) continue;
        s.sourceBytes += pipeline::vectorBytes(m.tris) + pipeline::vectorBytes(m.nodes);
    }
    s.sourceBytes += pipeline::vectorBytes(meshes_);
    return s;
}

}  // namespace wowee::rendering
