//  SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/file_sys/ifile.h"
#include "emulator.h"
#include "imgui/big_picture/big_picture.h"
#include "imgui/big_picture/imgui_impl_sdl3_big_picture.h"
#include "imgui/big_picture/imgui_impl_sdlrenderer3.h"
#include "imgui/big_picture/settings_dialog_imgui.h"
#include "imgui/big_picture/theme.h"
#include "imgui/big_picture/translation.h"
#include "imgui/imgui_std.h"
#include "imgui/renderer/font_data.h"
#include "imgui/renderer/font_stack.h"
#include "sdl_window.h"

namespace BigPictureMode {

constexpr float gameImageSize = 260.f;

bool done = false;
bool showSettings = false;

std::filesystem::path runEbootPath = "";
std::string runSerial = "";
std::vector<IconInfo> gameIcons = {};

float uiScale = 1.0f;
SDL_Renderer* renderer;

namespace {

std::filesystem::path UpdateChecker(const std::string sceItem, std::filesystem::path game_folder) {
    std::filesystem::path updatedPath = "";
    std::filesystem::path basePath = game_folder.parent_path();
    std::string fileName;
    std::string item = "sce_sys/" + sceItem;

    if (Core::FileSys::IsZArchiveFile(game_folder)) {
        fileName = Core::FileSys::StripZArchiveExtension(game_folder).filename().string();
    } else {
        fileName = game_folder.filename().string();
    }

    if (std::filesystem::exists(basePath / (fileName + "-UPDATE") / item)) {
        updatedPath = basePath / (fileName + "-UPDATE") / item;
    } else if (Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-UPDATE.zar"), item)
                   .has_value()) {
        updatedPath =
            Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-UPDATE.zar"), item).value();
    } else if (std::filesystem::exists(basePath / (fileName + "-patch") / item)) {
        updatedPath = basePath / (fileName + "-patch") / item;
    } else if (Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-patch.zar"), item)
                   .has_value()) {
        updatedPath =
            Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-patch.zar"), item).value();
    } else if (Core::FileSys::ResolveGameFilePath(game_folder, item).has_value()) {
        updatedPath = Core::FileSys::ResolveGameFilePath(game_folder, item).value();
    }

    return updatedPath;
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
        return {};
    }
    // A damaged file only means that there is nothing to show from it.
    return nlohmann::json::parse(in, nullptr, false);
}

/// The settings a game will start with are read from the files, not from the loaded settings:
/// those hold the profile of one game at most.
template <typename T>
T StoredSetting(const nlohmann::json& game, const nlohmann::json& global, const char* group,
                const char* key, T fallback) {
    for (const auto* file : {&game, &global}) {
        if (file->is_object() && file->contains(group) && (*file)[group].contains(key)) {
            const auto& value = (*file)[group][key];
            if (!value.is_null()) {
                return value.get<T>();
            }
        }
    }
    return fallback;
}

const char* ConsoleLanguageName(int id) {
    static constexpr std::array<const char*, 31> names = {
        "Japanese",
        "English (United States)",
        "French (France)",
        "Spanish (Spain)",
        "German",
        "Italian",
        "Dutch",
        "Portuguese (Portugal)",
        "Russian",
        "Korean",
        "Traditional Chinese",
        "Simplified Chinese",
        "Finnish",
        "Swedish",
        "Danish",
        "Norwegian (Bokmaal)",
        "Polish",
        "Portuguese (Brazil)",
        "English (United Kingdom)",
        "Turkish",
        "Spanish (Latin America)",
        "Arabic",
        "French (Canada)",
        "Czech",
        "Hungarian",
        "Greek",
        "Romanian",
        "Thai",
        "Vietnamese",
        "Indonesian",
        "Ukrainian",
    };
    return id >= 0 && id < static_cast<int>(names.size()) ? Tr(names[id]) : "";
}

/// Fills in what each card says under the name of its game.
void FillGameDetails(std::vector<IconInfo>& icons) {
    using namespace Common::FS;
    const auto global = ReadJson(GetUserPath(PathType::UserDir) / "config.json");

    // Full screen shows the game at the size of the display.
    int display_width = 0;
    int display_height = 0;
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        display_width = static_cast<int>(mode->w * mode->pixel_density);
        display_height = static_cast<int>(mode->h * mode->pixel_density);
    }

