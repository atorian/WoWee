#include <catch_amalgamated.hpp>
#include "audio/rain_mix.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "test_support.hpp"
#include <chrono>
#include <cstring>
#include <numeric>
#include <thread>

using namespace wowee::audio;
namespace {
float outdoor(const std::array<float, 6>& v) { return v[0] + v[1] + v[2]; }
float indoor(const std::array<float, 6>& v) { return v[3] + v[4] + v[5]; }
}

TEST_CASE("entering a room crossfades rain into quiet roof patter", "[rain]") {
    RainMix mix;
    const auto street = mix.update(10.0f, 0.8f, false);
    REQUIRE(outdoor(street) > 0.0f);
    CHECK(indoor(street) == 0.0f);
    const auto doorway = mix.update(0.05f, 0.8f, true);
    CHECK(outdoor(doorway) > outdoor(street) * 0.8f);
    CHECK(outdoor(doorway) < outdoor(street));
    CHECK(indoor(doorway) > 0.0f);
    const auto room = mix.update(5.0f, 0.8f, true);
    CHECK(outdoor(room) < 0.001f);
    CHECK(indoor(room) > 0.01f);
    CHECK(indoor(room) < outdoor(street) * 0.3f);
    const auto exit = mix.update(5.0f, 0.8f, false);
    CHECK(outdoor(exit) > outdoor(street) * 0.99f);
    CHECK(indoor(exit) < 0.001f);
}

TEST_CASE("rain changes intensity and ends smoothly under a roof", "[rain]") {
    RainMix mix;
    auto light = mix.update(10, 0.2f, true);
    CHECK(light[3] > 0);
    CHECK(light[5] == 0);
    auto transition = mix.update(0.1f, 1, true);
    CHECK(transition[3] > 0);
    CHECK(transition[5] > 0);
    auto heavy = mix.update(5, 1, true);
    CHECK(indoor(heavy) > indoor(light));
    CHECK(heavy[3] < 0.001f);
    auto ending = mix.update(0.1f, 0, true);
    CHECK(indoor(ending) > 0);
    CHECK(indoor(ending) < indoor(heavy));
    CHECK(indoor(mix.update(10, 0, true)) < 0.0001f);
}

TEST_CASE("rain envelope does not depend on frame rate or accumulate at doorways", "[rain]") {
    RainMix fast, slow;
    for (int i = 0; i < 240; ++i) fast.update(1.0f / 120, 0.5f, true);
    for (int i = 0; i < 40; ++i) slow.update(1.0f / 20, 0.5f, true);
    auto a = fast.update(0, 0, false), b = slow.update(0, 0, false);
    for (size_t i = 0; i < a.size(); ++i) CHECK(std::abs(a[i] - b[i]) < 0.00001f);
    for (int i = 0; i < 1000; ++i) {
        const auto gains = fast.update(0.016f, 1, i % 2 == 0);
        CHECK(outdoor(gains) + indoor(gains) < 0.401f);
    }
}

namespace {
// A short, quiet high-frequency clip makes actual looping and low-pass
// attenuation observable at the engine's speaker-output capture API.
std::vector<uint8_t> tone() {
    constexpr uint32_t rate = 48000, frames = 1440;
    std::vector<uint8_t> wav(44 + frames * 2);
    auto u32 = [&](int at, uint32_t v) { std::memcpy(wav.data() + at, &v, 4); };
    auto u16 = [&](int at, uint16_t v) { std::memcpy(wav.data() + at, &v, 2); };
    std::memcpy(wav.data(), "RIFF", 4); u32(4, uint32_t(wav.size() - 8));
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8); u32(16, 16);
    u16(20, 1); u16(22, 1); u32(24, rate); u32(28, rate * 2);
    u16(32, 2); u16(34, 16); std::memcpy(wav.data() + 36, "data", 4); u32(40, frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t value = int16_t(3000 * std::sin(2.0 * 3.141592653589793 * 6000 * i / rate));
        std::memcpy(wav.data() + 44 + i * 2, &value, 2);
    }
    return wav;
}

