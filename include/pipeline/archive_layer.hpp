#pragma once

/// Files served straight out of archives, above the extracted data.
///
/// Everything else the AssetManager reads was extracted once, ahead of time,
/// into loose files under a manifest. A realm's patch archives cannot be: they
/// arrive while logging in, belong to one realm only, and run to gigabytes. So
/// they are read where they are, and this is the shape the AssetManager sees
/// them through - a set of paths it answers for, and their bytes.
///
/// An interface rather than the archive reader itself because reading MPQs
/// needs StormLib, which the pipeline library does not link: the client
/// supplies the implementation when it has one, and tests supply their own.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline {

class ArchiveLayer {
public:
    virtual ~ArchiveLayer() = default;

    /// Whether the layer holds this path. Paths are normalized the way the
    /// manifest's keys are: lower case, backslashes.
    [[nodiscard]] virtual bool contains(const std::string& normalizedPath) const = 0;

    /// The file's bytes, or nothing when the layer does not hold it or it
    /// cannot be read. Safe to call from several threads at once.
    [[nodiscard]] virtual std::vector<uint8_t> read(const std::string& normalizedPath) const = 0;
};

} // namespace pipeline
} // namespace wowee
