#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game {

/// What the emotes are: their chat commands, the text they print and the
/// animation each plays, from EmotesText.dbc, EmotesTextData.dbc and
/// Emotes.dbc (a built-in table when those cannot be read). Loaded on first
/// use. Game data - the chat, the units' emote states and the interface all
/// ask it; the client owns one and hands it over (GameServices::emotes).
struct EmoteInfo {
    uint32_t animId = 0;
    uint32_t dbcId = 0;
    bool loop = false;
    std::string textNoTarget;
    std::string textTarget;
    std::string othersNoTarget;
    std::string othersTarget;
    std::string command;
};

class EmoteTable {
public:
    /// Without an asset manager only the built-in table is known.
    explicit EmoteTable(pipeline::AssetManager* assets) : assets_(assets) {}
    EmoteTable(const EmoteTable&) = delete;
    EmoteTable& operator=(const EmoteTable&) = delete;

    struct EmoteResult { uint32_t animId; bool loop; };

    /// Look up an emote by chat command (e.g. "dance", "wave").
    [[nodiscard]] std::optional<EmoteResult> findEmote(const std::string& command) const;


    /// Get the animation ID for an Emotes.dbc emote ID, as used by SMSG_EMOTE
    /// and UNIT_NPC_EMOTESTATE.
    [[nodiscard]] uint32_t animByEmotesId(uint32_t emoteId) const;

    /// True if the Emotes.dbc entry is a persistent state emote (EmoteSpecProc
    /// != 0, e.g. STATE_WORK_NOSHEATHE) rather than a one-shot (ONESHOT_WAVE).
    [[nodiscard]] bool isStateEmote(uint32_t emoteId) const;


    /// Get first-person emote text for a command.
    std::string textFor(const std::string& emoteName,
                        const std::string* targetName = nullptr) const;

    /// Get DBC ID for an emote command.
    [[nodiscard]] uint32_t dbcIdFor(const std::string& emoteName) const;

    /// Get third-person emote text by DBC ID.
    std::string textByDbcId(uint32_t dbcId,
                            const std::string& senderName,
                            const std::string* targetName = nullptr) const;


private:
    /// Reads the DBCs the first time anything is asked.
    void ensureLoaded() const;
    void loadFallbackEmotes() const;
    void buildDbcIdIndex() const;

    pipeline::AssetManager* assets_;
    // Filled on first use, so a const question can be the one that loads them.
    mutable bool loaded_ = false;
    mutable std::unordered_map<std::string, EmoteInfo> emoteTable_;
    mutable std::unordered_map<uint32_t, const EmoteInfo*> emoteByDbcId_;
    mutable std::unordered_map<uint32_t, uint32_t> animByEmotesId_;
    mutable std::unordered_set<uint32_t> stateEmoteIds_;
};

} // namespace game
} // namespace wowee
