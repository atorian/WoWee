#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace wowee::game::extension_wire {
constexpr uint32_t magic = 0x31584557; // WEX1, little endian
constexpr uint32_t maxPayload = 16384;
enum Operation : uint32_t { Start = 1, Read = 2, Stop = 3 };
inline void integer(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
inline void string(std::vector<uint8_t>& out, const std::string& value) {
    if (value.size() > maxPayload) throw std::runtime_error("extension string exceeds limit");
    integer(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
class Reader {
    const std::vector<uint8_t>& bytes_;
    size_t pos_ = 0;
public:
    explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    uint32_t integer() {
        if (bytes_.size() - pos_ < 4) throw std::runtime_error("short extension message");
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes_[pos_++]) << (i * 8);
        return value;
    }
    std::string string() {
        const auto count = integer();
        if (count > bytes_.size() - pos_) throw std::runtime_error("short extension string");
        std::string value(bytes_.begin() + pos_, bytes_.begin() + pos_ + count);
        pos_ += count;
        if (value.find('\0') != std::string::npos) throw std::runtime_error("NUL in extension string");
        return value;
    }
    void end() const {
        if (pos_ != bytes_.size()) throw std::runtime_error("trailing extension data");
    }
};
inline std::vector<uint8_t> frame(uint32_t operation, const std::vector<uint8_t>& payload) {
    if (payload.size() > maxPayload) throw std::runtime_error("extension message exceeds limit");
    std::vector<uint8_t> out;
    integer(out, magic); integer(out, operation); integer(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
}
