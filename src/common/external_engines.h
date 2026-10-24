// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace Common {

/// Games that run better on another program: motores.ini next to the executable names, per game
/// serial, the program and how to hand it the game. When the game being started has one and it is
/// there, it is started instead. Built in (paths relative to the executable's folder; motores.ini
/// replaces them line by line):
///   CUSA01623,CUSA01715 = spartanport | motores\SpartanPort\SpartanPort.exe
///   CUSA00004,CUSA00223 = shadps4-release | motores\inFamous\shadPS4.exe
///   CUSA02320,CUSA02343,CUSA02344 = uncharted | motores\Uncharted\Uncharted Launcher.exe
/// Kinds:
///   spartanport      writes the game folder into the program's settings.json, runs it with --play
///   issn             the same launcher with its settings in %APPDATA%\issn-launcher\settings.json
///   shadps4-release  writes the eboot path into shadps4-game.txt beside it, runs it with no arguments
///   shadps4          runs it with --game <eboot>
///   uncharted        writes the game folder into %APPDATA%\Uncharted Launcher\launcher.json and
///                    opens that launcher, to choose one of the collection's three games
/// "interno" as the kind keeps the game on this program.

/// What a game card says about a game that runs on another program, from that program's own
/// settings (or what it used the last time). Empty for games that run here.
struct ExternalEngineCard {
    std::string engine;
    std::array<std::string, 4> details;
};
std::optional<ExternalEngineCard> DescribeExternalEngine(const std::string& serial);

/// The render resolution of a game on another program, as "1080p", "1440p" or "4K" (empty when
/// that program's resolution cannot be set from here), and the next one, written into that
/// program's own settings.
std::string ExternalEngineResolution(const std::string& serial);
std::string CycleExternalEngineResolution(const std::string& serial);

/// Starts the game on its program, if it has one there. While that program prepares the game a
/// small window says so; this returns once the program and everything it started have ended, so
/// front ends such as RetroBat wait for the game. Returns false if the game runs here.
bool LaunchExternalEngine(const std::string& serial, const std::string& title,
                          const std::filesystem::path& eboot);

} // namespace Common
