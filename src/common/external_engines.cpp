// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <vector>
#include <SDL3/SDL.h>
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

/// A shadPS4 release build (shadps4-flags*.txt beside it): its render height and display mode.
std::array<std::string, 4> ReleaseBuildDetails(const std::filesystem::path& folder) {
    std::array<std::string, 4> details{};
    std::string height;
    for (const char* name : {"shadps4-flags.txt", "shadps4-flags-user.txt"}) {
        std::ifstream in{folder / name};
        std::string line;
        while (in && std::getline(in, line)) {
            line = Trim(line);
            if (line.starts_with("SHADPS4_RENDER_HEIGHT=")) {
                height = line.substr(22);
            }
        }
    }
    const std::map<std::string, std::string> sizes{
        {"2160", "3840 x 2160 (4K)"}, {"1440", "2560 x 1440"}, {"1080", "1920 x 1080"}};
    const auto size = sizes.find(height);
    details[0] = "Resolución: " + (size != sizes.end() ? size->second
                                   : height.empty()    ? std::string{"1920 x 1080"}
                                                       : height + " líneas");
    bool full = false;
    for (const char* name : {"user/config.json", "user/custom_configs/CUSA00004.json"}) {
        const auto json = ReadJsonFile(folder / name);
        if (json.contains("GPU") && json["GPU"].contains("full_screen")) {
            full = json["GPU"]["full_screen"].get<bool>();
        }
    }
    details[1] = full ? "Pantalla completa" : "En ventana";
    details[2] = "Ajustes: en su programa (F3)";
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
    if (job) {
        // When this process ends, by itself or because a front end closed it (RetroBat's Select +
        // Start closes the program it started, which is this one), the game goes with it.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
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

/// Sets KEY=value in a KEY=value text file (an .ini or a flags file), keeping the other lines.
bool SetKeyValue(const std::filesystem::path& path, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    bool done = false;
    {
        std::ifstream in{path};
        std::string line;
        while (in && std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (Trim(line).starts_with(key + "=")) {
                line = key + "=" + value;
                done = true;
            }
            lines.push_back(line);
        }
    }
    if (!done) {
        lines.push_back(key + "=" + value);
    }
    std::string text;
    for (const auto& line : lines) {
        text += line + "\n";
    }
    return WriteText(path, text);
}

std::string ReadKeyValue(const std::filesystem::path& path, const std::string& key) {
    std::ifstream in{path};
    std::string line, value;
    while (in && std::getline(in, line)) {
        line = Trim(line);
        if (line.starts_with(key + "=")) {
            value = Trim(line.substr(key.size() + 1));
        }
    }
    return value;
}

constexpr std::array<const char*, 3> Resolutions{"1080p", "1440p", "4K"};

/// A screen before the game to choose one of a few options (its resolution, a game of a
/// collection), with the pad (D-pad, cross to accept, circle to go back), the keyboard or the mouse:
/// front ends such as RetroBat start the game directly, without this emulator's launcher. Returns
/// the chosen index, or nothing when the player went back.
std::optional<int> ChooseOption(const std::string& title, const std::string& subtitle,
                                const std::vector<std::string>& labels, int selected) {
    const int n = static_cast<int>(labels.size());
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        return selected;
    }
    std::vector<SDL_Gamepad*> pads;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
                pads.push_back(pad);
            }
        }
        SDL_free(ids);
    }
    const int width = 1100, height = 620;
    SDL_Window* window = SDL_CreateWindow("brunoShadPs4", width, height, SDL_WINDOW_BORDERLESS);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return selected;
    }
    SDL_RaiseWindow(window);
    // The debug font has ASCII only.
    std::string ascii_title;
    for (const char c : title) {
        if (static_cast<unsigned char>(c) < 128) {
            ascii_title += c;
        }
    }
    const auto text = [&](float x, float y, float scale, const char* s, Uint8 r, Uint8 g, Uint8 b) {
        SDL_SetRenderScale(renderer, scale, scale);
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderDebugText(renderer, x / scale, y / scale, s);
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    };
    std::optional<int> result;
    bool done = false;
    int last_axis = 0;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                done = true;
            } else if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) {
                    pads.push_back(pad);
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                    selected = (selected + n - 1) % n;
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                    selected = (selected + 1) % n;
                    break;
                case SDL_GAMEPAD_BUTTON_SOUTH:
                case SDL_GAMEPAD_BUTTON_START:
                    result = selected;
                    done = true;
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:
                case SDL_GAMEPAD_BUTTON_BACK:
                    done = true;
                    break;
                default:
                    break;
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                       e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                const int dir = e.gaxis.value > 20000 ? 1 : e.gaxis.value < -20000 ? -1 : 0;
                if (dir != 0 && dir != last_axis) {
                    selected = (selected + n + dir) % n;
                }
                last_axis = dir;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_UP || e.key.key == SDLK_LEFT) {
                    selected = (selected + n - 1) % n;
                } else if (e.key.key == SDLK_DOWN || e.key.key == SDLK_RIGHT) {
                    selected = (selected + 1) % n;
                } else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE) {
                    result = selected;
                    done = true;
                } else if (e.key.key == SDLK_ESCAPE) {
                    done = true;
                }
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                const int row = static_cast<int>((e.button.y - 200.0f) / 110.0f);
                if (e.button.y >= 200.0f && row < n && e.button.x > 80 && e.button.x < width - 80) {
                    selected = row;
                    result = selected;
                    done = true;
                }
            }
        }
        SDL_SetRenderDrawColor(renderer, 14, 14, 20, 255);
        SDL_RenderClear(renderer);
        text(80, 50, 4.0f, ascii_title.c_str(), 245, 245, 250);
        text(80, 120, 2.5f, subtitle.c_str(), 160, 168, 190);
        for (int i = 0; i < n; ++i) {
            const SDL_FRect box{80.0f, 200.0f + i * 110.0f, width - 160.0f, 90.0f};
            const bool on = i == selected;
            SDL_SetRenderDrawColor(renderer, on ? 40 : 30, on ? 120 : 32, on ? 255 : 44, 255);
            SDL_RenderFillRect(renderer, &box);
            text(box.x + 30, box.y + 30, 3.5f, labels[i].c_str(), 255, 255, 255);
        }
        text(80, height - 60.0f, 2.0f, "Cruceta: elegir     X: aceptar     O: volver", 150, 155, 175);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    for (SDL_Gamepad* pad : pads) {
        SDL_CloseGamepad(pad);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    return result;
}

