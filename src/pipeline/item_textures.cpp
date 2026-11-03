#include "pipeline/item_textures.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <unordered_set>

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/required_table.hpp"

namespace wowee {
namespace pipeline {



std::string resolveItemRegionTexture(AssetManager& assets, int region,
                                     const std::string& texName, bool isFemale) {
    if (texName.empty()) return {};
    const char* dir = itemComponentDir(region);
    if (dir[0] == '\0') return {};

    const std::string base = std::string("Item\\TextureComponents\\") + dir + "\\" + texName;
    std::string gendered = base + (isFemale ? "_F.blp" : "_M.blp");
    if (assets.fileExists(gendered)) return gendered;
    std::string unisex = base + "_U.blp";
    if (assets.fileExists(unisex)) return unisex;
    std::string plain = base + ".blp";
    if (assets.fileExists(plain)) return plain;

    // The table names art that no archive carries. Every caller skips the
    // region, so the body shows through where the armour should be - which
    // looked like a renderer fault (plate legs drawn as bare thighs) and said
    // nothing. Realm DBC diffs do this for whole sets: a row naming
    // "plate_raiddeathknight_k_01_Pant_LU" where only the blood, frost and
    // unholy colours of it were shipped. Said once per name and wearer; the
    // workers that prepare other players' equipment ask too.
    static std::mutex logMutex;
    static std::unordered_set<std::string> reported;
    const std::string key = base + (isFemale ? "|F" : "|M");
    std::lock_guard<std::mutex> lock(logMutex);
    if (reported.insert(key).second) {
        LOG_WARNING("Item texture ", base, " (", isFemale ? "_F" : "_M",
                    ", _U or no suffix) is named by ItemDisplayInfo but no archive has it - ",
                    dir, " is drawn without that item's art");
    }
    return {};
}

ItemDisplayArt readItemDisplayArt(const RequiredTable& itemDisplayInfo, uint32_t recordIndex) {
    const uint32_t modelLeft   = itemDisplayInfo.column("LeftModel");
    const uint32_t modelRight  = itemDisplayInfo.column("RightModel");
    const uint32_t texLeft     = itemDisplayInfo.column("LeftModelTexture");
    const uint32_t texRight    = itemDisplayInfo.column("RightModelTexture");

    ItemDisplayArt art;
    art.modelFile   = itemDisplayInfo->getString(recordIndex, modelLeft);
    art.textureName = itemDisplayInfo->getString(recordIndex, texLeft);
    if (art.modelFile.empty()) {
        art.modelFile   = itemDisplayInfo->getString(recordIndex, modelRight);
        art.textureName = itemDisplayInfo->getString(recordIndex, texRight);
    }
    if (art.modelFile.empty()) return art;

    // The tables name .mdx, the format these models were in before they were
    // converted. Every caller renamed it, and one of them forgot the case where
    // the name has no extension at all.
    const size_t dot = art.modelFile.rfind('.');
    if (dot != std::string::npos) art.modelFile.resize(dot);
    art.modelFile += ".m2";
    return art;
}

std::string findCapeTexture(AssetManager& assets, const RequiredTable& itemDisplayInfo,
                            uint32_t displayId, bool isFemale) {
    if (displayId == 0) return {};
    const int32_t row = itemDisplayInfo->findRecordById(displayId);
    if (row < 0) return {};
    const std::string left = itemDisplayInfo->getString(static_cast<uint32_t>(row),
                                                        itemDisplayInfo.column("LeftModelTexture"));
    const std::string right = itemDisplayInfo->getString(static_cast<uint32_t>(row),
                                                         itemDisplayInfo.column("RightModelTexture"));
    for (const std::string* name : isFemale ? std::array{&right, &left} : std::array{&left, &right}) {
        for (const auto& candidate : capeTextureCandidates(*name, isFemale))
            if (assets.fileExists(candidate)) return candidate;
    }
    return {};
}

void getItemDisplayInfoTextureFields(const RequiredTable& itemDisplayInfo, uint32_t (&out)[8]) {
    const uint32_t width = itemDisplayInfo->getFieldCount();
    uint32_t base = itemDisplayInfo.column("TextureArmUpper", 8);
    if (base == 14 && width >= 25) base = 15;
    else if (base == 15 && width < 25) base = 14;
    for (int i = 0; i < 8; i++) out[i] = base + static_cast<uint32_t>(i);
}

}  // namespace pipeline
}  // namespace wowee
