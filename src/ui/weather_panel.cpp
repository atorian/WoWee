#include "ui/weather_panel.hpp"

#include "game/game_handler.hpp"
#include "game/weather_handler.hpp"
#include "pipeline/weather_kind.hpp"
#include "rendering/lens_drops.hpp"
#include "rendering/renderer.hpp"

#include <imgui.h>
#include <glm/glm.hpp>

#include <cmath>
#include <string>

namespace wowee::ui {

namespace {

using pipeline::WeatherKind;

std::string kindName(WeatherKind kind) {
    return std::string(pipeline::weatherKindName(kind));
}

/// A part of the weather as a labelled bar, 0-1.
void part(const char* label, float amount) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90.0f);
    ImGui::ProgressBar(amount, ImVec2(-1.0f, 0.0f));
}

}  // namespace

void WeatherPanel::draw(rendering::Renderer& renderer, const game::GameHandler* game) {
    if (!open_) return;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60.0f, 120.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Weather", &open_)) {
        ImGui::End();
        return;
    }
    rendering::WeatherControls& controls = renderer.weatherControls();

    // ---- What the realm says, and what is drawn ----
    if (game) {
        const game::WeatherReport& realm = game->weather().current();
        ImGui::Text("Realm: %s %.0f%%  (state %u%s)", kindName(realm.kind).c_str(), realm.intensity * 100.0f,
                    realm.realmState, realm.abrupt ? ", at once" : "");
    } else {
        ImGui::TextDisabled("Realm: no session");
    }
    const rendering::WeatherTarget& target = renderer.weatherTarget();
    ImGui::Text("Drawing: %s %.0f%%%s", kindName(target.kind).c_str(), target.intensity * 100.0f,
                controls.weather ? "  (override)" : "");
    const rendering::WeatherMix& mix = renderer.weatherMix();
    part("rain", mix.rain);
    part("snow", mix.snow);
    part("sand", mix.sand);
    part("mist", mix.mist);
    part("thunder", mix.thunder);
    part("dark", mix.dark);

    // ---- The weather to draw ----
    ImGui::SeparatorText("Weather");
    bool overriding = controls.weather.has_value();
    if (ImGui::Checkbox("Override the realm's weather", &overriding)) {
        if (overriding) {
            // From what is being drawn, so ticking it changes nothing yet.
            controls.weather = rendering::WeatherTarget{.kind = target.kind, .intensity = target.intensity};
        } else {
            controls.weather.reset();
        }
    }
    if (controls.weather) {
        rendering::WeatherTarget& w = *controls.weather;
        if (ImGui::BeginCombo("Kind", kindName(w.kind).c_str())) {
            for (const WeatherKind kind : pipeline::kWeatherKinds) {
                if (ImGui::Selectable(kindName(kind).c_str(), kind == w.kind)) w.kind = kind;
            }
            ImGui::EndCombo();
        }
        ImGui::SliderFloat("Intensity", &w.intensity, 0.0f, 1.0f, "%.2f");
        ImGui::Checkbox("Arrives at once", &w.abrupt);
    }
    ImGui::SliderFloat("Comes over in", &controls.transitionSeconds, 0.0f, 60.0f, "%.0f s");

    // ---- The wind ----
    ImGui::SeparatorText("Wind");
    const rendering::WindState& wind = renderer.windState();
    const float towardDegrees = std::fmod(glm::degrees(std::atan2(wind.direction.y, wind.direction.x)) + 360.0f, 360.0f);
    ImGui::Text("Blowing toward %.0f deg at %.1f yd/s, gusts x%.2f", towardDegrees, wind.speed, wind.gust);
    bool fixedWind = controls.wind.has_value();
    if (ImGui::Checkbox("Override the zone's wind", &fixedWind)) {
        if (fixedWind) {
            controls.wind = rendering::WindOverride{.directionDegrees = towardDegrees, .speed = wind.speed};
        } else {
            controls.wind.reset();
        }
    }
    if (controls.wind) {
        ImGui::SliderFloat("Toward", &controls.wind->directionDegrees, 0.0f, 360.0f, "%.0f deg");
        ImGui::SliderFloat("Speed", &controls.wind->speed, 0.0f, 20.0f, "%.1f yd/s");
    }

    // ---- What is drawn of it ----
    ImGui::SeparatorText("Drawn");
    ImGui::Checkbox("Rain, snow and sand", &controls.precipitation);
    ImGui::SameLine();
    ImGui::Checkbox("Splashes", &controls.splashes);
    ImGui::Checkbox("On the lens", &controls.lens);
    ImGui::SameLine();
    ImGui::Checkbox("Lightning", &controls.lightning);

    const rendering::LensDrops& lens = renderer.lensDrops();
    ImGui::Text("%u drops drawn, camera %.0f%% open to the sky, %.0f%% of the sky in view",
                renderer.precipitationCount(), renderer.skyExposure() * 100.0f, renderer.skyView() * 100.0f);
    ImGui::Text("Lens: %zu drops, frost %.0f%%, dust %.0f%%", lens.drops().size(), lens.frost() * 100.0f,
                lens.dust() * 100.0f);

    if (ImGui::Button("Back to the realm's", ImVec2(-1.0f, 0.0f))) controls = rendering::WeatherControls{};
    ImGui::End();
}

}  // namespace wowee::ui
