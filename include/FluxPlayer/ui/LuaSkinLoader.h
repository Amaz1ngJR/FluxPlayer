#pragma once

#include "FluxPlayer/ui/Skin.h"

#include <filesystem>
#include <string>

namespace FluxPlayer {

/** Load static tokens from a single-file Lua skin into the renderer snapshot. */
bool loadLuaSkinSnapshot(const std::filesystem::path& entryPath,
                         SkinSource source,
                         SkinSnapshot& out,
                         std::string& error);

} // namespace FluxPlayer
