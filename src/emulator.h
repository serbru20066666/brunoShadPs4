// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <thread>

#include "common/singleton.h"
#include "core/linker.h"
#include "input/controller.h"
#include "sdl_window.h"

namespace Core {

using HLEInitDef = void (*)(Core::Loader::SymbolsResolver* sym);

struct SysModules {
    std::string_view module_name;
    HLEInitDef callback;
};

/**
 * Launches a new emulator process with the supplied CLI arguments, then terminates the calling
 * process. The new process waits for this one to exit before initializing.
 *
 * This is a free function (rather than a method) so that callers who only want to hand off to a
 * freshly spawned instance - such as the game launcher - don't need to construct a full Emulator
 * first: doing so would register its Shutdown() as an at_quick_exit handler, which assumes a
 * previous Run() and can needlessly slow down process exit for a launcher that never ran one.
 */
[[noreturn]] void RelaunchProcess(const char* executableName, std::vector<std::string> args);

class Emulator {
public:
    Emulator();
    ~Emulator();

    void Run(std::filesystem::path file, std::vector<std::string> args = {},
             std::optional<std::filesystem::path> game_folder = {},
             std::vector<std::pair<std::filesystem::path, std::string>> mounts = {},
             std::vector<std::string> const& env_vars = {}, bool append_log = false);
    void UpdatePlayTime(const std::string_view serial);
    void Shutdown();

    /**
     * This will kill the current process and launch a new process with the same configuration
     * (using CLI args) but replacing the eboot image and guest arguments
     */
    void Restart(std::filesystem::path eboot_path, const std::vector<std::string>& guest_args = {});

    /// Calls RelaunchProcess() with this instance's executableName.
    [[noreturn]] void Relaunch(std::vector<std::string> args);

    const char* executableName;
    bool waitForDebuggerBeforeRun{false};

private:
    void LoadSystemModules(const std::string& game_serial);

    Core::MemoryManager* memory;
    Input::GameControllers* controllers;
    Core::Linker* linker;
    std::unique_ptr<Frontend::WindowSDL> window;
    std::chrono::steady_clock::time_point start_time;
    std::jthread play_time_thread;
};

} // namespace Core
