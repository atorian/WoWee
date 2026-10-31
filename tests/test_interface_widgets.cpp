// Widgets as the interface's Lua sees them.
//
// What a chat frame keeps of the lines the interface hands it: chat is a list that grows at one end and falls off the other. AddMessage
// was a name in the method list and nothing else, so every line went nowhere;
// a frame that keeps every line grows without bound over a session. Driven
// through the interface's own Lua, as FrameXML's chat frames drive it.
#include <catch_amalgamated.hpp>

#include "client_harness.hpp"

using wowee::test::ClientHarness;

TEST_CASE("a message frame keeps its last lines and drops the oldest", "[widget][chat][interface]") {
    ClientHarness h;
    h.enterWorld();
    h.runInInterface(R"lua(
        Chat = CreateFrame("ScrollingMessageFrame", "Chat", UIParent)
        Chat:SetMaxLines(3)
        for _, line in ipairs({"one", "two", "three", "four"}) do Chat:AddMessage(line) end
    )lua");
    CHECK(h.interfaceAnswers("Chat:GetNumMessages() == 3"));
    CHECK(h.interfaceAnswers("Chat:GetMessageInfo(1) == 'two'"));
    CHECK(h.interfaceAnswers("Chat:GetMessageInfo(3) == 'four'"));
}

TEST_CASE("an error frame that inserts at the top drops its bottom line", "[widget][chat][interface]") {
    // UIErrorsFrame's insertMode="TOP": the newest line goes above the others,
    // so the one that falls off is the oldest, at the bottom.
    ClientHarness h;
    h.enterWorld();
    h.runInInterface(R"lua(
        Errors = CreateFrame("MessageFrame", "Errors", UIParent)
        Errors:SetInsertMode("TOP")
        Errors:SetMaxLines(2)
        for _, line in ipairs({"one", "two", "three"}) do Errors:AddMessage(line) end
    )lua");
    CHECK(h.interfaceAnswers("Errors:GetNumMessages() == 2"));
    CHECK(h.interfaceAnswers("Errors:GetMessageInfo(1) == 'three'"));
    CHECK(h.interfaceAnswers("Errors:GetMessageInfo(2) == 'two'"));
}

TEST_CASE("a widget answers the type it was made as", "[widget][interface]") {
    // FrameXML branches on this: whether to treat something as a region it can
    // position, whether a frame is a Button worth clicking. Answering "Frame"
    // for everything makes each of those branches take the wrong side, and
    // silently, because the answer is a plausible string rather than nothing.
    ClientHarness h;
    h.enterWorld();
    h.runInInterface(R"lua(
        TypedButton = CreateFrame("Button", "TypedButton", UIParent)
        TypedTexture = TypedButton:CreateTexture()
        TypedFrame = CreateFrame("Frame", "TypedFrame", UIParent)
    )lua");
    CHECK(h.interfaceAnswers("TypedButton:GetObjectType() == 'Button'"));
    CHECK(h.interfaceAnswers("TypedTexture:GetObjectType() == 'Texture'"));
    CHECK(h.interfaceAnswers("TypedFrame:GetObjectType() == 'Frame'"));
    CHECK(h.interfaceAnswers("TypedButton:IsObjectType('Frame')"));
}
