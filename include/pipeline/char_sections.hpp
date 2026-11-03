#pragma once

/**
 * char_sections.hpp - reading a character's textures out of CharSections.dbc.
 *
 * One scan of that table, used by everything that draws a character: the player,
 * an NPC, another player, and the portrait.
 *
 * It was written three times before this - once in appearance_composer for the
 * player, once in entity_spawner for NPCs, once in character_preview for the
 * portrait - and the three did not agree. Only one of them read the skin row's
 * second texture, so ears and eyelashes were unbound everywhere else. Only one
 * had a fallback for a face the table does not carry. Every fault found in this
 * area had to be fixed two or three times, and each time one was missed the
 * report came back as "correct in the portrait, wrong in the world".
 *
 * The scan is here. What a caller does with the paths - composite them, hand
 * them to a model, put them in a portrait - stays with the caller.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "pipeline/dbc_layout.hpp"

namespace wowee {
namespace pipeline {

class DBCFile;

/// Which character these textures are for.
struct CharacterAppearance {
    uint32_t raceId = 0;
    uint32_t sexId = 0;      ///< 0 male, 1 female
    uint8_t skinId = 0;
    uint8_t faceId = 0;
    uint8_t hairStyleId = 0;
    uint8_t hairColorId = 0;
};

/// The textures CharSections names for that character.
struct CharacterSectionTextures {
    std::string bodySkin;
    /// The skin row's SECOND texture. Blank in the tables the game shipped and
    /// filled in by an HD one, where it is the art a model asks for as texture
    /// type 8 - the ears, the eyes, the mouth, the eyelashes.
    std::string skinExtra;
    std::string faceLower;
    std::string faceUpper;
    std::string hair;
    std::vector<std::string> underwear;

    /// True when the exact (face, skin) pair was found rather than approximated.
    bool exactFace = false;
    /// True when a face was found at all, exact or not.
    bool haveFace = false;
    bool haveHair = false;
};

/// Scan CharSections.dbc for one character's textures.
///
/// `keepUnderwear` is asked for each underwear path before it is kept - the
/// tables name art that is not always present, and a caller that can check the
/// filesystem should. Pass nothing to keep them all.
CharacterSectionTextures resolveCharacterSections(
    const DBCFile* charSections,
    const CharSectionsFields& fields,
    const CharacterAppearance& who,
    bool (*keepUnderwear)(const std::string&, void*) = nullptr,
    void* keepUnderwearContext = nullptr);

/// The art for a character model's Skin Extra slot (texture type 8): a
/// head-detail sheet for a human or an orc, the ears for a night elf, the
/// horns for a draenei male, the tail for a draenei female. Seven of the
/// twenty race and sex pairs name none, and take the first underwear, else
/// the body skin - art that exists rather than nothing. Empty only when the
/// character has no body skin either.
///
/// The slot is the client's to fill: whatever name a model carries in it is
/// not a file (one in the wild says 'Ohren'), and the renderer reads no name
/// but type 0's - so the art is bound to the slot, not written into the model.
const std::string& skinExtraArt(const std::string& skinExtra, const std::vector<std::string>& underwear,
                                const std::string& bodySkin);

}  // namespace pipeline
}  // namespace wowee
