#pragma once

#include "pipeline/blp_loader.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace wowee::pipeline {

/// Immutable output of loading and alpha analysis. A generation belongs to one
/// AssetManager: derived preparation must include it as well as its own recipe.
struct PreparedTexture {
    BLPImage image;
    uint64_t generation = 0;
};
using PreparedTextureRef = std::shared_ptr<const PreparedTexture>;
using PreparedTextures = std::unordered_map<std::string, PreparedTextureRef>;

} // namespace wowee::pipeline
