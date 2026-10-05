//  SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <fstream>
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
#include "imgui/imgui_std.h"
#include "imgui/renderer/font_data.h"
#include "imgui/renderer/font_stack.h"
#include "sdl_window.h"

namespace BigPictureMode {

constexpr float gameImageSize = 260.f;

bool done = false;
bool showSettings = false;

std::filesystem::path runEbootPath = "";
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

/// Draws the game grid: one card per game with its cover, its name and its two actions. Sets
/// `settingsFor` to the game whose settings button was pressed.
void SetGameIcons(std::vector<IconInfo>& gameIcons, int& settingsFor) {
    const float maxAvailableWidth = ImGui::GetContentRegionAvail().x;
    const float pad = 18.0f * uiScale;
    const float gap = 22.0f * uiScale;
    const float cover = gameImageSize * uiScale;
    const float line = ImGui::GetTextLineHeight();
    const float frame = ImGui::GetFrameHeight();
    const ImVec2 card{cover + pad * 2.0f,
                      pad + cover + 12.0f * uiScale + line * 2.0f + 12.0f * uiScale + frame + pad};
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

        // Two lines for the name, cut off if it is longer.
        const ImVec2 title_min{cover_min.x, cover_max.y + 12.0f * uiScale};
        ImGui::SetCursorScreenPos(title_min);
        ImGui::PushClipRect(title_min, {cover_max.x, title_min.y + line * 2.0f}, true);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cover);
        ImGui::TextWrapped("%s", gameIcons[i].title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopClipRect();

        ImGui::SetCursorScreenPos({cover_min.x, pos.y + card.y - pad - frame});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(16.0f * uiScale, ImGui::GetStyle().FramePadding.y));
        play |= Theme::AccentButton("Play");
        ImGui::SameLine(0.0f, 8.0f * uiScale);
        if (ImGui::Button("Settings")) {
            settingsFor = i;
        }
        ImGui::PopStyleVar();

        if (play) {
            done = true;
            Core::FileSys::MntPoints::ignore_game_patches =
                ImGui::IsKeyDown(ImGuiKey::ImGuiKey_LeftCtrl);
            runEbootPath = gameIcons[i].ebootPath;
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
        uiScale = EmulatorSettings.GetBigPictureScale() / 1000.f;
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
        ImGui::TextUnformatted("brunoShadPs4");
        ImGui::SetWindowFontScale(uiScale);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", fmt::format("  {}", Common::g_version).c_str());
        ImGui::SameLine();
        {
            const float width =
                ImGui::CalcTextSize("Add games").x + ImGui::CalcTextSize("Settings").x +
                ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
        }
        if (ImGui::Button("Add games")) {
            settingsWindow.Prepare();
            settingsWindow.OpenFolders();
            showSettings = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Settings")) {
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
        SetGameIcons(gameIcons, settingsFor);
        if (settingsFor >= 0) {
            settingsWindow.Prepare();
            settingsWindow.OpenProfile(gameIcons[settingsFor].serial, gameIcons[settingsFor].title);
            showSettings = true;
        }
        if (gameIcons.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 40.f * uiScale));
            Overlay::TextCentered("No games yet. Add the folder that contains your games.");
            ImGui::Dummy(ImVec2(0.0f, 10.f * uiScale));
            const char* label = "Add games folder";
            const float width =
                ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            if (Theme::AccentButton(label)) {
                settingsWindow.Prepare();
                settingsWindow.OpenFolders();
                showSettings = true;
            }
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(150.0f * uiScale);
        if (ImGui::IsWindowAppearing()) {
            sliderScale = uiScale;
        }
        ImGui::SliderFloat("##scale", &sliderScale, 0.5f, 2.5f, "");
        // Only apply the size once the slider is released, so it does not move under the mouse.
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            uiScale = sliderScale;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Size");

        // Whose work this is.
        {
            const char* credit = "A fork of shadPS4";
            const float width = ImGui::CalcTextSize(credit).x + ImGui::CalcTextSize("About").x +
                                ImGui::GetStyle().FramePadding.x * 2.0f +
                                ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", credit);
            ImGui::SameLine();
            if (ImGui::Button("About")) {
                ImGui::OpenPopup("About brunoShadPs4");
            }
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f * uiScale, 28.0f * uiScale));
        if (ImGui::BeginPopupModal("About brunoShadPs4", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoTitleBar)) {
            ImGui::SetWindowFontScale(uiScale * 1.4f);
            ImGui::TextUnformatted("brunoShadPs4");
            ImGui::SetWindowFontScale(uiScale);
            ImGui::TextDisabled(
                "%s",
                fmt::format("Version {}  ({})", Common::g_version, Common::g_scm_desc).c_str());
            ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
            ImGui::TextUnformatted("A fork of shadPS4, the PlayStation 4 emulator.");
            ImGui::TextUnformatted("All the credit for the emulator goes to the shadPS4 Emulator");
            ImGui::TextUnformatted("Project and its contributors.");
            ImGui::TextDisabled("github.com/shadps4-emu/shadPS4  -  GPL-2.0-or-later");
            ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));
            ImGui::TextUnformatted("This fork adds");
            ImGui::TextDisabled("Frame generation with AMD FSR 3 (FidelityFX SDK, MIT)");
            ImGui::TextDisabled("Fixes and speed-ups for God of War III and inFamous Second Son");
            ImGui::TextDisabled("This launcher, set in Poppins (OFL)");
            ImGui::TextDisabled("github.com/serbru20066666/brunoShadPs4");
            ImGui::Dummy(ImVec2(0.0f, 8.0f * uiScale));
            if (Theme::AccentButton("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
        auto* emulator = Common::Singleton<Core::Emulator>::Instance();
        emulator->executableName = executableName;
        if (sameProcess) {
            emulator->Run(runEbootPath);
        } else {
            emulator->Relaunch(
                {"--log-append", "--game", Common::FS::PathToUTF8String(runEbootPath)});
        }
    }
}

} // namespace BigPictureMode
