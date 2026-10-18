#include "rendering/pipeline_cache_file.hpp"

#include "core/logger.hpp"

#include <cstring>
#include <fstream>
#include <system_error>

namespace wowee {
namespace rendering {

PipelineCacheIdentity PipelineCacheIdentity::of(const VkPhysicalDeviceProperties& props) {
    PipelineCacheIdentity id;
    id.vendorId = props.vendorID;
    id.deviceId = props.deviceID;
    std::memcpy(id.cacheUuid.data(), props.pipelineCacheUUID, VK_UUID_SIZE);
    return id;
}

std::vector<char> loadPipelineCacheFile(const std::filesystem::path& path,
                                        const PipelineCacheIdentity& device) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        LOG_INFO("Pipeline cache: none saved at ", path.string(), " - pipelines are built from scratch");
        return {};
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file ? static_cast<std::streamoff>(file.tellg()) : std::streamoff{-1};
    std::vector<char> data;
    if (size > 0) {
        data.resize(static_cast<size_t>(size));
        file.seekg(0);
        file.read(data.data(), size);
    }
    if (!file || data.empty()) {
        LOG_WARNING("Pipeline cache: ", path.string(), " could not be read - starting with an empty one");
        return {};
    }

    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() < sizeof(header)) {
        LOG_WARNING("Pipeline cache: ", path.string(), " is ", data.size(),
                    " bytes, shorter than its header - starting with an empty one");
        return {};
    }
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerSize < sizeof(header) ||
        header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
        LOG_WARNING("Pipeline cache: ", path.string(), " has an unknown header (size ",
                    header.headerSize, ", version ", static_cast<int>(header.headerVersion),
                    ") - starting with an empty one");
        return {};
    }
    if (header.vendorID != device.vendorId || header.deviceID != device.deviceId ||
        std::memcmp(header.pipelineCacheUUID, device.cacheUuid.data(), VK_UUID_SIZE) != 0) {
        // The usual reason is a driver update; every pipeline is built anew
        // once and the cache saved again.
        LOG_WARNING("Pipeline cache: ", path.string(), " was written for another device or driver"
                    " (vendor 0x", std::hex, header.vendorID, " device 0x", header.deviceID, std::dec,
                    ") - starting with an empty one");
        return {};
    }
    return data;
}

bool savePipelineCacheFile(const std::filesystem::path& path, const std::vector<char>& data) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LOG_WARNING("Pipeline cache: could not create ", path.parent_path().string(), ": ",
                    ec.message(), " - not saved");
        return false;
    }
    std::filesystem::path partial = path;
    partial += ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) {
            LOG_WARNING("Pipeline cache: could not write ", partial.string(), " - not saved");
            file.close();
            std::filesystem::remove(partial, ec);
            return false;
        }
    }
    std::filesystem::rename(partial, path, ec);
    if (ec) {
        LOG_WARNING("Pipeline cache: could not replace ", path.string(), ": ", ec.message(),
                    " - not saved");
        std::filesystem::remove(partial, ec);
        return false;
    }
    return true;
}

} // namespace rendering
} // namespace wowee