double outputRms(AudioEngine& engine) {
    // Let the 40ms gain ramp and device queue settle, then sample well past
    // the original clip's 30ms duration. A one-shot cannot pass this check.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    REQUIRE(engine.beginOutputCapture());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::vector<float> pcm(size_t(engine.capturedOutputAvailable()) * engine.getOutputChannels());
    const auto count = engine.readCapturedOutput(pcm.data(), uint32_t(pcm.size() / engine.getOutputChannels()));
    engine.endOutputCapture();
    REQUIRE(count > 0);
    double power = 0;
    for (size_t i = 0; i < size_t(count) * engine.getOutputChannels(); ++i) power += double(pcm[i]) * pcm[i];
    return std::sqrt(power / (count * engine.getOutputChannels()));
}

struct DeviceSession {
    AudioEngine& engine = AudioEngine::instance();
    DeviceSession() { REQUIRE(engine.initialize()); engine.setMasterVolume(0.1f); }
    ~DeviceSession() { engine.shutdown(); }
};
}

// Explicit opt-in: requires a real playback device, never silently skips in CI.
TEST_CASE("loop playback survives the clip end and mute, filters and releases its voice", "[.audio-device]") {
    DeviceSession session;
    auto& engine = session.engine;
    engine.setMasterVolume(0);
    auto voice = engine.playSound2DLoop(tone(), 0);
    REQUIRE(voice != 0);
    engine.setSoundVolume(voice, 0.5f);
    engine.setMasterVolume(0.1f);
    const double direct = outputRms(engine);
    REQUIRE(direct > 0.0001);
    engine.stopSound(voice);
    CHECK(outputRms(engine) < 0.000001);
    voice = engine.playSound2DLoop(tone(), 0, 1500);
    REQUIRE(voice != 0);
    engine.setSoundVolume(voice, 0.5f);
    const double filtered = outputRms(engine);
    CHECK(filtered > 0.000001);
    CHECK(filtered < direct * 0.15);
    engine.setSoundVolume(voice, 0);
    CHECK(outputRms(engine) < 0.000001);
    engine.stopSound(voice);
    engine.update(0);
}

TEST_CASE("weather loads authored rain loops and keeps muffled rain playing indoors", "[.audio-device]") {
    const auto root = wowee::test::makeDataRoot("wowee_roof_rain", {
        {"sound/ambience/weather/rainlightloop.wav", tone()},
        {"sound/ambience/weather/rainmediumloop.wav", tone()},
        {"sound/ambience/weather/rainheavyloop.wav", tone()},
    });
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    DeviceSession session;
    AmbientSoundManager rain;
    REQUIRE(rain.initialize(&assets));
    rain.setWeather(AmbientSoundManager::WeatherType::RAIN_HEAVY, 1);
    rain.update(10, {}, false);
    const double street = outputRms(session.engine);
    REQUIRE(street > 0.0001);
    rain.update(10, {}, true);
    const double room = outputRms(session.engine);
    CHECK(room > 0.000001);
    CHECK(room < street * 0.1);
    rain.setWeather(AmbientSoundManager::WeatherType::NONE, 0);
    rain.update(10, {}, true);
    CHECK(outputRms(session.engine) < 0.000001);
    rain.setWeather(AmbientSoundManager::WeatherType::RAIN_HEAVY, 1);
    rain.update(10, {}, true);
    CHECK(outputRms(session.engine) > 0.000001);
    rain.resetWeather();
    CHECK(outputRms(session.engine) < 0.000001);
    // The same manager is reused for the next character.
    rain.setWeather(AmbientSoundManager::WeatherType::RAIN_HEAVY, 1);
    rain.update(10, {}, true);
    CHECK(outputRms(session.engine) > 0.000001);
    rain.shutdown();
    CHECK(outputRms(session.engine) < 0.000001);
}