    // A window is kept inside the usable area of the display, as the emulator does.
    SDL_Rect usable{};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);

    for (auto& icon : icons) {
        const auto game = ReadJson(GetUserPath(PathType::CustomConfigs) / (icon.serial + ".json"));
        try {
            const bool full_screen =
                StoredSetting(game, global, "GPU", "full_screen", false) &&
                StoredSetting<std::string>(game, global, "GPU", "full_screen_mode", "Windowed") !=
                    "Windowed";
            const int width = full_screen
                                  ? display_width
                                  : StoredSetting(game, global, "GPU", "window_width", 1280);
            const int height = full_screen
                                   ? display_height
                                   : StoredSetting(game, global, "GPU", "window_height", 720);
            int out_width = width;
            int out_height = height;
            if (!full_screen && usable.w > 0 && usable.h > 0 && width > 0 && height > 0) {
                const float fit =
                    std::min({1.0f, usable.w * 0.9f / width, usable.h * 0.9f / height});
                out_width = static_cast<int>(width * fit);
                out_height = static_cast<int>(height * fit);
            }
            // The game renders at a resolution of its own, which only a patch changes. A
            // resolution patch says the resolution in its name.
            using ImGuiEmuSettings::SettingsWindow;
            const std::string patch = StoredSetting(game, global, "GPU", "use_game_patch", true)
                                          ? SettingsWindow::GamePatchName(icon.serial)
                                          : std::string{};
            static const std::regex size{R"((\d{3,4})\s*[xX]\s*(\d{3,4}))"};
            if (std::smatch found; std::regex_search(patch, found, size)) {
                icon.details[0] =
                    fmt::format("{}{} x {}", Tr("Resolution: "), found[1].str(), found[2].str());
            } else {
                icon.details[0] =
                    fmt::format("{}{}", Tr("Resolution: "),
                                Tr(patch.empty() ? "original of the game" : "patched"));
            }
            // The resolution above is the detail the picture has. In full screen it is
            // stretched over the display, whose size says nothing about the picture, so only
            // a window is given a size.
            icon.details[1] =
                full_screen ? std::string{Tr("Full screen")}
                            : fmt::format("{} {} x {}", Tr("Windowed"), out_width, out_height);
            icon.details[2] = Tr(StoredSetting(game, global, "GPU", "fsr_enabled", false)
                                     ? "Enlarged with FSR"
                                     : "Enlarged without FSR");
            icon.details[3] = Tr(StoredSetting(game, global, "GPU", "frame_generation", false)
                                     ? "Frame generation: on"
                                     : "Frame generation: off");
            icon.details[4] =
                ConsoleLanguageName(StoredSetting(game, global, "General", "console_language", 1));
        } catch (const nlohmann::json::exception&) {
            icon.details[0].clear();
            icon.details[1].clear();
            icon.details[2].clear();
            icon.details[3].clear();
            icon.details[4].clear();
        }
        icon.details[5] = Tr("Not played yet");
        icon.details[6].clear();
    }

    // One line per game: its serial, the time played as h:mm:ss and when it was last played.
    std::ifstream play_times{GetUserPath(PathType::UserDir) / "play_time.txt"};
    std::string line;
    while (std::getline(play_times, line)) {
        // A line that cannot be read is skipped, the rest of the file still counts.
        std::istringstream fields{line};
        std::string serial;
        std::string played;
        std::time_t last{};
        if (!(fields >> serial >> played >> last)) {
            continue;
        }
        const auto icon = std::ranges::find(icons, serial, &IconInfo::serial);
        int hours = 0;
        int minutes = 0;
        if (icon == icons.end() || std::sscanf(played.c_str(), "%d:%d", &hours, &minutes) != 2) {
            continue;
        }
        icon->details[5] = hours > 0 ? fmt::format("{}{} h {} min", Tr("Played: "), hours, minutes)
                                     : fmt::format("{}{} min", Tr("Played: "), minutes);
        if (const std::tm* when = std::localtime(&last)) {
            char date[32]{};
            std::strftime(date, sizeof(date), Tr("%Y-%m-%d"), when);
            icon->details[6] = fmt::format("{}{}", Tr("Last played: "), date);
        }
    }
}

