// The sound settings, set the way the settings window, FrameXML and addons set
// them: through setSettingValue.
//
// Two faults lived here. The master, music and ambience volumes were bridged
// to the interface's CVars by hand and nothing wrote the setting back into the
// CVar store - which is replayed over the settings file at every start, so a
// music volume moved in this client's window was back at the interface's old
// value at the next login. And the enable switches were laid over the volumes
// by a second piece of code that only the interface reached, so a switch
// turned in the window did nothing, and one turned in the interface was undone
// by the next slider, the minimap's mute button or the next start.
#include <catch_amalgamated.hpp>

#include "addons/lua_api_registrations.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/music_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "core/cvar_store.hpp"
#include "ui/settings_panel.hpp"

using wowee::core::CVarStore;
using wowee::ui::SettingsPanel;

TEST_CASE("a volume set in the client is what the next start replays", "[settings][audio]") {
    CVarStore store;
    store.load();
    wowee::addons::useCVarStore(store);
    // What the interface wrote once - its options panel on apply, or the
    // volume keys - and what the start-up replay hands back to the setting.
    store.set("Sound_MasterVolume", "1");
    store.set("Sound_MusicVolume", "0.3");
    store.set("Sound_AmbienceVolume", "1");

    SettingsPanel panel;
    REQUIRE(panel.setSettingValue("mastervolume", "60"));
    REQUIRE(panel.setSettingValue("musicvolume", "45"));
    REQUIRE(panel.setSettingValue("ambientvolume", "20"));

    CHECK(store.get("Sound_MasterVolume", "") == "0.6");
    CHECK(store.get("Sound_MusicVolume", "") == "0.45");
    CHECK(store.get("Sound_AmbienceVolume", "") == "0.2");
}

// Explicit opt-in, as the other device tests are: the audio managers exist only
// once the engine has opened a playback device.
TEST_CASE("a sound switch keeps its channels quiet whatever moves next", "[.audio-device]") {
    CVarStore store;
    store.load();
    wowee::addons::useCVarStore(store);

    wowee::audio::AudioCoordinator audio;
    REQUIRE(audio.initialize());
    wowee::ui::UIServices services;
    services.audioCoordinator = &audio;
    SettingsPanel panel;
    panel.setServices(services);

    auto& music = *audio.getMusicManager();
    auto& ambient = *audio.getAmbientSoundManager();
    auto& ui = *audio.getUiSoundManager();
    auto& npc = *audio.getNpcVoiceManager();
    auto& voice = *audio.getPlayerVoiceManager();

    REQUIRE(panel.setSettingValue("musicvolume", "40"));
    REQUIRE(panel.setSettingValue("ambientvolume", "80"));
    REQUIRE(panel.setSettingValue("uivolume", "50"));

    // Each switch silences its channels as it is turned.
    REQUIRE(panel.setSettingValue("enablemusic", "0"));
    REQUIRE(panel.setSettingValue("enableambience", "0"));
    REQUIRE(panel.setSettingValue("enablesoundeffects", "0"));
    REQUIRE(panel.setSettingValue("errorspeech", "0"));
    CHECK(music.getVolume() == 0);
    CHECK(ambient.getVolumeScale() == 0.0f);
    CHECK(ui.getVolumeScale() == 0.0f);
    CHECK_FALSE(voice.isEnabled());

    // And stays so through what applies the sliders again: a slider, the mute
    // going on and off.
    REQUIRE(panel.setSettingValue("mastervolume", "80"));
    REQUIRE(panel.setSettingValue("mutesound", "1"));
    REQUIRE(panel.setSettingValue("mutesound", "0"));
    CHECK(music.getVolume() == 0);
    CHECK(ambient.getVolumeScale() == 0.0f);
    CHECK(ambient.getBellVolumeScale() == 0.0f);
    CHECK(ui.getVolumeScale() == 0.0f);
    CHECK(npc.getVolumeScale() == 0.0f);
    CHECK(voice.getVolumeScale() == 0.0f);
    CHECK_FALSE(voice.isEnabled());

    // Turned back on, each channel is where its slider is.
    REQUIRE(panel.setSettingValue("enablemusic", "1"));
    REQUIRE(panel.setSettingValue("enableambience", "1"));
    REQUIRE(panel.setSettingValue("enablesoundeffects", "1"));
    REQUIRE(panel.setSettingValue("errorspeech", "1"));
    CHECK(music.getVolume() == 40);
    CHECK(ambient.getVolumeScale() == Catch::Approx(0.8f));
    CHECK(ui.getVolumeScale() == Catch::Approx(0.5f));
    CHECK(voice.isEnabled());
}

TEST_CASE("play without gaps reaches the music when it is set", "[.audio-device]") {
    CVarStore store;
    store.load();
    wowee::addons::useCVarStore(store);

    wowee::audio::AudioCoordinator audio;
    REQUIRE(audio.initialize());
    wowee::ui::UIServices services;
    services.audioCoordinator = &audio;
    SettingsPanel panel;
    panel.setServices(services);

    REQUIRE(panel.setSettingValue("loopmusic", "1"));
    CHECK(audio.getMusicManager()->isLooping());
    REQUIRE(panel.setSettingValue("loopmusic", "0"));
    CHECK_FALSE(audio.getMusicManager()->isLooping());
}