/// The resolution screen: "1080p", "1440p" or "4K", or nothing when the player went back.
std::optional<std::string> ChooseResolution(const std::string& title, const std::string& current) {
    int selected = 0;
    for (int i = 0; i < 3; ++i) {
        if (current == Resolutions[i]) {
            selected = i;
        }
    }
    const auto choice = ChooseOption(title, "Elige la resolucion",
                                     {"1080p   (1920 x 1080)", "1440p   (2560 x 1440, 2K)",
                                      "4K      (3840 x 2160)"},
                                     selected);
    if (!choice) {
        return std::nullopt;
    }
    return std::string{Resolutions[*choice]};
}

} // namespace

std::string ExternalEngineResolution(const std::string& serial) {
    const auto found = FindEngine(serial);
    if (!found) {
        return {};
    }
    const auto& [engine, program] = *found;
    const auto folder = program.parent_path();
    if (engine.kind == "shadps4-release") {
        const auto height = ReadKeyValue(folder / "shadps4-flags-user.txt", "SHADPS4_RENDER_HEIGHT");
        return height == "2160" ? "4K" : height == "1440" ? "1440p" : "1080p";
    }
    if (engine.kind == "spartanport") {
        const auto res = ReadKeyValue(folder / "spartanport.ini", "render_resolution");
        return res == "4K" ? "4K" : res == "1440p" ? "1440p" : "1080p";
    }
    if (engine.kind == "uncharted") {
        const auto config = ReadJsonFile(AppDataFile("Uncharted Launcher/launcher.json"));
        const std::string game = config.value("game", std::string{"u1"});
        std::string res = "1920x1080";
        if (config.contains("games") && config["games"].contains(game)) {
            res = config["games"][game].value("resolution", res);
        }
        return res == "3840x2160" ? "4K" : res == "2560x1440" ? "1440p" : "1080p";
    }
    return {};
}

