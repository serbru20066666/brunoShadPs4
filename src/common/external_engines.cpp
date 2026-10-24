// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>

#include "common/external_engines.h"
#include "common/logging/log.h"
#include "common/path_util.h"

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "gdi32.lib")
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
    ParseLine(R"(CUSA00004,CUSA00223 = issn | motores\issn\inFAMOUS Second Son.exe)", table);
    ParseLine(R"(CUSA02320,CUSA02343,CUSA02344 = uncharted | motores\Uncharted\Uncharted Launcher.exe)",
              table);
    std::ifstream in{folder / "motores.ini"};
    std::string line;
    while (in && std::getline(in, line)) {
        ParseLine(line, table);
    }
    return table;
}

/// The engine of a game and its program's full path, if the game has one and the program is there.
std::optional<std::pair<Engine, std::filesystem::path>> FindEngine(const std::string& serial) {
    if (serial.empty()) {
        return std::nullopt;
    }
    const auto folder = ExecutableFolder();
    const auto table = LoadTable(folder);
    const auto it = table.find(serial);
    if (it == table.end() || it->second.kind == "interno") {
        return std::nullopt;
    }
    const auto program =
        it->second.program.is_absolute() ? it->second.program : folder / it->second.program;
    std::error_code ec;
    if (!std::filesystem::exists(program, ec)) {
        return std::nullopt;
    }
    return std::make_pair(it->second, program);
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    return static_cast<bool>(out);
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path) {
    if (std::ifstream in{path}; in) {
        auto json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_object()) {
            return json;
        }
    }
    return nlohmann::json::object();
}

/// Sets "game_dir" in a launcher's JSON settings, keeping everything else.
bool SetGameDir(const std::filesystem::path& settings_path, const std::filesystem::path& eboot) {
    auto settings = ReadJsonFile(settings_path);
    settings["game_dir"] = FS::PathToUTF8String(eboot.parent_path());
    std::error_code ec;
    std::filesystem::create_directories(settings_path.parent_path(), ec);
    return WriteText(settings_path, settings.dump(2));
}

/// A launcher's settings file in the user's AppData (the Uncharted collection's and issn_pc's).
std::filesystem::path AppDataFile(const std::filesystem::path& relative) {
#ifdef _WIN32
    if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata && *appdata) {
        return std::filesystem::path{appdata} / relative;
    }
#endif
    return {};
}

std::string EngineName(const Engine& engine, const std::filesystem::path& program) {
    if (engine.kind == "spartanport") {
        return "SpartanPort";
    }
    if (engine.kind == "issn") {
        return "issn_pc";
    }
    if (engine.kind == "uncharted") {
        return "Uncharted PC port";
    }
    return FS::PathToUTF8String(program.stem());
}

/// "2.7.0" from the first lines of the program's README (the ports name their version there).
std::string EngineVersion(const std::filesystem::path& folder) {
    static const std::regex version{R"((\d+\.\d+\.\d+))"};
    for (const char* name : {"VERSION", "README-Windows.txt", "README.txt"}) {
        std::ifstream in{folder / name};
        std::string line;
        for (int i = 0; i < 3 && std::getline(in, line); ++i) {
            if (std::smatch found; std::regex_search(line, found, version)) {
                return found[1].str();
            }
        }
    }
    return {};
}

/// What a port of the SpartanPort family used the last time it ran (its user\last_run.log).
std::array<std::string, 4> LastRunDetails(const std::filesystem::path& folder) {
    std::array<std::string, 4> details{};
    std::ifstream in{folder / "user" / "last_run.log"};
    if (!in) {
        details[0] = "Ajustes: en el lanzador del port";
        details[1] = "Aún sin jugar con este port";
        return details;
    }
    std::string line, window, fps, vsync, patches;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.starts_with("Window: ") && line.find("controller") == std::string::npos) {
            window = line.substr(8);
        } else if (line.starts_with("Engine frame rate: ")) {
            // "120 FPS, live up to 240" / "60 FPS (the frame rate patch ...)"
            fps = line.substr(19);
            fps = fps.substr(0, fps.find_first_of(",("));
            fps = Trim(fps);
        } else if (line.starts_with("Swapchain: ")) {
            vsync = line.find("VSync on") != std::string::npos ? "V-Sync: activado"
                                                               : "V-Sync: desactivado";
        } else if (line.find("3840x2160") != std::string::npos && line.starts_with("Patches:")) {
            patches = "Resolución: 3840 x 2160 (4K)";
        } else if (line.find("2560x1440") != std::string::npos && line.starts_with("Patches:")) {
            patches = "Resolución: 2560 x 1440";
        }
    }
    const std::map<std::string, std::string> modes{
        {"Windowed", "En ventana"}, {"Borderless", "Pantalla completa (sin bordes)"},
        {"Fullscreen", "Pantalla completa"}};
    const auto mode = modes.find(window);
    details[0] = patches.empty() ? "Resolución: 1920 x 1080" : patches;
    details[1] = window.empty() ? "" : (mode != modes.end() ? mode->second : window);
    details[2] = fps.empty() ? "" : "Cuadros: " + fps;
    details[3] = vsync;
    return details;
}

