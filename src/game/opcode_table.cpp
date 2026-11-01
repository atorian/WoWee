#include "game/opcode_table.hpp"
#include "game/json_table_scan.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <unordered_set>

namespace wowee {
namespace game {

// Global active opcode table pointer
static const OpcodeTable* g_activeOpcodeTable = nullptr;

void setActiveOpcodeTable(const OpcodeTable* table) { g_activeOpcodeTable = table; }
const OpcodeTable* getActiveOpcodeTable() { return g_activeOpcodeTable; }

// Name ↔ LogicalOpcode mapping table (generated from the enum)
struct OpcodeNameEntry {
    const char* name;
    LogicalOpcode op;
};

// Expansion/core naming aliases -> canonical LogicalOpcode names used by implementation.
struct OpcodeAliasEntry {
    const char* alias;
    const char* canonical;
};

// clang-format off
static const OpcodeAliasEntry kOpcodeAliases[] = {
#include "game/opcode_aliases_generated.inc"
};

static const OpcodeNameEntry kOpcodeNames[] = {
#include "game/opcode_names_generated.inc"
};
// clang-format on


static std::string_view canonicalOpcodeName(std::string_view name) {
    for (auto entry : kOpcodeAliases) {
        if (name == entry.alias) return entry.canonical;
    }
    return name;
}

static std::optional<uint16_t> resolveLogicalOpcodeIndex(std::string_view name) {
    const std::string_view canonical = canonicalOpcodeName(name);
    for (auto entry : kOpcodeNames) {
        if (canonical == entry.name) {
            return static_cast<uint16_t>(entry.op);
        }
    }
    return std::nullopt;
}

/// One opcodes.json: every "NAME": "0x123" pair whose name is a logical
/// opcode. A later pair for the same name or the same wire value replaces
/// the earlier one, so the table stays one-to-one.
static bool loadOpcodeJson(const std::filesystem::path& path,
                           std::unordered_map<uint16_t, uint16_t>& logicalToWire,
                           std::unordered_map<uint16_t, uint16_t>& wireToLogical) {
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_WARNING("OpcodeTable: cannot open ", path.string());
        return false;
    }

    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    forEachJsonKeyValue(json, [&](const std::string& key, const std::string& valStr) {
        uint32_t parsed = 0;
        // A value that is not wholly a number is skipped rather than stored.
        if (!parseTableNumber(valStr, parsed)) return;
        const uint16_t wire = static_cast<uint16_t>(parsed);

        auto logical = resolveLogicalOpcodeIndex(key);
        if (logical) {
            auto oldLogicalIt = logicalToWire.find(*logical);
            if (oldLogicalIt != logicalToWire.end()) {
                const uint16_t oldWire = oldLogicalIt->second;
                auto oldWireIt = wireToLogical.find(oldWire);
                if (oldWireIt != wireToLogical.end() && oldWireIt->second == *logical) {
                    wireToLogical.erase(oldWireIt);
                }
            }
            auto oldWireIt = wireToLogical.find(wire);
            if (oldWireIt != wireToLogical.end() && oldWireIt->second != *logical) {
                logicalToWire.erase(oldWireIt->second);
                wireToLogical.erase(oldWireIt);
            }
            logicalToWire[*logical] = wire;
            wireToLogical[wire] = *logical;
        }
    });
    return true;
}

const char* OpcodeTable::logicalToName(LogicalOpcode op) {
    uint16_t val = static_cast<uint16_t>(op);
    for (auto entry : kOpcodeNames) {
        if (static_cast<uint16_t>(entry.op) == val) return entry.name;
    }
    return "UNKNOWN";
}

bool OpcodeTable::loadFromJson(const std::string& path) {
    // Load into a scratch map, then bake it into the flat vector for fast
    // toWire(). Into temporaries, so a failed reload doesn't wipe the working
    // table.
    std::unordered_map<uint16_t, uint16_t> scratch;
    std::unordered_map<uint16_t, uint16_t> newWireToLogical;
    if (!loadOpcodeJson(std::filesystem::path(path), scratch, newWireToLogical) ||
        scratch.empty()) {
        LOG_WARNING("OpcodeTable: no opcodes loaded from ", path);
        return false;
    }

    // Bake into the flat lookup table. Sized to cover the highest logical id we saw;
    // unmapped slots stay 0xFFFF (the same sentinel toWire used to return on miss).
    uint16_t maxIdx = 0;
    for (const auto& [logical, _wire] : scratch) {
        if (logical > maxIdx) maxIdx = logical;
    }
    std::vector<uint16_t> newLogicalToWire(static_cast<size_t>(maxIdx) + 1, 0xFFFF);
    for (const auto& [logical, wire] : scratch) {
        newLogicalToWire[logical] = wire;
    }

    logicalToWire_ = std::move(newLogicalToWire);
    wireToLogical_ = std::move(newWireToLogical);
    logicalToWireSize_ = scratch.size();

    LOG_INFO("OpcodeTable: loaded ", logicalToWireSize_, " opcodes from ", path);
    return true;
}

uint16_t OpcodeTable::toWire(LogicalOpcode op) const {
    const size_t idx = static_cast<size_t>(op);
    return (idx < logicalToWire_.size()) ? logicalToWire_[idx] : 0xFFFF;
}

std::optional<LogicalOpcode> OpcodeTable::fromWire(uint16_t wireValue) const {
    auto it = wireToLogical_.find(wireValue);
    if (it != wireToLogical_.end()) {
        return static_cast<LogicalOpcode>(it->second);
    }
    return std::nullopt;
}

bool OpcodeTable::hasOpcode(LogicalOpcode op) const {
    return toWire(op) != 0xFFFF;
}

} // namespace game
} // namespace wowee