std::string CycleExternalEngineResolution(const std::string& serial) {
    const auto current = ExternalEngineResolution(serial);
    if (current.empty()) {
        return {};
    }
    size_t index = 0;
    while (index < Resolutions.size() && current != Resolutions[index]) {
        ++index;
    }
    const std::string next = Resolutions[(index + 1) % Resolutions.size()];
    const auto& [engine, program] = *FindEngine(serial);
    const auto folder = program.parent_path();
    if (engine.kind == "shadps4-release") {
        SetKeyValue(folder / "shadps4-flags-user.txt", "SHADPS4_RENDER_HEIGHT",
                    next == "4K" ? "2160" : next == "1440p" ? "1440" : "1080");
    } else if (engine.kind == "spartanport") {
        const auto ini = folder / "spartanport.ini";
        SetKeyValue(ini, "render_resolution", next == "1080p" ? "native" : next);
        SetKeyValue(ini, "output_res", next == "4K" ? "3840x2160" : next == "1440p" ? "2560x1440"
                                                                                   : "1920x1080");
    } else if (engine.kind == "uncharted") {
        const auto path = AppDataFile("Uncharted Launcher/launcher.json");
        auto config = ReadJsonFile(path);
        const std::string res =
            next == "4K" ? "3840x2160" : next == "1440p" ? "2560x1440" : "1920x1080";
        if (config.contains("games") && config["games"].is_object()) {
            for (auto& [key, game] : config["games"].items()) {
                if (game.is_object()) {
                    game["resolution"] = res;
                }
            }
        }
        WriteText(path, config.dump(2));
    }
    return next;
}

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
    } else if (engine.kind == "shadps4-release") {
        card.details = ReleaseBuildDetails(program.parent_path());
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
        // The resolution here (the port's launcher is hard to use with a pad), the rest at the
        // best the port offers, then straight in.
        const auto choice = ChooseResolution(title.empty() ? serial : title,
                                             ExternalEngineResolution(serial));
        if (!choice) {
            LOG_INFO(Common, "{}: the player went back before playing", serial);
            return true;
        }
        const auto ini = program_folder / "spartanport.ini";
        SetKeyValue(ini, "render_resolution", *choice == "1080p" ? "native" : *choice);
        SetKeyValue(ini, "output_res", *choice == "4K"      ? "3840x2160"
                                       : *choice == "1440p" ? "2560x1440"
                                                            : "1920x1080");
        SetKeyValue(ini, "display_mode", "fullscreen");
        SetKeyValue(ini, "engine_fps", "144");
        SetKeyValue(ini, "vsync", "1");
        SetKeyValue(ini, "async_shaders", "1");
        SetKeyValue(ini, "deferred_readback", "1");
        args.emplace_back("--play");
    } else if (engine.kind == "shadps4-release") {
        const auto choice = ChooseResolution(title.empty() ? serial : title,
                                             ExternalEngineResolution(serial));
        if (!choice) {
            LOG_INFO(Common, "{}: the player went back before playing", serial);
            return true;
        }
        SetKeyValue(program_folder / "shadps4-flags-user.txt", "SHADPS4_RENDER_HEIGHT",
                    *choice == "4K" ? "2160" : *choice == "1440p" ? "1440" : "1080");
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
    } else if (engine.kind == "uncharted") {
        const auto settings = AppDataFile("Uncharted Launcher/launcher.json");
        if (settings.empty() || !SetGameDir(settings, eboot)) {
            LOG_ERROR(Common, "Could not write the Uncharted launcher's settings");
            return false;
        }
        // Three games in one: first the game, then its resolution, the rest at the port's best
        // (its "ultra" preset), then straight in (--play u1|u2|u3).
        auto config = ReadJsonFile(settings);
        const std::array<const char*, 3> keys{"u1", "u2", "u3"};
        int game = 0;
        for (int i = 0; i < 3; ++i) {
            if (config.value("game", std::string{"u1"}) == keys[i]) {
                game = i;
            }
        }
        const std::string name = title.empty() ? serial : title;
        while (true) {
            const auto picked = ChooseOption(name, "Elige el juego",
                                             {"Uncharted: El Tesoro de Drake",
                                              "Uncharted 2: El Reino de los Ladrones",
                                              "Uncharted 3: La Traicion de Drake"},
                                             game);
            if (!picked) {
                LOG_INFO(Common, "{}: the player went back before playing", serial);
                return true;
            }
            game = *picked;
            config["game"] = keys[game];
            WriteText(settings, config.dump(2));
            const auto choice = ChooseResolution(name, ExternalEngineResolution(serial));
            if (!choice) {
                continue; // back to the game list
            }
            const std::string res = *choice == "4K"      ? "3840x2160"
                                    : *choice == "1440p" ? "2560x1440"
                                                         : "1920x1080";
            config = ReadJsonFile(settings);
            config["games"][keys[game]]["resolution"] = res;
            config["games"][keys[game]]["preset"] = "ultra";
            WriteText(settings, config.dump(2));
            // The rest lives in the game's own ini beside the launcher.
            const auto ini = program_folder / "analysis" / "uncharted-user" /
                             (std::string{"probe-"} + keys[game] + ".ini");
            SetKeyValue(ini, "display_mode", "fullscreen");
            SetKeyValue(ini, "fps_limit", "120");
            SetKeyValue(ini, "frames_queued", "1");
            SetKeyValue(ini, "vsync", "1");
            SetKeyValue(ini, "async_shaders", "1");
            SetKeyValue(ini, "rcas", "1");
            SetKeyValue(ini, "rcas_strength", "50");
            // Below 4K the picture is scaled to the screen: FSR 1 keeps it sharp.
            SetKeyValue(ini, "fsr1", *choice == "4K" ? "0" : "1");
            break;
        }
        args = {"--play", keys[game]};
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
