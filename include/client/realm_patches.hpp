#pragma once

/// Realm-supplied patch archives, and the rules that keep one realm's
/// downloads out of every other realm's.
///
/// A patch MPQ is not only art. FrameXML loads Lua out of the archives, so a
/// file a realm hands over is closer to a plugin than to a texture pack, and
/// the realm that supplies it is one whose address somebody typed into a box.
/// Two things follow, and both live here rather than at the call sites:
///
///   - Every realm gets a directory of its own, named from its address by a
///     rule that cannot produce a path. Nothing a manifest says can widen
///     that, because the directory is derived from the address the player
///     connected to and never from the manifest.
///   - Every name inside a manifest is checked before it is joined to
///     anything. A name is a bare file name or it is refused; there is no
///     sanitising pass that tries to repair one.
///
/// The name check and the naming are pure functions on purpose: this is the part
/// that has to be right, and it is the part that can be tested without a
/// network or a server.

#include <string>

namespace wowee::core {

/// Whether `name` may be joined to a realm's patch directory.
///
/// A bare file name ending in .mpq, of the characters an MPQ is ever actually
/// called: letters, digits, dot, dash, underscore. Everything else is refused,
/// including every spelling of a path - separators of both kinds, "..", a
/// leading dot, a drive letter, a UNC prefix - and the device names Windows
/// resolves before it looks at the filesystem.
///
/// Refusing rather than sanitising is the point. A repaired name is a name
/// somebody chose and the client then changed, and the interesting cases are
/// the ones where the repair produces something that still resolves.
[[nodiscard]] bool isSafePatchFileName(const std::string& name);

/// The directory this realm's archives live in, relative to the patch root.
///
/// Derived from the address connected to, never from anything a server sends.
/// The readable part is for a person looking in the folder; the hash is what
/// makes it unambiguous, since sanitising two different hostnames can arrive
/// at the same letters and they must not arrive at the same directory.
[[nodiscard]] std::string realmPatchDirName(const std::string& host, int port);

}  // namespace wowee::core
