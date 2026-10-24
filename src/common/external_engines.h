// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>

namespace Common {

/// Games that run better on another program: motores.ini next to the executable names, per game
/// serial, the program and how to hand it the game. When the game being started has one and it is
/// there, it is started instead and this process should end. Built in (paths relative to the
/// executable's folder; motores.ini replaces them line by line):
///   CUSA01623,CUSA01715 = spartanport | motores\SpartanPort\SpartanPort.exe
///   CUSA00004,CUSA00223 = issn | motores\issn\inFAMOUS Second Son.exe
///   CUSA02320,CUSA02343,CUSA02344 = uncharted | motores\Uncharted\Uncharted Launcher.exe
/// Kinds:
///   spartanport      writes the game folder into the program's settings.json, runs it with --play
///   issn             the same launcher with its settings in %APPDATA%\issn-launcher\settings.json
///   shadps4-release  writes the eboot path into shadps4-game.txt beside it, runs it with no arguments
///   shadps4          runs it with --game <eboot>
///   uncharted        writes the game folder into %APPDATA%\Uncharted Launcher\launcher.json and
///                    opens that launcher, to choose one of the collection's three games
/// "interno" as the kind keeps the game on this program. Returns true if the other program started.
bool LaunchExternalEngine(const std::string& serial, const std::filesystem::path& eboot);

} // namespace Common
