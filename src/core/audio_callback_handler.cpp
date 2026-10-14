#include "core/audio_callback_handler.hpp"
#include "game/game_events.hpp"
#include "core/player_view.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "game/game_handler.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/music_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "game/character.hpp"
#include "game/entity.hpp"
#include "ui/ui_manager.hpp"

namespace wowee { namespace core {

AudioCallbackHandler::AudioCallbackHandler(
    pipeline::AssetManager& assetManager,
    audio::AudioCoordinator* audioCoordinator,
    rendering::Renderer* renderer,
    PlayerView* playerView,
    ui::UIManager* uiManager,
    game::GameHandler& gameHandler)
    : assetManager_(assetManager)
    , audioCoordinator_(audioCoordinator)
    , renderer_(renderer)
    , playerView_(playerView)
    , uiManager_(uiManager)
    , gameHandler_(gameHandler)
{
}

template <class Play>
void AudioCallbackHandler::playUi(Play&& play) const {
    if (!audioCoordinator_) return;
    if (auto* sfx = audioCoordinator_->getUiSoundManager()) play(*sfx);
}

namespace {

/// The line the character says for a refusal. Every reason the game gives has
/// one; the mapping is here because which line answers is the client's choice.
audio::PlayerErrorSpeech speechFor(game::Refusal reason) {
    using game::Refusal;
    using audio::PlayerErrorSpeech;
    switch (reason) {
        case Refusal::NoMana:              return PlayerErrorSpeech::NO_MANA;
        case Refusal::NoRage:              return PlayerErrorSpeech::NO_RAGE;
        case Refusal::NoEnergy:            return PlayerErrorSpeech::NO_ENERGY;
        case Refusal::OutOfRange:          return PlayerErrorSpeech::OUT_OF_RANGE;
        case Refusal::NoTarget:            return PlayerErrorSpeech::GENERIC_NO_TARGET;
        case Refusal::InvalidAttackTarget: return PlayerErrorSpeech::INVALID_ATTACK_TARGET;
        case Refusal::SpellCooldown:       return PlayerErrorSpeech::SPELL_COOLDOWN;
        case Refusal::AbilityCooldown:     return PlayerErrorSpeech::ABILITY_COOLDOWN;
        case Refusal::ItemCooldown:        return PlayerErrorSpeech::ITEM_COOLDOWN;
        case Refusal::NoAmmo:              return PlayerErrorSpeech::NO_AMMO;
        case Refusal::AmmoOnly:            return PlayerErrorSpeech::AMMO_ONLY;
        case Refusal::ChestInUse:          return PlayerErrorSpeech::CHEST_IN_USE;
        case Refusal::InventoryFull:       return PlayerErrorSpeech::INVENTORY_FULL;
        case Refusal::BagFull:             return PlayerErrorSpeech::BAG_FULL;
        case Refusal::ItemMaxCount:        return PlayerErrorSpeech::ITEM_MAX_COUNT;
        case Refusal::NotABag:             return PlayerErrorSpeech::NOT_A_BAG;
        case Refusal::CantUseItem:         return PlayerErrorSpeech::CANT_USE_ITEM;
        case Refusal::ItemLocked:          return PlayerErrorSpeech::ITEM_LOCKED;
        case Refusal::NotEquippable:       return PlayerErrorSpeech::NOT_EQUIPPABLE;
        case Refusal::CantEquipLevel:      return PlayerErrorSpeech::CANT_EQUIP_LEVEL;
        case Refusal::CantEquipEver:       return PlayerErrorSpeech::CANT_EQUIP_EVER;
        case Refusal::CantDropSoulbound:   return PlayerErrorSpeech::CANT_DROP_SOULBOUND;
        case Refusal::NotEnoughMoney:      return PlayerErrorSpeech::NOT_ENOUGH_MONEY;
        case Refusal::CantLearnSpell:      return PlayerErrorSpeech::CANT_LEARN_SPELL;
    }
    return PlayerErrorSpeech::CANT_USE_ITEM;
}

} // namespace

void AudioCallbackHandler::subscribeGameSounds() {
    namespace ev = game::events;
    auto& bus = gameHandler_.eventBus();

    // The error line's sound. It belongs to no element - the error is the same
    // error whoever draws the text - and FrameXML plays none for these.
    subscriptions_.push_back(bus.subscribe<ev::UiErrorRaised>([this](const ev::UiErrorRaised&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playError(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::ThreatWarningRaised>([this](const ev::ThreatWarningRaised&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playError(); });
    }));

    // The character answering a refusal aloud, in its own race's and sex's voice.
    subscriptions_.push_back(bus.subscribe<ev::PlayerRefused>([this](const ev::PlayerRefused& e) {
        if (!audioCoordinator_) return;
        auto* voice = audioCoordinator_->getPlayerVoiceManager();
        const game::Character* ch = gameHandler_.getActiveCharacter();
        if (!voice || !ch) return;
        voice->playError(speechFor(e.reason), static_cast<uint8_t>(ch->race),
                         static_cast<uint8_t>(game::toServerGender(ch->gender)));
    }));

    subscriptions_.push_back(bus.subscribe<ev::LootMoneyReceived>([this](const ev::LootMoneyReceived& e) {
        playUi([&](audio::UiSoundManager& sfx) {
            if (e.copper >= 10000) sfx.playLootCoinLarge();
            else sfx.playLootCoinSmall();
        });
    }));
    subscriptions_.push_back(bus.subscribe<ev::ItemReceived>([this](const ev::ItemReceived&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playLootItem(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::ItemSold>([this](const ev::ItemSold&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playDropOnGround(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::ItemBought>([this](const ev::ItemBought&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playPickupBag(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::MailArrived>([this](const ev::MailArrived&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playMailReceived(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::QuestAccepted>([this](const ev::QuestAccepted&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playQuestActivate(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::QuestCompleted>([this](const ev::QuestCompleted&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playQuestComplete(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::TrainerSpellLearned>([this](const ev::TrainerSpellLearned&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playQuestActivate(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::DuelRequested>([this](const ev::DuelRequested&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playTargetSelect(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::GroupInviteReceived>([this](const ev::GroupInviteReceived&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playQuestActivate(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::MinimapPinged>([this](const ev::MinimapPinged&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playMinimapPing(); });
    }));
    subscriptions_.push_back(bus.subscribe<ev::FishingBite>([this](const ev::FishingBite&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playFishingBite(); });
    }));

    // A melee swing: the weapon, the player's grunt and wound, and an
    // attacking creature's battle cry where it stands.
    subscriptions_.push_back(bus.subscribe<ev::MeleeSwingResolved>([this](const ev::MeleeSwingResolved& e) {
        if (!audioCoordinator_) return;
        const bool landed = !e.missed && !e.avoided;
        if (!e.attackerIsPlayer) {
            auto* voice = audioCoordinator_->getNpcVoiceManager();
            auto entity = gameHandler_.getEntityManager().getEntity(e.attackerGuid);
            if (voice && entity && entity->getType() == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                const glm::vec3 canonical(unit->getLatestX(), unit->getLatestY(), unit->getLatestZ());
                voice->playCombatAttack(e.attackerGuid, unit->getDisplayId(),
                                        core::coords::canonicalToRender(canonical));
            }
        }
        if (auto* csm = audioCoordinator_->getCombatSoundManager()) {
            const auto weaponSize = audio::CombatSoundManager::WeaponSize::MEDIUM;
            if (e.missed) {
                csm->playWeaponMiss(false);
            } else if (e.avoided) {
                // Dodged or parried: the whoosh, and nothing hit.
                csm->playWeaponSwing(weaponSize, false);
            } else {
                csm->playWeaponSwing(weaponSize, e.critical);
                csm->playImpact(weaponSize, audio::CombatSoundManager::ImpactType::FLESH, e.critical);
            }
        }
        if (auto* vocal = audioCoordinator_->getActivitySoundManager()) {
            if (e.attackerIsPlayer && landed) vocal->playAttackGrunt();
            if (e.victimIsPlayer && landed) vocal->playWound(e.critical);
        }
    }));
}

std::optional<std::string> AudioCallbackHandler::resolveSoundEntryPath(uint32_t soundId) const {
    auto dbc = assetManager_.loadDBC("SoundEntries.dbc");
    if (!dbc || !dbc->isLoaded()) return std::nullopt;

    int32_t idx = dbc->findRecordById(soundId);
    if (idx < 0) return std::nullopt;

    // SoundEntries.dbc (WotLK): field 2 = Name (label), fields 3-12 = File[0..9], field 23 = DirectoryBase
    const uint32_t row = static_cast<uint32_t>(idx);
    std::string dir = dbc->getString(row, 23);
    for (uint32_t f = 3; f <= 12; ++f) {
        std::string name = dbc->getString(row, f);
        if (name.empty()) continue;
        return dir.empty() ? name : dir + "\\" + name;
    }
    return std::nullopt;
}

void AudioCallbackHandler::setupCallbacks() {
    // The player's own level-up: the sound, the ding banner and the 3D effect.
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::PlayerLevelUp>(
        [this](const game::events::PlayerLevelUp& e) {
        if (audioCoordinator_) {
            if (auto* sfx = audioCoordinator_->getUiSoundManager()) sfx->playLevelUp();
        }
        if (uiManager_) {
            uiManager_->getGameScreen().toastManager().triggerDing(e.newLevel);
        }
        if (renderer_) {
            if (auto* ac = playerView_->getAnimationController()) ac->triggerLevelUpEffect(renderer_->getCharacterPosition());
        }
    }));

    // An achievement: its alert and the toast banner.
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::AchievementEarned>(
        [this](const game::events::AchievementEarned&) {
        playUi([](audio::UiSoundManager& sfx) { sfx.playAchievementAlert(); });
        if (uiManager_) {
            uiManager_->getGameScreen().toastManager().playAchievementEarned();
        }
    }));

    subscribeGameSounds();

    // Server-triggered music callback (SMSG_PLAY_MUSIC)
    // Resolves soundId → SoundEntries.dbc → MPQ path → MusicManager.
    gameHandler_.setPlayMusicCallback([this](uint32_t soundId) {
        if (!renderer_) return;
        auto* music = audioCoordinator_ ? audioCoordinator_->getMusicManager() : nullptr;
        if (!music) return;

        auto path = resolveSoundEntryPath(soundId);
        if (path) {
            music->playMusic(*path, /*loop=*/false);
        }
    });

    // SMSG_PLAY_SOUND: look up SoundEntries.dbc and play 2-D sound effect
    gameHandler_.setPlaySoundCallback([this](uint32_t soundId) {
        auto path = resolveSoundEntryPath(soundId);
        if (path) {
            audio::AudioEngine::instance().playSound2D(*path);
        }
    });

    // SMSG_PLAY_OBJECT_SOUND / SMSG_PLAY_SPELL_IMPACT: play as 3D positional sound at source entity
    gameHandler_.setPlayPositionalSoundCallback([this](uint32_t soundId, uint64_t sourceGuid) {
        auto path = resolveSoundEntryPath(soundId);
        if (!path) return;

        // Play as 3D sound if source entity position is available.
        // Entity stores canonical coords; listener uses render coords (camera).
        auto entity = gameHandler_.getEntityManager().getEntity(sourceGuid);
        if (entity) {
            glm::vec3 canonical{entity->getLatestX(), entity->getLatestY(), entity->getLatestZ()};
            glm::vec3 pos = core::coords::canonicalToRender(canonical);
            audio::AudioEngine::instance().playSound3D(*path, pos);
        } else {
            audio::AudioEngine::instance().playSound2D(*path);
        }
    });

    // Other player level-up callback - trigger 3D effect + chat notification
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::OtherPlayerLevelUp>(
        [this](const game::events::OtherPlayerLevelUp& e) {
        const uint64_t guid = e.guid;
        const uint32_t newLevel = e.newLevel;
        if (!renderer_) return;

        // Trigger 3D effect at the other player's position
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (entity) {
            glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
            glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            if (auto* ac = playerView_->getAnimationController()) ac->triggerLevelUpEffect(renderPos);
        }

        // Show chat message if in group
        if (gameHandler_.isInGroup()) {
            std::string name = gameHandler_.getCachedPlayerName(guid);
            if (name.empty()) name = "A party member";
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = name + " has reached level " + std::to_string(newLevel) + "!";
            gameHandler_.addLocalChatMessage(msg);
        }
    }));

    // Open dungeon finder callback - server sends SMSG_OPEN_LFG_DUNGEON_FINDER
    gameHandler_.setOpenLfgCallback([this]() {
        if (uiManager_) uiManager_->getGameScreen().openDungeonFinder(gameHandler_);
    });
}

}} // namespace wowee::core