/// Draws the game grid: one card per game with its cover, its name and its two actions. Sets
/// `settingsFor` to the game whose settings button was pressed.
void SetGameIcons(std::vector<IconInfo>& gameIcons, int& settingsFor, int& suggestFor) {
    const float maxAvailableWidth = ImGui::GetContentRegionAvail().x;
    const float pad = 18.0f * uiScale;
    const float gap = 22.0f * uiScale;
    const float cover = gameImageSize * uiScale;
    const float line = ImGui::GetTextLineHeight();
    const float frame = ImGui::GetFrameHeight();
    // Seven smaller lines under the name say how the game is set up and how much it was played.
    constexpr float details_scale = 0.74f;
    const float details_line = line * details_scale + 2.0f * uiScale;
    const float details_height = details_line * 7.0f + 16.0f * uiScale;
    const ImVec2 card{cover + pad * 2.0f, pad + cover + 12.0f * uiScale + line * 2.0f +
                                              details_height + 12.0f * uiScale + frame + pad};
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float rowWidth = 0.0f;

    for (int i = 0; i < gameIcons.size(); i++) {
        if (i > 0) {
            // Use same line if the next card fits, move to the next row if not
            if (rowWidth + gap + card.x <= maxAvailableWidth) {
                ImGui::SameLine(0.0f, gap);
            } else {
                ImGui::Dummy(ImVec2(0.0f, gap - ImGui::GetStyle().ItemSpacing.y));
                rowWidth = 0.0f;
            }
        }
        rowWidth += (rowWidth > 0.0f ? gap : 0.0f) + card.x;

        ImGui::PushID(i);
        ImGui::BeginGroup();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 cover_min{pos.x + pad, pos.y + pad};
        const ImVec2 cover_max{cover_min.x + cover, cover_min.y + cover};

        // The cover starts the game.
        ImGui::SetCursorScreenPos(cover_min);
        // The recommended settings button sits on top of the cover and has to get its clicks.
        ImGui::SetNextItemAllowOverlap();
        bool play = ImGui::InvisibleButton("cover", ImVec2(cover, cover));
        const bool cover_hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (ImGui::IsItemFocused() && !gameIcons[i].focusState) {
            ImGui::SetScrollHereY(0.5f);
        }
        if (ImGui::IsWindowFocused()) {
            gameIcons[i].focusState = ImGui::IsItemFocused();
        }

        draw_list->AddRectFilled(
            pos, {pos.x + card.x, pos.y + card.y},
            ImGui::GetColorU32(cover_hovered ? Theme::CardHovered : Theme::Card), 28.0f * uiScale);
        if (ImTextureID id = gameIcons[i].textureId; id != nullptr) {
            draw_list->AddImageRounded(id, cover_min, cover_max, {0.0f, 0.0f}, {1.0f, 1.0f},
                                       IM_COL32_WHITE, 18.0f * uiScale);
        } else {
            draw_list->AddRectFilled(cover_min, cover_max, ImGui::GetColorU32(Theme::Chip),
                                     16.0f * uiScale);
        }

        // A game with known good settings that has none of its own yet offers them right here.
        using ImGuiEmuSettings::SettingsWindow;
        if (SettingsWindow::HasSuggestedFor(gameIcons[i].serial) &&
            !SettingsWindow::HasOwnSettings(gameIcons[i].serial)) {
            ImGui::SetCursorScreenPos({cover_min.x + 8.0f * uiScale, cover_min.y + 8.0f * uiScale});
            ImGui::SetWindowFontScale(uiScale * 0.72f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(10.0f * uiScale, 5.0f * uiScale));
            if (Theme::AccentButton(Tr("Use recommended settings##suggest"))) {
                suggestFor = i;
            }
            ImGui::PopStyleVar();
            ImGui::SetWindowFontScale(uiScale);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(Tr("Settings tested with this game. You can change them later."));
            }
        }

        // Two lines for the name, cut off if it is longer.
        const ImVec2 title_min{cover_min.x, cover_max.y + 12.0f * uiScale};
        ImGui::SetCursorScreenPos(title_min);
        ImGui::PushClipRect(title_min, {cover_max.x, title_min.y + line * 2.0f}, true);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cover);
        ImGui::TextWrapped("%s", gameIcons[i].title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopClipRect();

        const float details_top = title_min.y + line * 2.0f + 8.0f * uiScale;
        ImGui::PushClipRect({cover_min.x, details_top},
                            {cover_max.x, details_top + details_line * 7.0f}, true);
        ImGui::SetWindowFontScale(uiScale * details_scale);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
        for (int row = 0; row < 7; ++row) {
            ImGui::SetCursorScreenPos({cover_min.x, details_top + details_line * row});
            ImGui::TextUnformatted(gameIcons[i].details[row].c_str());
        }
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(uiScale);
        ImGui::PopClipRect();

        ImGui::SetCursorScreenPos({cover_min.x, pos.y + card.y - pad - frame});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(16.0f * uiScale, ImGui::GetStyle().FramePadding.y));
        play |= Theme::AccentButton(Tr("Play"));
        ImGui::SameLine(0.0f, 8.0f * uiScale);
        if (ImGui::Button(Tr("Settings"))) {
            settingsFor = i;
        }
        ImGui::PopStyleVar();

        if (play) {
            done = true;
            Core::FileSys::MntPoints::ignore_game_patches =
                ImGui::IsKeyDown(ImGuiKey::ImGuiKey_LeftCtrl);
            runEbootPath = gameIcons[i].ebootPath;
            runSerial = gameIcons[i].serial;
        }

        // Make the group as large as the card, whatever was drawn inside.
        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy(card);
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

} // namespace