/// The Uncharted collection's launcher: the game chosen last and its preset.
std::array<std::string, 4> UnchartedDetails() {
    std::array<std::string, 4> details{};
    const auto config = ReadJsonFile(AppDataFile("Uncharted Launcher/launcher.json"));
    const std::string game = config.value("game", std::string{"u1"});
    const std::map<std::string, std::string> names{
        {"u1", "Uncharted 1"}, {"u2", "Uncharted 2"}, {"u3", "Uncharted 3"}};
    const std::map<std::string, std::string> presets{
        {"low", "bajo"}, {"medium", "medio"}, {"high", "alto"}, {"ultra", "ultra"}};
    const auto name = names.find(game);
    details[0] = "Último elegido: " + (name != names.end() ? name->second : game);
    if (config.contains("games") && config["games"].contains(game)) {
        const auto& g = config["games"][game];
        details[1] = "Resolución: " + g.value("resolution", std::string{"1920x1080"});
        const auto preset = presets.find(g.value("preset", std::string{"medium"}));
        details[2] = "Calidad: " + (preset != presets.end() ? preset->second : g.value("preset", std::string{}));
    }
    details[3] = "El juego se elige al abrir";
    return details;
}

#ifdef _WIN32
/// NAME=VALUE lines of brunoshadps4.env beside the program, set for it (and what it starts).
void ApplyEnvFile(const std::filesystem::path& folder) {
    std::ifstream in{folder / "brunoshadps4.env"};
    std::string line;
    while (in && std::getline(in, line)) {
        line = Trim(line);
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) {
            continue;
        }
        const auto name = std::filesystem::path{Trim(line.substr(0, equals))}.wstring();
        const auto value = std::filesystem::path{Trim(line.substr(equals + 1))}.wstring();
        SetEnvironmentVariableW(name.c_str(), value.c_str());
        LOG_INFO(Common, "Engine environment: {}", Trim(line.substr(0, equals)));
    }
}

/// The small window shown while the other program prepares the game.
struct Splash {
    HWND window{};
    std::wstring line1, line2;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    int phase{};

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Splash*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            HBRUSH back = CreateSolidBrush(RGB(18, 18, 24));
            FillRect(dc, &r, back);
            DeleteObject(back);
            SetBkMode(dc, TRANSPARENT);
            HFONT big = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                    CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT small = CreateFontW(18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                      CLEARTYPE_QUALITY, 0, L"Segoe UI");
            auto old = SelectObject(dc, big);
            SetTextColor(dc, RGB(240, 240, 245));
            RECT t{24, 20, r.right - 24, 60};
            DrawTextW(dc, self->line1.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, small);
            SetTextColor(dc, RGB(160, 165, 180));
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - self->start)
                                  .count();
            const std::wstring text =
                self->line2 + L"   (" + std::to_wstring(secs / 60) + L":" +
                (secs % 60 < 10 ? L"0" : L"") + std::to_wstring(secs % 60) + L")";
            RECT t2{24, 62, r.right - 24, 90};
            DrawTextW(dc, text.c_str(), -1, &t2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            DeleteObject(big);
            DeleteObject(small);
            // A bar that keeps moving: the other program says nothing about its progress.
            const int bar_top = r.bottom - 34, bar_h = 10, width = r.right - 48;
            HBRUSH track = CreateSolidBrush(RGB(40, 44, 60));
            RECT tr{24, bar_top, 24 + width, bar_top + bar_h};
            FillRect(dc, &tr, track);
            DeleteObject(track);
            const int block = width / 4;
            const int x = 24 + (self->phase * 6) % (width + block) - block;
            RECT br{std::max(24, x), bar_top, std::min(24 + width, x + block), bar_top + bar_h};
            HBRUSH accent = CreateSolidBrush(RGB(40, 140, 255));
            if (br.right > br.left) {
                FillRect(dc, &br, accent);
            }
            DeleteObject(accent);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_TIMER && self) {
            ++self->phase;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_CLOSE) {
            return 0; // it closes by itself when the game shows
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void Open() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"brunoShadPs4Abriendo";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc);
        const int w = 620, h = 150;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"brunoShadPs4",
                                 WS_POPUP | WS_BORDER, x, y, w, h, nullptr, nullptr, wc.hInstance,
                                 nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ShowWindow(window, SW_SHOWNOACTIVATE);
        SetTimer(window, 1, 30, nullptr);
    }

    void Pump() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void Close() {
        if (window) {
            KillTimer(window, 1);
            DestroyWindow(window);
            window = nullptr;
            Pump();
        }
    }
};

