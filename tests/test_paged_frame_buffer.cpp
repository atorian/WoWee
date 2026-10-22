#include <catch_amalgamated.hpp>
#include "rendering/paged_frame_buffer.hpp"

using Table=wowee::rendering::PagedFrameBuffer<int,16>;
namespace {
struct Slot {
    Table::Uploaded state;
    std::vector<int> device;
    size_t accept(const Table::Snapshot& frame) {
        device.resize(frame.size);
        return Table::upload(frame,state,[&](size_t offset,std::span<const int> values) {
            std::copy(values.begin(),values.end(),device.begin()+offset);
        });
    }
};
void set(Table& table,size_t offset,int value) { table.write(offset,std::span(&value,1)); }
}

TEST_CASE("Frame buffer keeps queued versions immutable and updates each slot independently", "[frame-buffer]") {
    Table table;
    Table::Snapshot first,second,third;
    set(table,3,10); set(table,35,20); table.publish(first);
    Slot a,b;
    a.accept(first);
    set(table,3,11); table.publish(second);
    set(table,35,21); table.publish(third);
    // A consumes every publication; B was delayed and missed second.
    a.accept(second); a.accept(third); b.accept(third);
    CHECK(a.device==b.device);
    CHECK(b.device[3]==11); CHECK(b.device[35]==21);
    CHECK(a.accept(third)==0);
    Slot delayed; delayed.accept(first);
    CHECK(delayed.device[3]==10); CHECK(delayed.device[35]==20);
    // Re-recording an older snapshot must upload its old contents, not reuse
    // a newer GPU page just because this is the same frame slot.
    a.accept(first); CHECK(a.device==delayed.device);
    a.accept(third); CHECK(a.device==b.device);
}

TEST_CASE("Only the modified page is uploaded after a local edit", "[frame-buffer]") {
    Table table; Table::Snapshot frame;
    std::vector<int> initial(4096,7);
    table.write(0,initial); table.publish(frame);
    Slot slot; slot.accept(frame);
    set(table,2000,9); table.publish(frame);
    const size_t uploaded=slot.accept(frame);
    CHECK(uploaded<initial.size()/100);
    initial[2000]=9;
    CHECK(slot.device==initial);
}

TEST_CASE("A new world replaces reused offsets even when local revision numbers match", "[frame-buffer]") {
    Table worldA,worldB; Table::Snapshot a,b;
    set(worldA,4,55); worldA.publish(a);
    set(worldB,4,99); worldB.publish(b);
    Slot slot; slot.accept(a); slot.accept(b);
    CHECK(slot.device[4]==99);
    slot.accept(a); CHECK(slot.device[4]==55);
}

TEST_CASE("Moving a scene transfers page identity while old snapshots remain recordable", "[frame-buffer]") {
    Table original; Table::Snapshot before,after;
    set(original,1,17); original.publish(before);
    Table session(std::move(original));
    Slot slot; slot.accept(before);
    session.publish(after); CHECK(slot.accept(after)==0);
    set(session,1,23); session.publish(after); slot.accept(after);
    CHECK(slot.device[1]==23);
    original=Table{};
    set(original,1,42); original.publish(after); slot.accept(after);
    CHECK(slot.device[1]==42);
    slot.accept(before); CHECK(slot.device[1]==17);
}
