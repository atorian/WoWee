#pragma once

/// A stack of MPQ archives read in place, as an ArchiveLayer.
///
/// The archives are given in load order and a later one's copy of a path
/// replaces an earlier one's, which is how the original client stacks patch
/// archives and how ezLib stacks a realm's: its hotfix archives carry
/// corrected copies of models the larger archives before them already hold.
///
/// Which archive answers for a path is decided once, when the stack is opened,
/// from each archive's (listfile): a lookup is then a hash probe rather than a
/// question put to two dozen archives in turn. A file an archive holds but
/// does not list cannot be found, the same as for the extractor.
///
/// Built only where StormLib is linked - see WOWEE_HAVE_STORMLIB.

#include "pipeline/archive_layer.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline {

class MpqArchiveLayer final : public ArchiveLayer {
public:
    /// Opens `archives`, lowest priority first. Nothing on failure, with the
    /// reason in `error`: a stack missing one of its archives would serve the
    /// files beneath it as though they were current.
    [[nodiscard]] static std::shared_ptr<MpqArchiveLayer> open(
        const std::vector<std::filesystem::path>& archives, std::string& error);

    ~MpqArchiveLayer() override;
    MpqArchiveLayer(const MpqArchiveLayer&) = delete;
    MpqArchiveLayer& operator=(const MpqArchiveLayer&) = delete;

    [[nodiscard]] bool contains(const std::string& normalizedPath) const override;
    [[nodiscard]] std::vector<uint8_t> read(const std::string& normalizedPath) const override;

    [[nodiscard]] size_t fileCount() const { return owner_.size(); }
    [[nodiscard]] size_t archiveCount() const { return archives_.size(); }
    /// Files each archive answers for once the ones above it have taken
    /// theirs, in load order. For the log.
    [[nodiscard]] std::vector<size_t> filesPerArchive() const;

private:
    MpqArchiveLayer() = default;

    struct Archive {
        void* handle = nullptr;
        std::filesystem::path path;
    };
    std::vector<Archive> archives_;
    /// Normalized path -> index into archives_ of the copy that wins.
    std::unordered_map<std::string, uint32_t> owner_;
};

} // namespace pipeline
} // namespace wowee
