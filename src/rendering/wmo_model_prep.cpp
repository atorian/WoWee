// A WMO model's CPU-side work, done before its upload and on any thread
// (WMORenderer::prepareModel). The upload that follows, on the main thread,
// is buffers, textures and materials: WMORenderer::loadPreparedIncremental.
#include "wmo_model_prep.hpp"

#include "core/logger.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/wmo_lighting.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace wowee {
namespace rendering {

namespace {

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// The file a texture path names as the texture cache keys it: no stray bytes
/// after a NUL, backslashes, lowercase, no leading ".\" or "\", and the .blp a
/// .tga or .dds stands for. Two paths with one key load as one texture.
std::string textureFileKey(const std::string& path) {
    std::string key = path;
    if (const size_t nul = key.find('\0'); nul != std::string::npos) key.resize(nul);
    std::replace(key.begin(), key.end(), '/', '\\');
    key = lowercase(std::move(key));
    if (key.rfind(".\\", 0) == 0) key = key.substr(2);
    while (!key.empty() && key.front() == '\\') key.erase(key.begin());
    if (key.size() >= 4) {
        const std::string ext = key.substr(key.size() - 4);
        if (ext == ".tga" || ext == ".dds") key = key.substr(0, key.size() - 4) + ".blp";
    }
    return key;
}

} // namespace

bool WMORenderer::prepareGroupResources(const pipeline::WMOGroup& group, GroupResources& resources,
                                        std::vector<WMOVertex>& vertices) {
    if (group.vertices.empty() || group.indices.empty()) {
        return false;
    }
    const uint32_t groupFlags = group.flags;

    resources.groupFlags = groupFlags;

    resources.vertexCount = group.vertices.size();
    resources.indexCount = group.indices.size();
    resources.boundingBoxMin = group.boundingBoxMin;
    resources.boundingBoxMax = group.boundingBoxMax;

    vertices.clear();
    vertices.reserve(group.vertices.size());

    for (const auto& v : group.vertices) {
        WMOVertex vd;
        vd.position = v.position;
        vd.normal = v.normal;
        vd.texCoord = v.texCoord;
        vd.color = v.color;
        vd.tangent = glm::vec4(0.0f);
        vertices.push_back(vd);
    }

    // Compute tangents using Lengyel's method
    {
        std::vector<glm::vec3> tan1(vertices.size(), glm::vec3(0.0f));
        std::vector<glm::vec3> tan2(vertices.size(), glm::vec3(0.0f));

        const auto& indices = group.indices;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            uint16_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

            const glm::vec3& p0 = vertices[i0].position;
            const glm::vec3& p1 = vertices[i1].position;
            const glm::vec3& p2 = vertices[i2].position;
            const glm::vec2& uv0 = vertices[i0].texCoord;
            const glm::vec2& uv1 = vertices[i1].texCoord;
            const glm::vec2& uv2 = vertices[i2].texCoord;

            glm::vec3 dp1 = p1 - p0;
            glm::vec3 dp2 = p2 - p0;
            glm::vec2 duv1 = uv1 - uv0;
            glm::vec2 duv2 = uv2 - uv0;

            float det = duv1.x * duv2.y - duv1.y * duv2.x;
            if (std::abs(det) < 1e-8f) continue;  // degenerate UVs
            float r = 1.0f / det;

            glm::vec3 sdir = (dp1 * duv2.y - dp2 * duv1.y) * r;
            glm::vec3 tdir = (dp2 * duv1.x - dp1 * duv2.x) * r;

            tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
            tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
        }

        for (size_t i = 0; i < vertices.size(); i++) {
            // Vertex normals from corrupt WMO data could be zero-length or
            // NaN. glm::normalize on either returns NaN that contaminates
            // the entire Gram-Schmidt tangent below; fall back to up-axis.
            glm::vec3 n;
            float normLen = glm::length(vertices[i].normal);
            if (std::isfinite(normLen) && normLen > 1e-6f) {
                n = vertices[i].normal / normLen;
            } else {
                n = glm::vec3(0, 0, 1);
            }
            glm::vec3 t = tan1[i];

            if (glm::dot(t, t) < 1e-8f) {
                // Fallback: generate tangent perpendicular to normal
                glm::vec3 up = (std::abs(n.y) < 0.999f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                t = glm::normalize(glm::cross(n, up));
            }

            // Gram-Schmidt orthogonalize
            t = glm::normalize(t - n * glm::dot(n, t));
            float w = (glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f;
            vertices[i].tangent = glm::vec4(t, w);
        }
    }

    // What the group is solid with: every triangle, each tagged with its MOPY
    // flags, which decide what blocks (detail - gears, railings - does not).
    std::vector<glm::vec3> collisionVertices;
    collisionVertices.reserve(group.vertices.size());
    for (const auto& v : group.vertices) collisionVertices.push_back(v.position);
    // The floor's colour lights a unit standing on it, in an interior group
    // that has colours.
    std::vector<glm::vec3> vertexColors;
    if ((groupFlags & pipeline::kWMOGroupIndoor) && (groupFlags & pipeline::kWMOGroupHasVertexColors)) {
        vertexColors.reserve(group.vertices.size());
        for (const auto& v : group.vertices) vertexColors.push_back(glm::vec3(v.color));
    }
    std::vector<uint8_t> mopyFlags;
    if (!group.triFlags.empty()) {
        const size_t numTris = group.indices.size() / 3;
        mopyFlags.resize(numTris, 0);
        for (size_t t = 0; t < numTris; t++) {
            mopyFlags[t] = (t < group.triFlags.size()) ? group.triFlags[t] : 0;
        }
    }

    // The box from the vertices themselves: WMO header boxes can be wrong.
    if (!collisionVertices.empty()) {
        resources.boundingBoxMin = collisionVertices[0];
        resources.boundingBoxMax = collisionVertices[0];
        for (const auto& v : collisionVertices) {
            resources.boundingBoxMin = glm::min(resources.boundingBoxMin, v);
            resources.boundingBoxMax = glm::max(resources.boundingBoxMax, v);
        }
    }

    // Says at warning when the group offers no floor at all: Darkshore's
    // bridges are WMOs whose every triangle is detail, and a floor that cannot
    // be found shows up as nothing but falling.
    resources.shape = collision::WmoGroupShape::build(
        groupFlags, resources.boundingBoxMin, resources.boundingBoxMax,
        std::move(collisionVertices), group.indices, std::move(vertexColors),
        std::move(mopyFlags));

    // Create batches
    if (!group.batches.empty()) {
        for (size_t i = 0; i < group.batches.size(); ++i) {
            const auto& batch = group.batches[i];
            GroupResources::Batch resBatch;
            resBatch.startIndex = batch.startIndex;
            resBatch.indexCount = batch.indexCount;
            resBatch.materialId = batch.materialId;
            resBatch.kind = group.batchKind(i);
            resources.batches.push_back(resBatch);
        }
    } else {
        // No batches defined - render entire group as one batch
        GroupResources::Batch batch;
        batch.startIndex = 0;
        batch.indexCount = resources.indexCount;
        batch.materialId = 0;
        resources.batches.push_back(batch);
    }

    return true;
}

std::shared_ptr<WmoModelPrep> WMORenderer::prepareModel(const pipeline::WMOModel& model) {
    auto prep = std::make_shared<WmoModelPrep>();
    if (!model.isValid()) return prep;

    prep->boundingBoxMin = model.boundingBoxMin;
    prep->boundingBoxMax = model.boundingBoxMax;
    prep->ambientColor = model.ambientColor;
    {
        glm::vec3 ext = model.boundingBoxMax - model.boundingBoxMin;
        float horiz = std::max(ext.x, ext.y);
        float vert = ext.z;
        prep->isLowPlatform = (vert < 6.0f && horiz > 20.0f);
    }
    std::string lowerSourcePath = model.sourcePath;
    std::replace(lowerSourcePath.begin(), lowerSourcePath.end(), '/', '\\');
    lowerSourcePath = lowercase(std::move(lowerSourcePath));
    const bool isStormwindCityWmo =
        lowerSourcePath.find("\\buildings\\stormwind\\stormwind.wmo") != std::string::npos;

    // The texture list, and for each texture the first in the list naming the
    // same file - what a batch binds, as far as merging batches goes.
    prep->texturePaths = model.textures;
    std::vector<uint32_t> sameFileAs(model.textures.size(), UINT32_MAX);
    {
        std::unordered_map<std::string, uint32_t> firstWithKey;
        for (size_t i = 0; i < model.textures.size(); ++i) {
            prep->textureNames.push_back(lowercase(model.textures[i]));
            const std::string key = textureFileKey(model.textures[i]);
            if (key.empty()) continue;  // no file: the white fallback
            sameFileAs[i] = firstWithKey.emplace(key, static_cast<uint32_t>(i)).first->second;
        }
    }

    // Store material -> texture index mapping
    // IMPORTANT: mat.texture1 is a byte offset into MOTX, not an array index!
    // We need to convert it using the textureOffsetToIndex map
    auto resolveTextureIndex = [&](uint32_t textureField) -> uint32_t {
        auto it = model.textureOffsetToIndex.find(textureField);
        if (it != model.textureOffsetToIndex.end()) {
            return it->second;
        }
        // Some files may store direct index instead of MOTX byte offset.
        if (textureField < model.textures.size()) {
            return textureField;
        }
        return std::numeric_limits<uint32_t>::max();
    };
    for (const auto& mat : model.materials) {
        uint32_t texIndex = 0;  // Default to first texture
        // Prefer first valid non-empty texture among texture1/2/3.
        auto pickValid = [&](uint32_t idx) -> bool {
            if (idx == std::numeric_limits<uint32_t>::max()) return false;
            if (idx >= model.textures.size()) return false;
            if (model.textures[idx].empty()) return false;
            texIndex = idx;
            return true;
        };
        if (!pickValid(resolveTextureIndex(mat.texture1))) {
            if (!pickValid(resolveTextureIndex(mat.texture2))) {
                pickValid(resolveTextureIndex(mat.texture3));
            }
        }
        prep->materialTextureIndices.push_back(texIndex);
        prep->materialBlendModes.push_back(mat.blendMode);
        prep->materialFlags.push_back(mat.flags);
        prep->materialSidnColors.push_back(mat.sidnColor);
    }

    // Helper: look up group name from MOGN raw data via MOGI nameOffset
    auto getGroupName = [&](uint32_t groupIdx) -> std::string {
        if (groupIdx < model.groupInfo.size()) {
            int32_t nameOff = model.groupInfo[groupIdx].nameOffset;
            if (nameOff >= 0 && static_cast<size_t>(nameOff) < model.groupNameRaw.size()) {
                const char* str = reinterpret_cast<const char*>(model.groupNameRaw.data() + nameOff);
                size_t maxLen = model.groupNameRaw.size() - nameOff;
                return std::string(str, strnlen(str, maxLen));
            }
        }
        return {};
    };

    for (size_t gi = 0; gi < model.groups.size(); gi++) {
        const auto& wmoGroup = model.groups[gi];
        WmoModelPrep::Group g;
        if (!prepareGroupResources(wmoGroup, g.resources, g.vertices)) continue;
        g.indices = wmoGroup.indices;
        GroupResources& resources = g.resources;

        // Detect distance-only LOD/exterior shell groups:
        // 1. Very low vertex count (<100) - portal connectors, tiny shells
        // 2. ALWAYS_DRAW (0x10000) with low verts - distant LOD stand-ins
        // 3. Pure OUTDOOR groups (0x8 set, 0x2000 not set) in large WMOs -
        //    exterior cityscape shells (e.g. "city01" in Stormwind)
        bool alwaysDraw = (wmoGroup.flags & 0x10000) != 0;
        size_t nVerts = wmoGroup.vertices.size();
        bool isLargeWmo = model.nGroups > 50;
        // Detect facade groups by name (exterior face of buildings)
        std::string gname = getGroupName(static_cast<uint32_t>(gi));
        bool isFacade = false;
        bool isCityShell = false;
        if (!gname.empty()) {
            const std::string lower = lowercase(gname);
            isFacade = lower.find("facade") != std::string::npos;
            // "city01" etc are exterior cityscape shells in large WMOs
            isCityShell = (lower.find("city") == 0 && lower.size() <= 8);
        }
        bool isIndoor = (wmoGroup.flags & 0x2000) != 0;
        const bool isStormwindCathedralShell = isStormwindCityWmo && isLargeWmo &&
                                               isIndoor && (wmoGroup.flags & 0x80) != 0;
        if ((nVerts < 100 && isLargeWmo && !isIndoor) ||
            (alwaysDraw && nVerts < 5000 && isLargeWmo && !isIndoor) ||
            (isFacade && isLargeWmo && !isIndoor) ||
            (isCityShell && !isIndoor && isLargeWmo) ||
            isStormwindCathedralShell) {
            resources.isLOD = true;
        }

        // What each batch draws with. Batches merge only when all of it
        // matches; see rendering/wmo_batch_merge.hpp.
        std::vector<WmoBatchState> batchStates;
        batchStates.reserve(resources.batches.size());
        for (const auto& batch : resources.batches) {
            uint32_t texture = UINT32_MAX;
            if (batch.materialId < prep->materialTextureIndices.size()) {
                const uint32_t texIndex = prep->materialTextureIndices[batch.materialId];
                if (texIndex < model.textures.size()) {
                    texture = sameFileAs[texIndex];
                } else {
                    LOG_WARNING("WMO '", model.sourcePath, "' batch materialId=", batch.materialId,
                                " texIndex=", texIndex, " >= textures size ",
                                model.textures.size(), " - white fallback");
                }
            } else {
                LOG_WARNING("WMO '", model.sourcePath, "' batch materialId=", batch.materialId,
                            " >= material count ", prep->materialTextureIndices.size(),
                            " - white fallback");
            }

            uint32_t blendMode = 0;
            if (batch.materialId < prep->materialBlendModes.size()) {
                blendMode = prep->materialBlendModes[batch.materialId];
            }
            uint32_t matFlags = 0;
            if (batch.materialId < prep->materialFlags.size()) {
                matFlags = prep->materialFlags[batch.materialId];
            }
            // F_SIDN: the material glows in its sidnColor - windows and lamps.
            uint32_t sidnColor = 0;
            if ((matFlags & 0x10) != 0 && batch.materialId < prep->materialSidnColors.size()) {
                sidnColor = prep->materialSidnColors[batch.materialId];
            }

            bool isLava = false;
            bool isCloth = false;
            uint8_t emissiveLevel = 0;
            if (batch.materialId < prep->materialTextureIndices.size()) {
                const uint32_t ti = prep->materialTextureIndices[batch.materialId];
                if (ti < prep->textureNames.size()) {
                    const std::string& texNameLower = prep->textureNames[ti];
                    if (texNameLower.find("stormwindlampglass.blp") != std::string::npos) {
                        emissiveLevel = 1;  // authored lamp glass: bright
                    } else if (texNameLower.find("mm_clockface") != std::string::npos) {
                        // Darkshire's town hall clock, and any building sharing the
                        // face: backlit by a flickering fire in the tower.
                        emissiveLevel = 2;
                    }
                    isLava = (texNameLower.find("lava") != std::string::npos ||
                              texNameLower.find("molten") != std::string::npos ||
                              texNameLower.find("magma") != std::string::npos);
                    // Cloth painted into the building: the gate banners of
                    // Stormwind are geometry in the city's own WMO, textured
                    // STORMWINDBANNER01, not doodads with models of their own.
                    // A flagstone is a floor, as ever.
                    isCloth = texNameLower.find("banner") != std::string::npos ||
                              texNameLower.find("tapestry") != std::string::npos ||
                              texNameLower.find("pennant") != std::string::npos ||
                              (texNameLower.find("flag") != std::string::npos &&
                               texNameLower.find("flagstone") == std::string::npos);
                }
            }

            // Where that cloth hangs, from the vertices this batch draws:
            // the top edge it is held at, how far it falls, and its middle -
            // which is what makes two banners on one gate swing out of step.
            float clothTop = 0.0f, clothDrop = 0.0f;
            float clothCx = 0.0f, clothCy = 0.0f;
            if (isCloth) {
                float minZ = std::numeric_limits<float>::max();
                float maxZ = -std::numeric_limits<float>::max();
                float minX = minZ, maxX = maxZ, minY = minZ, maxY = maxZ;
                for (uint32_t i = 0; i < batch.indexCount; ++i) {
                    const uint32_t at = batch.startIndex + i;
                    if (at >= wmoGroup.indices.size()) break;
                    const uint16_t vi = wmoGroup.indices[at];
                    if (vi >= wmoGroup.vertices.size()) continue;
                    const auto& v = wmoGroup.vertices[vi].position;
                    minZ = std::min(minZ, v.z); maxZ = std::max(maxZ, v.z);
                    minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
                    minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
                }
                if (maxZ > minZ) {
                    clothTop = maxZ;
                    clothDrop = maxZ - minZ;
                    clothCx = (minX + maxX) * 0.5f;
                    clothCy = (minY + maxY) * 0.5f;
                }
            }

            batchStates.push_back({ .texture = texture, .blendMode = blendMode,
                                    .lighting = wmoBatchLighting(model.flags, resources.groupFlags,
                                                                 batch.kind, matFlags),
                                    .sidnColor = sidnColor, .isLava = isLava,
                                    .emissiveLevel = emissiveLevel,
                                    .clothTop = clothTop, .clothDrop = clothDrop,
                                    .clothCentreX = clothCx, .clothCentreY = clothCy });
        }

        bool bindsATexture = false;
        for (const auto& members : groupWmoBatches(batchStates)) {
            WmoModelPrep::Merge merge;
            merge.state = batchStates[members.front()];
            if (merge.state.texture != UINT32_MAX) bindsATexture = true;
            for (size_t i : members) {
                const auto& batch = resources.batches[i];
                merge.draws.push_back({ .firstIndex = batch.startIndex, .indexCount = batch.indexCount });
            }
            g.merges.push_back(std::move(merge));
        }
        // A group whose batches name no texture of their own is collision or
        // a placeholder; the ray-traced scene leaves it out.
        resources.allUntextured = !bindsATexture && !g.merges.empty();

        prep->groups.push_back(std::move(g));
    }

    // Copy portal data for visibility culling
    prep->portalVertices = model.portalVertices;
    for (const auto& portal : model.portals) {
        PortalData pd;
        pd.startVertex = portal.startVertex;
        pd.vertexCount = portal.vertexCount;
        // Compute portal plane from vertices if we have them
        if (portal.vertexCount >= 3 && portal.startVertex + portal.vertexCount <= model.portalVertices.size()) {
            glm::vec3 v0 = model.portalVertices[portal.startVertex];
            glm::vec3 v1 = model.portalVertices[portal.startVertex + 1];
            glm::vec3 v2 = model.portalVertices[portal.startVertex + 2];
            // Degenerate portal (collinear or coincident verts) → cross is
            // zero → normalize returns NaN. Fall back to up-axis instead of
            // poisoning the portal-frustum cull.
            glm::vec3 cross = glm::cross(v1 - v0, v2 - v0);
            float crossLen = glm::length(cross);
            if (crossLen > 1e-6f) {
                pd.normal = cross / crossLen;
                pd.distance = glm::dot(pd.normal, v0);
            } else {
                pd.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                pd.distance = 0.0f;
            }
        } else {
            pd.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            pd.distance = 0.0f;
        }
        prep->portals.push_back(pd);
    }
    for (const auto& ref : model.portalRefs) {
        PortalRef pr;
        pr.portalIndex = ref.portalIndex;
        pr.groupIndex = ref.groupIndex;
        pr.side = ref.side;
        prep->portalRefs.push_back(pr);
    }
    // Build per-group portal ref ranges from WMOGroup data
    prep->groupPortalRefs.resize(model.groups.size(), {0, 0});
    for (size_t gi = 0; gi < model.groups.size(); gi++) {
        prep->groupPortalRefs[gi] = {model.groups[gi].portalStart, model.groups[gi].portalCount};
    }

    // Store doodad templates (M2 models placed in WMO) for instancing later
    if (!model.doodadSets.empty() && !model.doodads.empty()) {
        const auto& doodadSet = model.doodadSets[0];  // Use first doodad set
        for (uint32_t di = 0; di < doodadSet.count; di++) {
            uint32_t doodadIdx = doodadSet.startIndex + di;
            if (doodadIdx >= model.doodads.size()) break;

            const auto& doodad = model.doodads[doodadIdx];
            auto nameIt = model.doodadNames.find(doodad.nameIndex);
            if (nameIt == model.doodadNames.end()) continue;

            std::string m2Path = nameIt->second;
            if (m2Path.empty()) continue;

            m2Path = pipeline::modelPathToM2(m2Path);

            // Build doodad's local transform (WoW coordinates)
            // WMO doodads use quaternion rotation
            glm::quat fixedRotation(doodad.rotation.w, doodad.rotation.x, doodad.rotation.y, doodad.rotation.z);

            glm::mat4 localTransform(1.0f);
            localTransform = glm::translate(localTransform, doodad.position);
            localTransform *= glm::mat4_cast(fixedRotation);
            localTransform = glm::scale(localTransform, glm::vec3(doodad.scale));

            DoodadTemplate doodadTemplate;
            doodadTemplate.m2Path = m2Path;
            doodadTemplate.localTransform = localTransform;
            prep->doodadTemplates.push_back(doodadTemplate);
        }
    }

    // The ray-traced scene's mesh: the groups drawn up close, opaque and
    // alpha-tested batches only. Distance shells and groups the renderer
    // never draws would cast shadows the player cannot see the source of.
    for (const auto& g : prep->groups) {
        const GroupResources& group = g.resources;
        if (group.isLOD || group.allUntextured || (group.groupFlags & 0x4000000u)) continue;
        const collision::WmoGroupShape& shape = *group.shape;
        if (shape.vertices.empty()) continue;
        const uint32_t base = static_cast<uint32_t>(prep->rtMesh.positions.size());
        prep->rtMesh.positions.insert(prep->rtMesh.positions.end(), shape.vertices.begin(),
                                      shape.vertices.end());
        for (const auto& batch : group.batches) {
            const uint32_t mat = batch.materialId;
            const uint32_t blend = mat < prep->materialBlendModes.size() ? prep->materialBlendModes[mat] : 0;
            if (blend >= 2) continue;  // blended: glass, light cards, water sheets
            const uint32_t end = std::min<uint32_t>(batch.startIndex + batch.indexCount,
                                                    static_cast<uint32_t>(shape.indices.size()));
            for (uint32_t i = batch.startIndex; i + 2 < end; i += 3) {
                prep->rtMesh.indices.push_back(base + shape.indices[i]);
                prep->rtMesh.indices.push_back(base + shape.indices[i + 1]);
                prep->rtMesh.indices.push_back(base + shape.indices[i + 2]);
                prep->rtTriangleMaterial.push_back(mat);
            }
        }
    }

    return prep;
}

} // namespace rendering
} // namespace wowee