struct WindowSearch {
    DWORD own_pid;
    FILETIME started;
    bool found;
};

/// A visible top-level window of a process that started after the game was launched: the other
/// program (or the game it started) is showing something.
BOOL CALLBACK FindNewWindow(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == search->own_pid) {
        return TRUE;
    }
    RECT r;
    if (!GetWindowRect(hwnd, &r) || r.right - r.left < 320 || r.bottom - r.top < 200) {
        return TRUE;
    }
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (std::wstring{cls} == L"ConsoleWindowClass" || std::wstring{cls} == L"PseudoConsoleWindow") {
        return TRUE;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return TRUE;
    }
    FILETIME created, exited, kernel, user;
    const bool newer = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                       CompareFileTime(&created, &search->started) >= 0;
    CloseHandle(process);
    if (newer) {
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

/// Starts the program in a job, shows the splash until it shows a window, then waits for the job
/// (the program and everything it started) to end.
bool StartAndWait(const std::filesystem::path& program, const std::vector<std::string>& args,
                  const std::string& title, const std::string& engine_name) {
    ApplyEnvFile(program.parent_path());
    std::wstring cmdline = L"\"" + program.wstring() + L"\"";
    for (const auto& arg : args) {
        cmdline += L" \"" + std::filesystem::path{arg}.wstring() + L"\"";
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    FILETIME started;
    GetSystemTimeAsFileTime(&started);
    const std::wstring folder = program.parent_path().wstring();
    if (!CreateProcessW(program.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, folder.c_str(), &si, &pi)) {
        LOG_ERROR(Common, "Could not start {}: error {}", FS::PathToUTF8String(program),
                  GetLastError());
        if (job) {
            CloseHandle(job);
        }
        return false;
    }
    if (job) {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    // Without the focus the new window gets no controller input.
    AllowSetForegroundWindow(pi.dwProcessId);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    Splash splash;
    splash.line1 = L"Abriendo " + std::filesystem::path{title}.wstring();
    splash.line2 =
        L"con " + std::filesystem::path{engine_name}.wstring() + L"  -  la primera vez tarda mas";
    splash.Open();
    const auto running = [&] {
        if (!job) {
            return WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info),
                                  nullptr);
        return info.ActiveProcesses > 0;
    };
    while (running()) {
        splash.Pump();
        WindowSearch search{GetCurrentProcessId(), started, false};
        EnumWindows(FindNewWindow, reinterpret_cast<LPARAM>(&search));
        if (search.found) {
            break;
        }
        Sleep(100);
    }
    splash.Close();
    // Stay until the game ends: front ends such as RetroBat wait for this process.
    while (running()) {
        Sleep(500);
    }
    CloseHandle(pi.hProcess);
    if (job) {
        CloseHandle(job);
    }
    return true;
}
#endif

} // namespace

std::optional<ExternalEngineCard> DescribeExternalEngine(const std::string& serial) {
    const auto found = FindEngine(serial);
    if (!found) {
        return std::nullopt;
    }
    const auto& [engine, program] = *found;
    ExternalEngineCard card;
    const auto version = EngineVersion(program.parent_path());
    card.engine = EngineName(engine, program) + (version.empty() ? "" : " " + version);
    if (engine.kind == "spartanport" || engine.kind == "issn") {
        card.details = LastRunDetails(program.parent_path());
    } else if (engine.kind == "uncharted") {
        card.details = UnchartedDetails();
    } else {
        card.details[0] = "Ajustes: en su propio programa";
    }
    return card;
}

bool LaunchExternalEngine(const std::string& serial, const std::string& title,
                          const std::filesystem::path& eboot) {
    if (serial.empty()) {
        return false;
    }
    const auto found = FindEngine(serial);
    if (!found) {
        return false;
    }
    const auto& [engine, program] = *found;
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
    } else if (engine.kind == "issn") {
        const auto settings = AppDataFile("issn-launcher/settings.json");
        if (settings.empty() || !SetGameDir(settings, eboot)) {
            LOG_ERROR(Common, "Could not write issn_pc's settings.json");
            return false;
        }
        args.emplace_back("--play");
    } else if (engine.kind == "uncharted") {
        const auto settings = AppDataFile("Uncharted Launcher/launcher.json");
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
#ifdef _WIN32
    return StartAndWait(program, args, title.empty() ? serial : title,
                        EngineName(engine, program));
#else
    LOG_ERROR(Common, "Other programs for a game are only started on Windows");
    return false;
#endif
}

} // namespace Common
