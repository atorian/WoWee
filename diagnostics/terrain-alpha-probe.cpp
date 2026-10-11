// Runs the real ADT loader/decoder against the independently decoded MCAL
// fixture produced by terrain-alpha-check.py. No Vulkan or Windows required.
#include "pipeline/adt_loader.hpp"
#include "pipeline/adt_alpha.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream adtFile(argv[1], std::ios::binary), referenceFile(argv[2], std::ios::binary);
    if (!adtFile || !referenceFile) return 3;
    std::vector<uint8_t> adtBytes{std::istreambuf_iterator<char>(adtFile), {}},
                         reference{std::istreambuf_iterator<char>(referenceFile), {}};
    auto terrain = wowee::pipeline::ADTLoader::load(adtBytes);
    constexpr size_t recordSize = 8 + wowee::pipeline::ALPHA_MAP_SIZE;
    if (reference.empty() || reference.size() % recordSize) return 4;
    size_t layers = 0;
    for (size_t offset = 0; offset < reference.size(); offset += recordSize) {
        uint32_t chunkIdx, layerIdx;
        std::memcpy(&chunkIdx, reference.data() + offset, 4);
        std::memcpy(&layerIdx, reference.data() + offset + 4, 4);
        if (chunkIdx >= 256) return 5;
        std::vector<uint8_t> alpha;
        if (!wowee::pipeline::decodeLayerAlpha(terrain.chunks[chunkIdx], layerIdx, alpha, 0) ||
            alpha.size() != 4096 || std::memcmp(alpha.data(), reference.data() + offset + 8, 4096)) {
            std::cerr << "Mismatch: chunk=" << chunkIdx << " layer=" << layerIdx << '\n';
            return 6;
        }
        ++layers;
    }
    std::cout << "Real ADT loader/decoder matches " << layers << " MCAL layers ("
              << layers * 4096 << " texels).\n";
}
