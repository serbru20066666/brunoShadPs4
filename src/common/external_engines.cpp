// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <map>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>

#include "common/external_engines.h"
#include "common/logging/log.h"
#include "common/path_util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {

namespace {

struct Engine {
    std::string kind;
    std::filesystem::path program;
};

std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

std::filesystem::path ExecutableFolder() {
#ifdef _WIN32
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path{path}.parent_path();
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

/// "CUSA01623,CUSA01715 = spartanport | motores\SpartanPort\SpartanPort.exe"
void ParseLine(const std::string& line, std::map<std::string, Engine>& table) {
    const std::string text = Trim(line.substr(0, line.find('#')));
    const auto equals = text.find('=');
    if (text.empty() || equals == std::string::npos) {
        return;
    }
    const std::string rest = text.substr(equals + 1);
    const auto bar = rest.find('|');
    Engine engine{Trim(rest.substr(0, bar)),
                  bar == std::string::npos ? std::filesystem::path{}
                                           : std::filesystem::path{Trim(rest.substr(bar + 1))}};
    std::stringstream serials{text.substr(0, equals)};
    std::string serial;
    while (std::getline(serials, serial, ',')) {
        if (serial = Trim(serial); !serial.empty()) {
            table[serial] = engine;
        }
    }
}

std::map<std::string, Engine> LoadTable(const std::filesystem::path& folder) {
    std::map<std::string, Engine> table;
    ParseLine(R"(CUSA01623,CUSA01715 = spartanport | motores\SpartanPort\SpartanPort.exe)", table);
    ParseLine(R"(CUSA00004,CUSA00223 = shadps4-release | motores\inFamous\shadPS4.exe)", table);
    ParseLine(R"(CUSA02320,CUSA02343,CUSA02344 = uncharted | motores\Uncharted\Uncharted Launcher.exe)",
              table);
    std::ifstream in{folder / "motores.ini"};
    std::string line;
    while (in && std::getline(in, line)) {
        ParseLine(line, table);
    }
    return table;
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    return static_cast<bool>(out);
}

/// Sets "game_dir" in a launcher's JSON settings, keeping everything else.
bool SetGameDir(const std::filesystem::path& settings_path, const std::filesystem::path& eboot) {
    nlohmann::json settings = nlohmann::json::object();
    if (std::ifstream in{settings_path}; in) {
        settings = nlohmann::json::parse(in, nullptr, false);
        if (!settings.is_object()) {
            settings = nlohmann::json::object();
        }
    }
    settings["game_dir"] = FS::PathToUTF8String(eboot.parent_path());
    std::error_code ec;
    std::filesystem::create_directories(settings_path.parent_path(), ec);
    return WriteText(settings_path, settings.dump(2));
}

/// The Uncharted collection's launcher keeps its settings in the user's AppData.
std::filesystem::path UnchartedSettings() {
#ifdef _WIN32
    if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata && *appdata) {
        return std::filesystem::path{appdata} / "Uncharted Launcher" / "launcher.json";
    }
#endif
    return {};
}

bool Start(const std::filesystem::path& program, const std::vector<std::string>& args) {
#ifdef _WIN32
    std::wstring cmdline = L"\"" + program.wstring() + L"\"";
    for (const auto& arg : args) {
        cmdline += L" \"" + std::filesystem::path{arg}.wstring() + L"\"";
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring folder = program.parent_path().wstring();
    if (!CreateProcessW(program.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        folder.c_str(), &si, &pi)) {
        LOG_ERROR(Common, "Could not start {}: error {}", FS::PathToUTF8String(program),
                  GetLastError());
        return false;
    }
    // Without the focus the new window gets no controller input.
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    LOG_ERROR(Common, "Other programs for a game are only started on Windows");
    return false;
#endif
}

} // namespace

bool LaunchExternalEngine(const std::string& serial, const std::filesystem::path& eboot) {
    if (serial.empty()) {
        return false;
    }
    const auto folder = ExecutableFolder();
    const auto table = LoadTable(folder);
    const auto it = table.find(serial);
    if (it == table.end() || it->second.kind == "interno") {
        return false;
    }
    const auto& engine = it->second;
    const auto program =
        engine.program.is_absolute() ? engine.program : folder / engine.program;
    std::error_code ec;
    if (!std::filesystem::exists(program, ec)) {
        LOG_WARNING(Common, "{} is set to run on {}, which is not there: running it here", serial,
                    FS::PathToUTF8String(program));
        return false;
    }
    const auto program_folder = program.parent_path();
    std::vector<std::string> args;
    if (engine.kind == "spartanport") {
        // SpartanPort keeps its options in settings.json beside it.
        if (!SetGameDir(program_folder / "settings.json", eboot)) {
            LOG_ERROR(Common, "Could not write SpartanPort's settings.json");
            return false;
        }
        args.emplace_back("--play");
    } else if (engine.kind == "shadps4-release") {
        if (!WriteText(program_folder / "shadps4-game.txt", FS::PathToUTF8String(eboot) + "\n")) {
            LOG_ERROR(Common, "Could not write shadps4-game.txt");
            return false;
        }
    } else if (engine.kind == "uncharted") {
        const auto settings = UnchartedSettings();
        if (settings.empty() || !SetGameDir(settings, eboot)) {
            LOG_ERROR(Common, "Could not write the Uncharted launcher's settings");
            return false;
        }
    } else if (engine.kind == "shadps4") {
        args = {"--game", FS::PathToUTF8String(eboot)};
    } else {
        LOG_WARNING(Common, "Unknown kind '{}' for {} in motores.ini", engine.kind, serial);
        return false;
    }
    LOG_INFO(Common, "{} runs on {} ({})", serial, FS::PathToUTF8String(program), engine.kind);
    return Start(program, args);
}

} // namespace Common
