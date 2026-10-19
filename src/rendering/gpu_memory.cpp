#include "rendering/gpu_memory.hpp"

#include <nlohmann/json.hpp>
#include <vulkan/vulkan.h>

#include <charconv>
#include <string>

namespace wowee {
namespace rendering {

const char* gpuMemoryUseName(GpuMemoryUse use) {
    switch (use) {
    case GpuMemoryUse::RenderTargets: return "render targets";
    case GpuMemoryUse::Textures: return "textures";
    case GpuMemoryUse::Geometry: return "geometry";
    case GpuMemoryUse::RayTracing: return "ray tracing";
    case GpuMemoryUse::OtherBuffers: return "other buffers";
    case GpuMemoryUse::Count_: break;
    }
    return "?";
}

namespace {

constexpr uint64_t kImageWrittenUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
constexpr uint64_t kGeometryUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
constexpr uint64_t kRayTracingUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                      VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;

GpuMemoryUse useOf(std::string_view type, uint64_t usage) {
    if (type.rfind("IMAGE", 0) == 0) return gpuMemoryUseOfImage(usage);
    if (type == "BUFFER") return gpuMemoryUseOfBuffer(usage);
    return GpuMemoryUse::OtherBuffers;
}

} // namespace

GpuMemoryUse gpuMemoryUseOfImage(uint64_t usage) {
    return (usage & kImageWrittenUsage) != 0 ? GpuMemoryUse::RenderTargets : GpuMemoryUse::Textures;
}

GpuMemoryUse gpuMemoryUseOfBuffer(uint64_t usage) {
    // Geometry first: a vertex buffer the ray tracer also builds from is
    // still the model's geometry, and is counted once.
    if ((usage & kGeometryUsage) != 0) return GpuMemoryUse::Geometry;
    if ((usage & kRayTracingUsage) != 0) return GpuMemoryUse::RayTracing;
    return GpuMemoryUse::OtherBuffers;
}

namespace {

/// "Type 7" -> 7: how the detailed map names the memory type a pool or a
/// block list belongs to.
std::optional<uint32_t> memoryTypeKey(std::string_view key) {
    constexpr std::string_view prefix = "Type ";
    if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix) return std::nullopt;
    uint32_t index = 0;
    const char* first = key.data() + prefix.size();
    const char* last = key.data() + key.size();
    const auto [end, error] = std::from_chars(first, last, index);
    if (error != std::errc{} || end != last) return std::nullopt;
    return index;
}

/// Walks the map without building it: the map of a city's worth of
/// allocations runs to megabytes, and only four fields of each object matter.
class AllocationSax : public nlohmann::json_sax<nlohmann::json> {
public:
    AllocationSax(const std::vector<bool>& deviceLocal, GpuMemoryByUse& out)
        : deviceLocal_(deviceLocal), out_(out) {}

    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t value) override {
        return number(value < 0 ? 0 : static_cast<uint64_t>(value));
    }
    bool number_unsigned(number_unsigned_t value) override { return number(value); }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }
    bool string(string_t& value) override {
        if (!frames_.empty() && frames_.back().isObject && key_ == "Type") {
            frames_.back().type = value;
        }
        return true;
    }
    bool key(string_t& value) override {
        key_ = value;
        return true;
    }
    bool start_object(std::size_t) override {
        push(true);
        return true;
    }
    bool end_object() override {
        const Frame& frame = frames_.back();
        if (frame.type && *frame.type != "FREE" && frame.size) {
            const auto use = static_cast<size_t>(useOf(*frame.type, frame.usage));
            const bool local = frame.memoryType && *frame.memoryType < deviceLocal_.size() &&
                               deviceLocal_[*frame.memoryType];
            (local ? out_.deviceLocalBytes : out_.hostBytes)[use] += *frame.size;
            ++out_.allocations;
        }
        frames_.pop_back();
        key_.clear();
        return true;
    }
    bool start_array(std::size_t) override {
        push(false);
        return true;
    }
    bool end_array() override {
        frames_.pop_back();
        key_.clear();
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

private:
    struct Frame {
        bool isObject = false;
        std::optional<uint32_t> memoryType;
        std::optional<std::string> type;
        std::optional<uint64_t> size;
        uint64_t usage = 0;
    };

    void push(bool isObject) {
        Frame frame;
        frame.isObject = isObject;
        // A value under "Type N" and everything inside it is of memory type N.
        if (auto index = memoryTypeKey(key_)) frame.memoryType = index;
        else if (!frames_.empty()) frame.memoryType = frames_.back().memoryType;
        frames_.push_back(std::move(frame));
        key_.clear();
    }

    bool number(uint64_t value) {
        if (frames_.empty() || !frames_.back().isObject) return true;
        if (key_ == "Size") frames_.back().size = value;
        else if (key_ == "Usage") frames_.back().usage = value;
        return true;
    }

    const std::vector<bool>& deviceLocal_;
    GpuMemoryByUse& out_;
    std::vector<Frame> frames_;
    std::string key_;
};

} // namespace

std::optional<GpuMemoryByUse> classifyGpuAllocations(std::string_view json,
                                                     const std::vector<bool>& memoryTypeIsDeviceLocal) {
    GpuMemoryByUse out;
    AllocationSax sax(memoryTypeIsDeviceLocal, out);
    if (!nlohmann::json::sax_parse(json.begin(), json.end(), &sax)) return std::nullopt;
    return out;
}

} // namespace rendering
} // namespace wowee
