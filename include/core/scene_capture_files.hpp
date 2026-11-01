#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

namespace wowee::core {

/// What a scene capture records of a file it was taken against (the
/// expansion's manifest, each realm patch archive): its path, its size and
/// its last write in UTC nanoseconds since 1970 ("written"). Daylight saving
/// does not move it. Throws std::runtime_error naming the file it cannot read.
nlohmann::json signCapturedFile(const std::filesystem::path& path);

/// Refuses, with a std::runtime_error naming the file, a file that is not as
/// the capture signed it - another size or another write - and a signature in
/// the local-time form captures had before 2026-11-01, which moves with
/// daylight saving: tools/resign_scene_capture.py converts those.
void verifyCapturedFile(const nlohmann::json& signature);

/// Refuses a replay whose data root is not the directory the capture was
/// taken with. The two may spell it differently (separators, case).
void verifyCapturedDataPath(const std::string& current, const std::string& captured);

}
