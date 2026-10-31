/// The rules that keep one realm's downloads out of another realm's, and out
/// of everywhere that is not a patch directory at all.
///
/// A patch archive can carry Lua that FrameXML will load, and the manifest
/// naming it comes from a server whose address somebody typed into a box. So
/// the interesting cases here are not the well-formed ones: they are the
/// names that are trying to be a path, and the ones that look like a bare
/// file name until a particular filesystem reads them.

#include <catch_amalgamated.hpp>

#include <string>

#include "core/realm_patches.hpp"

using wowee::core::isSafePatchFileName;
using wowee::core::realmPatchDirName;

TEST_CASE("an ordinary patch name is accepted", "[patches]") {
    CHECK(isSafePatchFileName("patch-Z.mpq"));
    CHECK(isSafePatchFileName("patch-3.MPQ"));
    CHECK(isSafePatchFileName("chromiecraft_content.mpq"));
    CHECK(isSafePatchFileName("a.mpq"));
}

TEST_CASE("a name that is trying to be a path is refused", "[patches]") {
    // Both separators, because a name is checked once and used on every
    // platform - a backslash is a separator on Windows and an ordinary
    // character on Linux, which is exactly the asymmetry worth refusing.
    CHECK_FALSE(isSafePatchFileName("../patch.mpq"));
    CHECK_FALSE(isSafePatchFileName("..\\patch.mpq"));
    CHECK_FALSE(isSafePatchFileName("sub/patch.mpq"));
    CHECK_FALSE(isSafePatchFileName("sub\\patch.mpq"));
    CHECK_FALSE(isSafePatchFileName("/etc/patch.mpq"));
    CHECK_FALSE(isSafePatchFileName("C:patch.mpq"));
    CHECK_FALSE(isSafePatchFileName("C:\\Windows\\patch.mpq"));
    CHECK_FALSE(isSafePatchFileName("\\\\host\\share\\patch.mpq"));
    // A dot run in the middle, which is how ".." arrives inside a name that
    // starts out looking ordinary.
    CHECK_FALSE(isSafePatchFileName("patch..mpq"));
    CHECK_FALSE(isSafePatchFileName("a..b.mpq"));
}

TEST_CASE("a name that is not plainly a file is refused", "[patches]") {
    CHECK_FALSE(isSafePatchFileName(""));
    CHECK_FALSE(isSafePatchFileName(".mpq"));
    CHECK_FALSE(isSafePatchFileName(".hidden.mpq"));
    CHECK_FALSE(isSafePatchFileName("-rf.mpq"));
    CHECK_FALSE(isSafePatchFileName("patch.mpq."));
    // Not an archive at all. The client only ever mounts MPQs, so a manifest
    // naming anything else is not describing work this can do.
    CHECK_FALSE(isSafePatchFileName("patch.exe"));
    CHECK_FALSE(isSafePatchFileName("patch.dll"));
    CHECK_FALSE(isSafePatchFileName("patch.lua"));
    CHECK_FALSE(isSafePatchFileName("patch.mpq.exe"));
    // A null and a newline inside an otherwise fine name: the first truncates
    // wherever the name reaches C, the second is how a log is forged.
    CHECK_FALSE(isSafePatchFileName(std::string("patch\0.mpq", 10)));
    CHECK_FALSE(isSafePatchFileName("patch\n.mpq"));
    CHECK_FALSE(isSafePatchFileName("patch .mpq"));
    // Far longer than any archive is called.
    CHECK_FALSE(isSafePatchFileName(std::string(200, 'a') + ".mpq"));
}

TEST_CASE("the Windows device names are refused whatever follows them", "[patches]") {
    // These resolve before the filesystem is consulted, so a write to CON.mpq
    // is not a write to a file in the patch directory - on Windows it is not
    // a file at all.
    CHECK_FALSE(isSafePatchFileName("CON.mpq"));
    CHECK_FALSE(isSafePatchFileName("con.mpq"));
    CHECK_FALSE(isSafePatchFileName("NUL.mpq"));
    CHECK_FALSE(isSafePatchFileName("com1.mpq"));
    CHECK_FALSE(isSafePatchFileName("LPT9.mpq"));
    // Not a device, and must still be allowed.
    CHECK(isSafePatchFileName("console.mpq"));
    CHECK(isSafePatchFileName("com10.mpq"));
}

TEST_CASE("a realm's directory is its own", "[patches]") {
    const std::string a = realmPatchDirName("logon.chromiecraft.com", 3724);
    const std::string b = realmPatchDirName("logon.example.com", 3724);
    CHECK(a != b);

    // The port is part of the identity: two realms on one host are two realms.
    CHECK(realmPatchDirName("logon.example.com", 3724) !=
          realmPatchDirName("logon.example.com", 3725));

    // Stable, or every launch downloads the patches again into a new folder.
    CHECK(a == realmPatchDirName("logon.chromiecraft.com", 3724));

    // A hostname is not case-sensitive, so these are one realm and not two.
    CHECK(a == realmPatchDirName("LOGON.ChromieCraft.com", 3724));
}

TEST_CASE("a hostile hostname cannot produce a path", "[patches]") {
    // The address is the player's own, so this is not the attack it would be
    // coming from a server - but it is the one place a directory name is
    // built from text, and a name that escaped would put a realm's archives
    // somewhere no realm should be able to reach.
    const char* hostile[] = {
        "../../etc",
        "..\\..\\windows",
        "/absolute/path",
        "C:\\Windows\\System32",
        "host/../../..",
        "...",
        ".",
        "..",
        "",
        "\\\\unc\\share",
        "host\nname",
    };
    for (const char* h : hostile) {
        const std::string dir = realmPatchDirName(h, 3724);
        INFO("hostname: '" << h << "' produced '" << dir << "'");
        CHECK(dir.find('/') == std::string::npos);
        CHECK(dir.find('\\') == std::string::npos);
        CHECK(dir.find("..") == std::string::npos);
        CHECK(dir.find(':') == std::string::npos);
        CHECK(dir.find('\n') == std::string::npos);
        CHECK_FALSE(dir.empty());
        CHECK(dir.front() != '.');
        CHECK(dir.front() != '-');
    }

    // And two different hostile names are still two different directories,
    // which is the part sanitising alone would get wrong: both of these fold
    // to the same letters.
    CHECK(realmPatchDirName("../../etc", 3724) != realmPatchDirName("..\\..\\etc", 3724));
}
