// What the interface is told about its frames once they are laid out: OnShow
// when one appears, OnHide when it goes, OnSizeChanged when its rect changes.
//
// None of these is announced by the code that makes the change - a frame is
// hidden by hiding something three levels above it, resized by its parent's
// anchors - so the client notices them after layout, once a frame. Driven here
// the way the client drives it: Lua builds and changes frames in the
// character's interface, the harness lays the interface out
// (ClientHarness::layOutInterface), and the handlers' own record says what
// they were told.
#include <catch_amalgamated.hpp>

#include "client_harness.hpp"

using wowee::test::ClientHarness;

namespace {

/// A parent anchored on screen and a child and grandchild filling it, each
/// counting what it was told. Laid out once, and the counts cleared, so a test
/// starts from frames that are on screen and have been told so.
void buildNest(ClientHarness& h) {
    h.runInInterface(R"lua(
        TOLD = {}
        local function count(name, what)
            return function(self, w, hgt)
                TOLD[name .. what] = (TOLD[name .. what] or 0) + 1
                if what == "Size" then TOLD[name .. "W"] = w; TOLD[name .. "H"] = hgt end
            end
        end
        local function watched(name, parent)
            local f = CreateFrame("Frame", name, parent)
            f:SetScript("OnShow", count(name, "Show"))
            f:SetScript("OnHide", count(name, "Hide"))
            f:SetScript("OnSizeChanged", count(name, "Size"))
            return f
        end
        NestParent = watched("NestParent", UIParent)
        NestParent:SetPoint("CENTER")
        NestParent:SetSize(100, 80)
        NestChild = watched("NestChild", NestParent)
        NestChild:SetAllPoints(NestParent)
        NestGrandchild = watched("NestGrandchild", NestChild)
        NestGrandchild:SetPoint("TOPLEFT", NestChild, "TOPLEFT")
        NestGrandchild:SetPoint("BOTTOMRIGHT", NestChild, "BOTTOMRIGHT")
        NestHidden = CreateFrame("Frame", "NestHidden", UIParent)
        NestHidden:SetPoint("CENTER")
        NestHidden:SetSize(10, 10)
        NestHidden:Hide()
    )lua");
    h.layOutInterface();
    h.runInInterface("TOLD = {}");
}

} // namespace

TEST_CASE("frames hidden with their parent are each told they went, and told again when it returns",
          "[interface][harness]") {
    ClientHarness h;
    h.enterWorld();
    buildNest(h);

    h.runInInterface("NestParent:Hide()");
    h.layOutInterface();
    CHECK(h.interfaceAnswers("TOLD.NestChildHide == 1"));
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildHide == 1"));
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildShow == nil"));

    // Nothing more to say while nothing changes.
    h.layOutInterface();
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildHide == 1"));

    h.runInInterface("NestParent:Show()");
    h.layOutInterface();
    CHECK(h.interfaceAnswers("TOLD.NestChildShow == 1"));
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildShow == 1"));
}

TEST_CASE("a frame moved under a hidden frame is told it went",
          "[interface][harness]") {
    ClientHarness h;
    h.enterWorld();
    buildNest(h);

    h.runInInterface("NestChild:SetParent(NestHidden)");
    h.layOutInterface();
    CHECK(h.interfaceAnswers("TOLD.NestChildHide == 1"));
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildHide == 1"));
}

TEST_CASE("a frame hidden and shown again before anything looked is told both",
          "[interface][harness]") {
    ClientHarness h;
    h.enterWorld();
    buildNest(h);

    // FrameXML's way of asking a panel to rebuild itself (Widget::shownToggles).
    h.runInInterface("NestChild:Hide(); NestChild:Show()");
    h.layOutInterface();
    CHECK(h.interfaceAnswers("TOLD.NestChildHide == 1"));
    CHECK(h.interfaceAnswers("TOLD.NestChildShow == 1"));
    // Its children never stopped being on screen.
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildHide == nil"));
}

TEST_CASE("frames stretched by their parent's anchors are told their new size",
          "[interface][harness]") {
    ClientHarness h;
    h.enterWorld();
    buildNest(h);

    h.runInInterface("NestParent:SetWidth(240)");
    h.layOutInterface();
    CHECK(h.interfaceAnswers("TOLD.NestParentSize == 1"));
    // Neither child was resized by anyone calling a setter.
    CHECK(h.interfaceAnswers("TOLD.NestChildSize == 1"));
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildSize == 1"));
    CHECK(h.interfaceAnswers("math.abs(TOLD.NestGrandchildW - NestGrandchild:GetWidth()) < 0.01"));
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildW > TOLD.NestGrandchildH"));

    // Nothing more to say while nothing changes.
    h.layOutInterface();
    CHECK(h.interfaceAnswers("TOLD.NestGrandchildSize == 1"));
}
