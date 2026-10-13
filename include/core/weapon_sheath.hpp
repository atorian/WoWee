#pragma once

// Where a character's weapons hang: in the hands when drawn, on the back and
// hips when sheathed, and on the back on a mount. One rule for the player's
// own character (AppearanceComposer) and for other players (EntitySpawner),
// who put their weapons away on a mount whatever their sheath state says.

#include "game/inventory.hpp"
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace core {

// M2 attachment 11 is the helm; 0 is the shield mount, which is where head
// gear was going - attached successfully, on the forearm, invisible on the head.
constexpr uint32_t kAttachShield = 0;
constexpr uint32_t kAttachRightHand = 1;
constexpr uint32_t kAttachLeftHand = 2;
constexpr uint32_t kAttachBack = 12;
// The model's own sheath points, which carry the sheathed orientation. Its
// plain hip points do not, and some models name a second hip point at the
// feet.
constexpr uint32_t kAttachSheathMainHand = 26;
constexpr uint32_t kAttachSheathOffHand = 27;
constexpr uint32_t kAttachSheathShield = 28;
constexpr uint32_t kAttachHipWeaponLeft = 32;
constexpr uint32_t kAttachHipWeaponRight = 33;

/// Every point a weapon can be on, held or sheathed.
constexpr uint32_t kWeaponAttachmentPoints[] = {
    kAttachShield, kAttachRightHand, kAttachLeftHand, kAttachBack,
    kAttachSheathMainHand, kAttachSheathOffHand, kAttachSheathShield,
    kAttachHipWeaponLeft, kAttachHipWeaponRight,
};

/// Where a rider carries a weapon: on the back, whatever it is. The hips are
/// against the mount's flanks, where a weapon is lost in the saddle, and a
/// held book or orb is kept rather than dropped. Every race's model has the
/// three back sheath points, and they carry the sheathed orientation.
inline uint32_t riderWeaponAttachment(game::EquipSlot slot, uint8_t inventoryType) {
    if (inventoryType == game::InvType::SHIELD) return kAttachSheathShield;
    return slot == game::EquipSlot::OFF_HAND ? kAttachSheathOffHand : kAttachSheathMainHand;
}

/// The attachment a weapon in `slot` (MAIN_HAND or OFF_HAND) hangs from.
/// UINT32_MAX for one with no sheath position - a held book or orb - which
/// the client hides while sheathed rather than pinning it to a bone.
inline uint32_t weaponAttachment(bool sheathed, game::EquipSlot slot, uint8_t inventoryType) {
    if (!sheathed) {
        if (inventoryType == game::InvType::SHIELD) return kAttachShield;
        return slot == game::EquipSlot::OFF_HAND ? kAttachLeftHand : kAttachRightHand;
    }

    if (inventoryType == game::InvType::TWO_HAND) return kAttachBack;
    if (inventoryType == game::InvType::SHIELD) return kAttachSheathShield;
    if (inventoryType == game::InvType::ONE_HAND ||
        inventoryType == game::InvType::MAIN_HAND) {
        // Main hand on the left hip, drawn across the body.
        return slot == game::EquipSlot::OFF_HAND ? kAttachHipWeaponRight : kAttachHipWeaponLeft;
    }
    return UINT32_MAX;
}

/// How the weapon sits on that attachment.
inline glm::mat4 weaponLocalTransform(bool sheathed, uint8_t inventoryType) {
    glm::mat4 transform(1.0f);
    if (!sheathed || inventoryType == game::InvType::SHIELD) return transform;

    if (inventoryType == game::InvType::TWO_HAND) {
        // Weapon models are authored along local X, which is also the
        // character's front/back axis. First rotate weapon X completely onto
        // character Z, then cant that vertical axis within the Y/Z back plane.
        // This ordering is important: rotating around X cannot change an
        // X-aligned blade. The final innermost roll spins the blade about its
        // own long axis so the flat rests against the back instead of the
        // sharp edge. Values tuned against the live attachment frame; its Z
        // axis moves the weapon laterally rather than vertically.
        transform = glm::translate(transform, glm::vec3(-0.03f, -0.10f, 0.0f));
        transform = glm::rotate(transform, glm::radians(33.0f), glm::vec3(1, 0, 0));
        transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(0, 1, 0));
        transform = glm::rotate(transform, glm::radians(90.0f), glm::vec3(1, 0, 0));
    }
    return transform;
}

} // namespace core
} // namespace wowee
