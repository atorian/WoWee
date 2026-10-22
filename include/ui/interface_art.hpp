#pragma once
#include <string>
#include <vector>
namespace wowee::ui {
struct InterfaceArt { std::string path; bool additive = false; };
// Source declarations, including templates and load-on-demand addons. Does
// not execute scripts or construct character state. Dynamic icon directories
// are deliberately not expanded; the character supplies its actual icons.
std::vector<InterfaceArt> interfaceArtReferences(const std::string& source,
                                                const std::vector<std::string>& texturePaths);
std::vector<InterfaceArt> discoverInterfaceArt(const std::string& interfaceDirectory,
                                              const std::vector<std::string>& texturePaths);
}
