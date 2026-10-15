#pragma once

#include "addons/lua_engine.hpp"
#include "addons/toc_parser.hpp"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::addons {

struct InterfaceState;

/// The addons on disk and which of them the player has switched on.
///
/// Process-wide: the AddOns list on the character screen reads and toggles it
/// while no character is in the world, and the choice is the player's, not a
/// character's (addons.cfg). Each character's interface (AddonManager) loads
/// from it; a rescan happens when a character enters the world, so an addon
/// dropped in between two logins is found.
class AddonCatalog {
public:
    AddonCatalog();

    AddonCatalog(const AddonCatalog&) = delete;
    AddonCatalog& operator=(const AddonCatalog&) = delete;

    /// Read the AddOns directories again, and the enabled state with them.
    void scan(const std::string& addonsPath);
    /// Again from the directory the last scan was asked for.
    void rescan();

    /// Where FrameXML lives, remembered at scan time so the loader can find it.
    void setFrameXmlDir(const std::string& dir) { frameXmlDir_ = dir; }
    [[nodiscard]] const std::string& frameXmlDir() const { return frameXmlDir_; }

    [[nodiscard]] const std::vector<TocFile>& addons() const { return addons_; }
    /// The addons that wait to be asked for. Half the interface's own panels
    /// are here - the talent tree, the achievements, the macro editor - and
    /// each is loaded whole or not at all, so one of them raising during load
    /// costs its entire panel rather than degrading it.
    [[nodiscard]] const std::vector<TocFile>& loadOnDemandAddons() const { return lodAddons_; }

    // Per-addon enable/disable (persisted). Disabled addons are skipped by
    // loadAllAddons; changes take effect on the next load (world entry or
    // /reload).
    [[nodiscard]] bool isAddonEnabled(const std::string& addonName) const;
    void setAddonEnabled(const std::string& addonName, bool enabled);

    /// Every global the load-on-demand addons on disk define, read out of their
    /// own files. This is the source of truth for which names must answer as
    /// absent before their addon loads; the literal list in the Lua bootstrap
    /// is a floor for an install with no addons extracted.
    [[nodiscard]] std::vector<std::string> deferredAddonGlobals() const;

private:
    std::vector<TocFile> addons_;
    /// Declared LoadOnDemand: known, listed, and not run until asked for.
    std::vector<TocFile> lodAddons_;
    std::string addonsPath_;
    std::string frameXmlDir_;
    // addonName -> enabled. Absent means enabled (default on).
    std::unordered_map<std::string, bool> addonEnabled_;
    static std::string enabledStatePath();
    void loadEnabledState();
    void saveEnabledState() const;
};

/// One character's interface: the Lua state, the frames FrameXML and the
/// addons build in it, and the saved variables they hold.
///
/// Built by core::CharacterSession when the character enters the world and
/// destroyed with it (or replaced by a /reload). Construction creates the Lua
/// state; loadAllAddons runs the interface, once, at the first world entry.
/// The destructor writes the saved variables - under the name the interface
/// was built for, which cannot change - and then closes the state, so one
/// character's variables are never written to another's file.
class AddonManager {
public:
    /// `characterName` keys the per-character saved variables; empty skips
    /// them rather than guessing at a file (an offline fixture with no
    /// character). Lua starts here: call inside the client's Interface phase.
    AddonManager(AddonCatalog& catalog, InterfaceState& interfaceState,
                 game::GameHandler* gameHandler, const LuaServices& services,
                 std::string characterName);
    ~AddonManager();

    AddonManager(const AddonManager&) = delete;
    AddonManager& operator=(const AddonManager&) = delete;

    /// Run the interface: FrameXML, then every enabled addon, once.
    void loadAllAddons();
    /// Parse an XML file, build what it declares, and follow its includes and
    /// scripts. depth guards against a file that includes itself.
    bool loadXmlFile(const std::string& path, int depth);
    /// Register this client's own settings as a category in FrameXML's
    /// Interface Options, built from the schema. Runs after FrameXML, because
    /// InterfaceOptions_AddCategory is FrameXML's.
    void registerWoweeOptionsPanel();
    /// Move the coin amounts off the coins they are drawn against.
    void giveCoinAmountsClearance();

    /// Load the original interface from its own manifest, in the order it
    /// states. Opt-in through WOWEE_LOAD_FRAMEXML; see loadAllAddons.
    bool loadFrameXml(const std::string& frameXmlDir);
    bool runScript(const std::string& code);
    /// Run one line of interface Lua, for a keybinding whose window FrameXML
    /// now owns. Errors are logged rather than thrown: a bad line here should
    /// cost the keypress, not the frame.
    void runInterfaceCommand(const std::string& lua);
    /// The same, for a command whose answer decides what happens next.
    bool interfaceCommandBoolean(const std::string& expression);

    void fireEvent(const std::string& event, const std::vector<std::string>& args = {});
    void update(float deltaTime);

    [[nodiscard]] const std::vector<TocFile>& getAddons() const { return catalog_.addons(); }
    [[nodiscard]] const std::vector<TocFile>& getLoadOnDemandAddons() const {
        return catalog_.loadOnDemandAddons();
    }

    /// Load a load-on-demand addon by name. Names are matched without regard to
    /// case, because the interface asks for "Blizzard_TalentUI" and the
    /// directory on a case-sensitive filesystem is blizzard_talentui.
    bool loadAddOnByName(const std::string& name, std::string& reason);
    [[nodiscard]] bool isAddOnLoadedByName(const std::string& name) const;
    LuaEngine* getLuaEngine() { return &luaEngine_; }
    [[nodiscard]] bool isInitialized() const { return luaEngine_.isInitialized(); }

    [[nodiscard]] bool isAddonEnabled(const std::string& addonName) const {
        return catalog_.isAddonEnabled(addonName);
    }
    void setAddonEnabled(const std::string& addonName, bool enabled) {
        catalog_.setAddonEnabled(addonName, enabled);
    }
    /// True once loadAllAddons has run in this interface.
    [[nodiscard]] bool addonsLoaded() const { return addonsLoaded_; }

    /// The character whose per-character saved variables this interface reads
    /// and writes. Fixed for the interface's life.
    [[nodiscard]] const std::string& characterName() const { return characterName_; }

    /// Write every loaded addon's saved variables now - on demand, and from the
    /// destructor. Per-character files are keyed by characterName()
    /// (TocFile::savedVariablesPerCharacterPath).
    void saveAllSavedVariables();

private:
    AddonCatalog& catalog_;
    const std::string characterName_;
    game::GameHandler* gameHandler_ = nullptr;
    LuaServices luaServices_;
    LuaEngine luaEngine_;
    std::set<std::string> lodLoaded_;
    /// Every addon this interface has run - at load and on demand - whose
    /// saved variables are therefore in its Lua state to be written back.
    std::vector<TocFile> loadedAddons_;
    /// Whether the first target of the session has asked for an interface
    /// check yet; see fireEvent.
    bool targetCheckRequested_ = false;

    bool loadAddon(const TocFile& addon);

    /// The same directory as it is actually spelled on disk.
    ///
    /// The caller says ".../interface/FrameXML" and this install has
    /// ".../interface/framexml". loadFrameXml resolves that to open it, and
    /// the resolved spelling was thrown away with the local it was kept in -
    /// so every later use of the member was a path that does not exist, on any
    /// filesystem that cares about case.
    std::string frameXmlResolvedDir_;
    /// Why the last loadXmlFile returned false, so a caller loading many files
    /// can report the reasons together instead of leaving them scattered.
    std::string lastXmlError_;
    bool addonsLoaded_ = false;
};

} // namespace wowee::addons
