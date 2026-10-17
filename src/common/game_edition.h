// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Common {

/// A game's own edition of the program: a juego.ini next to the executable names the game. The
/// launcher then shows only that game, and its windows carry the game's name and picture.
///   title=The Order: 1886
///   serials=CUSA00076,CUSA00035,CUSA00100
struct GameEdition {
    bool active{};
    std::string title;
    std::vector<std::string> serials;
    /// The folder of the executable.
    std::filesystem::path folder;
};

/// Reads the juego.ini next to the executable the first time; returns the same after that.
const GameEdition& LoadGameEdition(const std::filesystem::path& executable);

} // namespace Common
