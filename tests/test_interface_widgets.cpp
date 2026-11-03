// Widgets as the interface's Lua sees them.
//
// What a chat frame keeps of the lines the interface hands it: chat is a list that grows at one end and falls off the other. AddMessage
// was a name in the method list and nothing else, so every line went nowhere;
// a frame that keeps every line grows without bound over a session. Driven
// through the interface's own Lua, as FrameXML's chat frames drive it.
#include <catch_amalgamated.hpp>

#include "client_harness.hpp"
#include "addons/lua_engine.hpp"

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

TEST_CASE("a message frame's line fades out once its time is up", "[widget][chat][interface]") {
    // UIErrorsFrame and the combat text show a line for a while and then fade
    // it: aged every frame, as each frame's lines are drawn. The ageing walked
    // every widget for the few message frames, so it is now a list of them -
    // and a frame left off that list would show its lines for ever.
    ClientHarness h;
    h.enterWorld();
    h.runInInterface(R"lua(
        Fading = CreateFrame("MessageFrame", "Fading", UIParent)
        Fading:SetTimeVisible(1)
        Fading:SetFadeDuration(0.5)
        Fading:AddMessage("gone soon")
    )lua");
    const auto alpha = [&] {
        float a = -1.0f;
        h.withInterface([&](wowee::addons::LuaEngine& lua) {
            if (const auto* w = lua.widgets().findByName("Fading"); w && !w->messages.empty())
                a = w->messages.front().color[3];
        });
        return a;
    };
    CHECK(alpha() == 1.0f);
    h.advance(0.75f);  // into the fade: half a second left of the line's second
    const float fading = alpha();
    CHECK(fading > 0.0f);
    CHECK(fading < 1.0f);
    h.advance(0.5f);
    CHECK(alpha() == 0.0f);
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
