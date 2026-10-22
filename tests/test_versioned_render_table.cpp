#include <catch_amalgamated.hpp>
#include "rendering/versioned_render_table.hpp"

using Table = wowee::rendering::VersionedRenderTable<int>;
using Key = wowee::rendering::RenderRecordKey;
namespace {
struct Device {
    Table table;
    std::array<std::vector<int>, wowee::rendering::MAX_FRAMES_IN_FLIGHT> slots;
    uint32_t slot = 0;
    size_t uploads = 0;
    void begin(uint32_t next, size_t count) {
        slot = next; uploads = 0;
        slots[slot].resize(table.begin(slot, count), -1);
    }
    uint32_t draw(Key key, int value) {
        return table.put(key, value, [&](uint32_t row, int data) {
            slots[slot][row] = data; ++uploads;
        });
    }
};
}

TEST_CASE("Instance slots catch up after skipped publications and repeated captures", "[render-table]") {
    Device d;
    d.begin(0, 2); auto a = d.draw({1, 0}, 10); auto b = d.draw({2, 0}, 20); d.table.finish();
    d.begin(1, 2); d.draw({1, 0}, 11); d.draw({2, 0}, 21); d.table.finish();
    CHECK(d.slots[0][a] == 10); CHECK(d.slots[0][b] == 20); // prior submission remains intact
    d.begin(1, 2); d.draw({2, 0}, 22); d.draw({1, 0}, 11); d.table.finish();
    d.begin(0, 2); a = d.draw({1, 0}, 11); b = d.draw({2, 0}, 22); d.table.finish();
    CHECK(d.slots[0][a] == 11); CHECK(d.slots[0][b] == 22);
    d.begin(0, 2); d.draw({2, 0}, 22); d.draw({1, 0}, 11); d.table.finish();
    CHECK(d.uploads == 0);
    // Re-recording a previously captured snapshot restores its own values.
    d.begin(0, 2); a = d.draw({1, 0}, 10); b = d.draw({2, 0}, 20); d.table.finish();
    CHECK(d.slots[0][a] == 10); CHECK(d.slots[0][b] == 20);
}

TEST_CASE("Departed instance rows and replaced GPU buffers cannot retain stale values", "[render-table]") {
    Device d;
    d.begin(0, 600);
    for(uint64_t i=1; i<=600; ++i) d.draw({i, 0}, int(i));
    d.table.finish();
    d.begin(1, 1); d.draw({300, 0}, 300); d.table.finish();
    d.begin(0, 1); auto survivor = d.draw({300, 0}, 300); d.table.finish();
    CHECK(d.slots[0][survivor] == 300);
    d.begin(0, 2); survivor = d.draw({300, 0}, 300); auto replacement = d.draw({700, 0}, 777); d.table.finish();
    CHECK(d.slots[0][survivor] == 300); CHECK(d.slots[0][replacement] == 777);
    std::fill(d.slots[0].begin(), d.slots[0].end(), -1); d.table.invalidate(0);
    d.begin(0, 2); survivor = d.draw({300, 0}, 300); replacement = d.draw({700, 0}, 777); d.table.finish();
    CHECK(d.slots[0][survivor] == 300); CHECK(d.slots[0][replacement] == 777);
    d.begin(0, 0); d.table.finish();
    d.begin(0, 1); replacement = d.draw({700, 0}, 888); d.table.finish();
    CHECK(d.slots[0][replacement] == 888);
}

TEST_CASE("Cancelled instance recording is not acknowledged as an uploaded frame", "[render-table]") {
    Device d;
    d.begin(0, 1); d.draw({1, 0}, 42);
    // Simulate bytes copied into a cancelled, unflushed recording.
    std::fill(d.slots[0].begin(), d.slots[0].end(), -1);
    d.begin(1, 1); d.draw({1, 0}, 42); d.table.finish();
    d.begin(0, 1); const auto row = d.draw({1, 0}, 42); d.table.finish();
    CHECK(d.slots[0][row] == 42);
}
