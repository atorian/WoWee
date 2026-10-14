// Mana Shield's card, through the renderer's own bone update: whichever way
// the unit carrying it faces, the card faces a fixed camera. Its root bone is
// a billboard locked to Z (flags 0x240), its quads lie in the model's YZ
// plane and hang from that bone's children.
#include <catch_amalgamated.hpp>
#include "test_support.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_renderer_internal.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace wowee;

TEST_CASE("Mana Shield's card faces the camera whichever way its unit faces", "[m2][billboard][asset]") {
    const auto bytes = test::readFile("Data/expansions/wotlk/spells/manashield_state_base.m2");
    if (bytes.empty()) SKIP("Data/expansions/wotlk is not extracted here");
    const pipeline::M2Model model = pipeline::M2Loader::load(bytes);
    REQUIRE(model.bones.size() >= 2);
    REQUIRE((model.bones[0].flags & 0x40) != 0);

    rendering::M2ModelGPU gpu;
    gpu.bones = model.bones;
    gpu.sequences = model.sequences;
    gpu.globalSequenceDurations = model.globalSequenceDurations;

    // A camera south of the unit, a little above, looking at it.
    const glm::vec3 unitPos(100.0f, 200.0f, 50.0f);
    const glm::vec3 eye = unitPos + glm::vec3(0.0f, -8.0f, 3.0f);
    const glm::mat4 view = glm::lookAt(eye, unitPos + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    for (const float yawDeg : {0.0f, 45.0f, 90.0f, 180.0f, 270.0f}) {
        rendering::M2Instance inst;
        inst.modelMatrix = glm::rotate(glm::translate(glm::mat4(1.0f), unitPos), glm::radians(yawDeg),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
        inst.invModelMatrix = glm::inverse(inst.modelMatrix);
        inst.currentSequenceIndex = 0;
        inst.animTime = 350.0f;  // the card at full size
        rendering::computeBoneMatrices(gpu, inst, &view);

        // The card's normal is the model's X axis, carried by a child bone.
        const glm::mat4 world = inst.modelMatrix * inst.boneMatrices[1];
        const glm::vec3 normal = glm::normalize(glm::vec3(world[0]));
        const glm::vec3 toCamera = glm::normalize(eye - glm::vec3(world * glm::vec4(0.21f, 0.0f, 1.11f, 1.0f)));
        INFO("unit facing " << yawDeg << " degrees");
        // Turned about its own Z only, so square on across the ground.
        const glm::vec3 flatNormal = glm::normalize(glm::vec3(normal.x, normal.y, 0.0f));
        const glm::vec3 flatToCamera = glm::normalize(glm::vec3(toCamera.x, toCamera.y, 0.0f));
        CHECK(std::abs(glm::dot(flatNormal, flatToCamera)) == Catch::Approx(1.0f).margin(0.02));
    }
}