SDL_Texture* LoadSdlTextureData(std::vector<u8> data) {
    int image_width = 0;
    int image_height = 0;
    int channels = 4;
    unsigned char* image_data = stbi_load_from_memory(
        (const unsigned char*)data.data(), (int)data.size(), &image_width, &image_height, NULL, 4);
    if (image_data == nullptr) {
        LOG_ERROR(ImGui, "Failed to load image: {}", stbi_failure_reason());
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(image_width, image_height, SDL_PIXELFORMAT_RGBA32,
                                                 (void*)image_data, channels * image_width);
    if (surface == nullptr) {
        LOG_ERROR(ImGui, "Unable to create SDL surface: {}", SDL_GetError());
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == nullptr) {
        LOG_ERROR(ImGui, "Unable to create SDL texture: {}", SDL_GetError());
    }

    SDL_DestroySurface(surface);
    stbi_image_free(image_data);

    return texture;
}

SDL_Texture* LoadSdlTextureDataFromFile(std::filesystem::path filePath) {
    std::ifstream file(filePath, std::ios::binary);
    std::vector<u8> data =
        std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return LoadSdlTextureData(data);
}

void GetGameIconInfo(std::vector<IconInfo>& icons) {
    icons.clear();

    for (const auto& installLoc : EmulatorSettings.GetAllGameInstallDirs()) {
        if (installLoc.enabled && std::filesystem::exists(installLoc.path)) {
            for (const auto& entry : std::filesystem::directory_iterator(installLoc.path)) {

                std::string pathstring = entry.path().filename().string();
                if (pathstring.ends_with("-UPDATE") || pathstring.ends_with("-patch") ||
                    (!entry.is_directory() && !Core::FileSys::IsZArchiveFile(entry))) {
                    continue;
                }

                if (Core::FileSys::IsZArchiveFile(entry)) {
                    size_t start = pathstring.length() - 3;
                    for (size_t i = start; i < pathstring.length(); ++i) {
                        pathstring[i] = static_cast<char>(
                            std::tolower(static_cast<unsigned char>(pathstring[i])));
                    }

                    if (pathstring.ends_with("-UPDATE.zar") || pathstring.ends_with("-patch.zar")) {
                        continue;
                    }
                }

                IconInfo icon;
                PSF psf;
                const std::string sfoFileName = "param.sfo";
                std::filesystem::path sfoPath = UpdateChecker(sfoFileName, entry.path());

                if (std::filesystem::exists(sfoPath) && psf.Open(sfoPath)) {
                    if (const auto title = psf.GetString("TITLE"); title.has_value()) {
                        icon.title = *title;
                    }

                    if (const auto title_id = psf.GetString("TITLE_ID"); title_id.has_value()) {
                        icon.serial = *title_id;
                    }

                    // Additional content (DLC) comes with a param.sfo of its own, but it is
                    // not a game to list.
                    if (const auto category = psf.GetString("CATEGORY");
                        category.has_value() && category->starts_with("ac")) {
                        continue;
                    }
                } else {
                    continue;
                }

                const std::string iconFileName = "icon0.png";
                std::filesystem::path iconPath = UpdateChecker(iconFileName, entry.path());

                SDL_Texture* texture = LoadSdlTextureDataFromFile(iconPath);
                icon.textureId = ImTextureID(texture);

                icon.ebootPath = entry.path() / "eboot.bin";
                if (Core::FileSys::IsZArchiveFile(entry.path())) {
                    icon.ebootPath = entry.path();
                }

                icon.focusState = false;
                icons.push_back(icon);
            }
        }
    }

    std::sort(icons.begin(), icons.end(), [](const IconInfo& a, const IconInfo& b) {
        return a.title < b.title; // Alphabetical order
    });
    FillGameDetails(icons);
}

void Launch(char* executableName, bool sameProcess) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR(ImGui, "SDL_INIT_VIDEO Error: {}", SDL_GetError());
        SDL_Quit();
        return;
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOG_ERROR(ImGui, "SDL_INIT_GAMEPAD Error: {}", SDL_GetError());
    }

    SDL_Window* window = SDL_CreateWindow("brunoShadPs4", 1280, 800, SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        LOG_ERROR(ImGui, "SDL Window Creation Error: {}", SDL_GetError());
        SDL_Quit();
        return;
    }

    Frontend::SetDefaultWindowIcon(window);
    renderer = SDL_CreateRenderer(window, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAlways = true;

    ImFontConfig cfgBase;
    cfgBase.OversampleH = 2;
    cfgBase.OversampleV = 1;

    // Poppins for everything it covers, with the usual fonts merged in behind it for the rest.
    static const ImWchar latin_ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0};
    io.FontDefault = io.Fonts->AddFontFromMemoryCompressedTTF(
        imgui_font_poppins_medium_compressed_data, imgui_font_poppins_medium_compressed_size, 64.0f,
        &cfgBase, latin_ranges);
    ImFontConfig cfgFallback = cfgBase;
    cfgFallback.MergeMode = true;
    ImGui::FontStack::AddPrimaryUiFont(io.Fonts, 64.0f, EmulatorSettings.GetConsoleLanguage(),
                                       cfgFallback, true);
    io.FontGlobalScale = 0.5f;
    // size the big picture font atlas cap from the renderer limit
    const auto max_dim = SDL_GetNumberProperty(SDL_GetRendererProperties(renderer),
                                               SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 8192);
    const int atlas_max = static_cast<int>(std::bit_floor(std::max<u64>(max_dim, 512)));
    io.Fonts->TexMaxWidth = atlas_max;
    io.Fonts->TexMaxHeight = atlas_max;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    ImGuiEmuSettings::SettingsWindow settingsWindow(false);

    float sliderScale = 1.0f;
    auto applySettings = [&] {
        uiScale = std::clamp(EmulatorSettings.GetBigPictureScale() / 1000.f, 0.75f, 1.5f);
        sliderScale = uiScale;
        GetGameIconInfo(gameIcons);
        // The launcher always stays in a window; the full screen setting is for the games.
    };
    applySettings();

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        Theme::Push(uiScale);

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::Begin("Game Window", &done, window_flags);
        ImGui::SetWindowFontScale(uiScale);

        ImGuiChildFlags child_flags = ImGuiChildFlags_NavFlattened;

        ImGuiWindowFlags child_window_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetNextWindowFocus();
        }

        // Top bar: the name on the left, the two actions on the right.
        ImGui::SetWindowFontScale(uiScale * 1.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Tr("brunoShadPs4"));
        ImGui::SetWindowFontScale(uiScale);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", fmt::format("  {}", Common::g_version).c_str());
        ImGui::SameLine();
        {
            const float width =
                ImGui::CalcTextSize(Tr("Add games")).x + ImGui::CalcTextSize(Tr("Settings")).x +
                ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
        }
        if (ImGui::Button(Tr("Add games"))) {
            ImGuiEmuSettings::SettingsWindow::RequestGamesFolder();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Settings"))) {
            EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
            EmulatorSettings.Save();
            settingsWindow.Prepare();
            showSettings = true;
        }

        ImGui::Dummy(ImVec2(0.0f, 8.f * uiScale));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                          child_flags, child_window_flags);
        ImGui::PopStyleColor();

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        int settingsFor = -1;
        int suggestFor = -1;
        SetGameIcons(gameIcons, settingsFor, suggestFor);
        if (suggestFor >= 0) {
            settingsWindow.ApplySuggestedTo(gameIcons[suggestFor].serial,
                                            gameIcons[suggestFor].title);
        }
        // A folder picked in the system's dialog arrives here.
        if (settingsWindow.ConsumeGamesFolder()) {
            GetGameIconInfo(gameIcons);
        }
        // Without a system picker, adding a folder goes through the folders page and its
        // built-in one.
        if (ImGuiEmuSettings::SettingsWindow::BuiltinPickerPending() && !showSettings) {
            settingsWindow.Prepare();
            settingsWindow.OpenFolders();
            showSettings = true;
        }
        if (settingsFor >= 0) {
            settingsWindow.Prepare();
            settingsWindow.OpenProfile(gameIcons[settingsFor].serial, gameIcons[settingsFor].title);
            showSettings = true;
        }
        if (gameIcons.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 40.f * uiScale));
            Overlay::TextCentered(Tr("No games yet. Add the folder that contains your games."));
            ImGui::Dummy(ImVec2(0.0f, 10.f * uiScale));
            const char* label = Tr("Add games folder");
            const float width =
                ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            if (Theme::AccentButton(label)) {
                ImGuiEmuSettings::SettingsWindow::RequestGamesFolder();
            }
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(150.0f * uiScale);
        if (ImGui::IsWindowAppearing()) {
            sliderScale = uiScale;
        }
        ImGui::SliderFloat("##scale", &sliderScale, 0.75f, 1.5f, "");
        // Only apply the size once the slider is released, so it does not move under the mouse.
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            uiScale = sliderScale;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(Tr("Size"));

        // Whose work this is.
        {
            const char* credit = Tr("A fork of shadPS4");
            const float width = ImGui::CalcTextSize(credit).x + ImGui::CalcTextSize(Tr("About")).x +
                                ImGui::GetStyle().FramePadding.x * 2.0f +
                                ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", credit);
            ImGui::SameLine();
            if (ImGui::Button(Tr("About"))) {
                ImGui::OpenPopup(Tr("About brunoShadPs4"));
            }
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f * uiScale, 28.0f * uiScale));
        if (ImGui::BeginPopupModal(Tr("About brunoShadPs4"), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoTitleBar)) {
            ImGui::SetWindowFontScale(uiScale * 1.4f);
            ImGui::TextUnformatted(Tr("brunoShadPs4"));
            ImGui::SetWindowFontScale(uiScale);
            ImGui::TextDisabled("%s", fmt::format("{} {}  ({})", Tr("Version"), Common::g_version,
                                                  Common::g_scm_desc)
                                          .c_str());
            ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
            ImGui::TextUnformatted(Tr("A fork of shadPS4, the PlayStation 4 emulator."));
            ImGui::TextUnformatted(
                Tr("All the credit for the emulator goes to the shadPS4 Emulator"));
            ImGui::TextUnformatted(Tr("Project and its contributors."));
            ImGui::TextDisabled(Tr("github.com/shadps4-emu/shadPS4  -  GPL-2.0-or-later"));
            ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
            ImGui::TextUnformatted(Tr("This fork adds"));
            ImGui::TextDisabled(Tr("Frame generation with AMD FSR 3 (FidelityFX SDK, MIT)"));
            ImGui::TextDisabled(
                Tr("Fixes and speed-ups for God of War III and inFamous Second Son"));
            ImGui::TextDisabled(Tr("This launcher, set in Poppins (OFL)"));
            ImGui::TextDisabled(Tr("github.com/serbru20066666/brunoShadPs4"));
            ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
            ImGui::TextUnformatted(Tr("Contact"));
            ImGui::TextDisabled("Bruno Cardenas");
            ImGui::TextDisabled("brunocardenasproyectos@gmail.com");
            ImGui::TextDisabled("+51 987 970 898");
            ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
            if (Theme::AccentButton(Tr("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        if (showSettings) {
            settingsWindow.DrawSettings(&showSettings, applySettings);
        }

        Theme::Pop();
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 9, 9, 11, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
    SDL_DestroyWindow(window);
    SDL_Quit();

    EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
    EmulatorSettings.Save();

    if (runEbootPath != "") {
        if (sameProcess) {
            auto* emulator = Common::Singleton<Core::Emulator>::Instance();
            emulator->executableName = executableName;
            emulator->Run(runEbootPath);
        } else {
            std::vector<std::string> args{"--log-append", "--game",
                                          Common::FS::PathToUTF8String(runEbootPath)};
            // A patch file named after the game in the patches folder is applied with it.
            const auto patch =
                Common::FS::GetUserPath(Common::FS::PathType::PatchesDir) / (runSerial + ".xml");
            // Unless the settings of the game say to leave it as it is.
            const auto own = ReadJson(Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
                                      (runSerial + ".json"));
            bool use_patch = true;
            try {
                use_patch = StoredSetting(own, nlohmann::json{}, "GPU", "use_game_patch", true);
            } catch (const nlohmann::json::exception&) {
            }
            if (!runSerial.empty() && use_patch && std::filesystem::exists(patch)) {
                args.insert(args.end(), {"--patch", Common::FS::PathToUTF8String(patch)});
            }
            // Spawning the new process directly, without constructing an Emulator here, avoids
            // registering its Shutdown() as an at_quick_exit handler: that handler assumes a
            // previous Run() and can hold up this process's exit for no reason, which on macOS
            // left this window appearing frozen while the next process compiled its first
            // shaders and crowded this one off the CPU.
            Core::RelaunchProcess(executableName, std::move(args));
        }
    }
}

} // namespace BigPictureMode
