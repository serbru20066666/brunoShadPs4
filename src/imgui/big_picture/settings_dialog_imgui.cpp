//  SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <ImGuiFileDialog.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_keyboard.h>
#include <cmrc/cmrc.hpp>
#include <stb_image.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/cpu_patches.h"
#include "core/devtools/layer.h"
#include "imgui/imgui_std.h"
#include "settings_dialog_imgui.h"
#include "theme.h"

CMRC_DECLARE(res);

constexpr float gameImageSize = 200.f;

namespace ImGuiEmuSettings {

int SettingsWindow::GetComboIndex(std::string selection, std::vector<std::string> options) {
    for (int i = 0; i < options.size(); i++) {
        if (selection == options[i])
            return i;
    }

    return 0;
}

void SettingsWindow::LoadSettings(std::string profile) {
    const bool isSpecific = currentProfile != "Global";
    isSpecific ? EmulatorSettings.Load(profile) : EmulatorSettings.Load();

    /////////// General Tab
    int languageIndex = EmulatorSettings.GetConsoleLanguage();
    std::string language;
    for (const auto& [key, value] : languageMap) {
        if (value == languageIndex) {
            language = key;
        }
    }

    consoleLanguageSetting = GetComboIndex(language, languageOptions);
    volumeSetting = EmulatorSettings.GetVolumeSlider();
    showSplashSetting = EmulatorSettings.IsShowSplash();
    audioBackendSetting = EmulatorSettings.GetAudioBackend();

    /////////// Graphics Tab
    fullscreenModeSetting =
        GetComboIndex(EmulatorSettings.GetFullScreenMode(), fullscreenModeOptions);
    presentModeSetting = GetComboIndex(EmulatorSettings.GetPresentMode(), presentModeOptions);
    windowHeightSetting = EmulatorSettings.GetWindowHeight();
    windowWidthSetting = EmulatorSettings.GetWindowWidth();
    hdrAllowedSetting = EmulatorSettings.IsHdrAllowed();
    fsrEnabledSetting = EmulatorSettings.IsFsrEnabled();
    frameGenerationSetting = EmulatorSettings.IsFrameGenerationEnabled();
    enhanceGameQualitySetting = EmulatorSettings.IsEnhanceGameQualityEnabled();
    fxaaSetting = EmulatorSettings.IsFxaaEnabled();
    directReadbacksSetting = EmulatorSettings.IsDirectReadbacksEnabled();
    renderTargetSyncSetting = EmulatorSettings.IsRenderTargetSyncEnabled();
    rcasEnabledSetting = EmulatorSettings.IsRcasEnabled();
    rcasAttenuationSetting = static_cast<float>(EmulatorSettings.GetRcasAttenuation() * 0.001f);

    /////////// Input Tab
    motionControlsSetting = EmulatorSettings.IsMotionControlsEnabled();
    backgroundControllerSetting = EmulatorSettings.IsBackgroundControllerInput();
    cursorStateSetting = EmulatorSettings.GetCursorState();
    cursorTimeoutSetting = EmulatorSettings.GetCursorHideTimeout();

    /////////// Trophy Tab
    trophyPopupDisabledSetting = EmulatorSettings.IsTrophyPopupDisabled();
    trophySideSetting =
        GetComboIndex(EmulatorSettings.GetTrophyNotificationSide(), trophySideOptions);
    trophyDurationSetting = static_cast<float>(EmulatorSettings.GetTrophyNotificationDuration());

    /////////// Log Tab
    logEnableSetting = EmulatorSettings.IsLogEnable();
    logSeparateSetting = EmulatorSettings.IsLogSeparate();
    logSyncSetting = EmulatorSettings.IsLogSync();

    /////////// Experimental Tab
    if (isSpecific) {
        readbacksModeSetting = EmulatorSettings.GetReadbacksMode();
        readbackLinearImagesSetting = EmulatorSettings.IsReadbackLinearImagesEnabled();
        directMemoryAccessSetting = EmulatorSettings.IsDirectMemoryAccessEnabled();
        windowsGuestRedZoneProtectionModeSetting = EmulatorSettings.IsRedZonePatchingEnabled();
        devkitConsoleSetting = EmulatorSettings.IsDevKit();
        neoModeSetting = EmulatorSettings.IsNeo();
        shadnetEnabledSetting = EmulatorSettings.IsShadNetEnabledSetting();
        connectedNetworkSetting = EmulatorSettings.IsConnectedToNetwork();
        pipelineCacheEnabledSetting = EmulatorSettings.IsPipelineCacheEnabled();
        pipelineCacheArchiveSetting = EmulatorSettings.IsPipelineCacheArchived();
        extraDmemSetting = EmulatorSettings.GetExtraDmemInMBytes();
        vblankFrequencySetting = EmulatorSettings.GetVblankFrequency();
    }
}

void SettingsWindow::SaveSettings(std::string profile) {
    const bool isSpecific = currentProfile != "Global";

    /////////// General Tab
    EmulatorSettings.SetConsoleLanguage(languageMap.at(languageOptions.at(consoleLanguageSetting)),
                                        isSpecific);
    EmulatorSettings.SetVolumeSlider(volumeSetting, isSpecific);
    EmulatorSettings.SetShowSplash(showSplashSetting, isSpecific);
    EmulatorSettings.SetAudioBackend(audioBackendSetting, isSpecific);

    /////////// Graphics Tab
    bool isFullscreen = fullscreenModeSetting != 0;
    EmulatorSettings.SetFullScreen(isFullscreen);
    EmulatorSettings.SetFullScreenMode(fullscreenModeOptions.at(fullscreenModeSetting), isSpecific);
    EmulatorSettings.SetPresentMode(presentModeOptions.at(presentModeSetting), isSpecific);
    EmulatorSettings.SetWindowHeight(windowHeightSetting, isSpecific);
    EmulatorSettings.SetWindowWidth(windowWidthSetting, isSpecific);
    EmulatorSettings.SetHdrAllowed(hdrAllowedSetting, isSpecific);
    EmulatorSettings.SetFsrEnabled(fsrEnabledSetting, isSpecific);
    EmulatorSettings.SetFrameGenerationEnabled(frameGenerationSetting, isSpecific);
    EmulatorSettings.SetEnhanceGameQualityEnabled(enhanceGameQualitySetting, isSpecific);
    EmulatorSettings.SetFxaaEnabled(fxaaSetting, isSpecific);
    EmulatorSettings.SetDirectReadbacksEnabled(directReadbacksSetting, isSpecific);
    EmulatorSettings.SetRenderTargetSyncEnabled(renderTargetSyncSetting, isSpecific);
    EmulatorSettings.SetRcasEnabled(rcasEnabledSetting, isSpecific);
    EmulatorSettings.SetRcasAttenuation(static_cast<int>(rcasAttenuationSetting * 1000),
                                        isSpecific);

    /////////// Input Tab
    EmulatorSettings.SetMotionControlsEnabled(motionControlsSetting, isSpecific);
    EmulatorSettings.SetBackgroundControllerInput(backgroundControllerSetting, isSpecific);
    EmulatorSettings.SetCursorState(cursorStateSetting, isSpecific);
    EmulatorSettings.SetCursorHideTimeout(cursorTimeoutSetting, isSpecific);

    /////////// Trophy Tab
    EmulatorSettings.SetTrophyPopupDisabled(trophyPopupDisabledSetting, isSpecific);
    EmulatorSettings.SetTrophyNotificationSide(trophySideOptions.at(trophySideSetting), isSpecific);
    EmulatorSettings.SetTrophyNotificationDuration(static_cast<double>(trophyDurationSetting));

    /////////// Log Tab
    EmulatorSettings.SetLogEnable(logEnableSetting, isSpecific);
    EmulatorSettings.SetLogSeparate(logSeparateSetting, isSpecific);
    EmulatorSettings.SetLogSync(logSyncSetting, isSpecific);

    /////////// Experimental Tab
    if (isSpecific) {
        EmulatorSettings.SetReadbacksMode(readbacksModeSetting, true);
        EmulatorSettings.SetReadbackLinearImagesEnabled(readbackLinearImagesSetting, true);
        EmulatorSettings.SetDirectMemoryAccessEnabled(directMemoryAccessSetting, true);
        // Windows static guest red-zone protection
        EmulatorSettings.SetRedZonePatchingEnabled(windowsGuestRedZoneProtectionModeSetting, true);
        EmulatorSettings.SetDevKit(devkitConsoleSetting, true);
        EmulatorSettings.SetNeo(neoModeSetting, true);
        EmulatorSettings.SetShadNetEnabled(shadnetEnabledSetting, true);
        EmulatorSettings.SetConnectedToNetwork(connectedNetworkSetting, true);
        EmulatorSettings.SetPipelineCacheEnabled(pipelineCacheEnabledSetting, true);
        EmulatorSettings.SetPipelineCacheArchived(pipelineCacheArchiveSetting, true);
        EmulatorSettings.SetExtraDmemInMBytes(extraDmemSetting, true);
        EmulatorSettings.SetVblankFrequency(vblankFrequencySetting, true);
    }

    isSpecific ? EmulatorSettings.Save(profile) : EmulatorSettings.Save();
}

void SettingsWindow::SaveInstallDirs() {
    std::string profile;
    const bool isGlobal = currentProfile == "Global";
    if (!isGlobal) {
        profile = currentProfile.substr(0, 9);
        EmulatorSettings.Load();
    }

    EmulatorSettings.SetAllGameInstallDirs(m_GameInstallDirs);
    EmulatorSettings.Save();

    if (!isGlobal) {
        EmulatorSettings.Load(profile);
    }

    if (!isGameRunning) {
        GetProfileInfo();
    }
}

void SettingsWindow::GetProfileInfo() {
    GetGameIconInfo(profileIcons);

    BigPictureMode::IconInfo global;
    global.title = "Global";
    profileIcons.emplace(profileIcons.begin(), global);
}

SettingsWindow::SettingsWindow(bool gameRunning) : isGameRunning(gameRunning) {
    auto resource = cmrc::res::get_filesystem();
    auto loadTexture = [&](const std::string& resourcePath,
                           std::variant<SDL_Texture*, ImGui::RefCountedTexture>& texture) {
        auto file = resource.open(resourcePath);
        std::vector<u8> texData = std::vector<u8>(file.begin(), file.end());
        gameRunning ? texture = ImGui::RefCountedTexture::DecodePngTexture(texData)
                    : texture = BigPictureMode::LoadSdlTextureData(texData);
    };

    loadTexture("src/resources/big_picture/settings.png", generalTexture);
    loadTexture("src/resources/big_picture/experimental.png", experimentalTexture);
    loadTexture("src/resources/big_picture/graphics.png", graphicsTexture);
    loadTexture("src/resources/big_picture/controller.png", inputTexture);
    loadTexture("src/resources/big_picture/trophy.png", trophyTexture);
    loadTexture("src/resources/big_picture/log.png", logTexture);
    loadTexture("src/resources/big_picture/folder.png", foldersTexture);
    loadTexture("src/resources/big_picture/profiles.png", profilesTexture);

    auto languageKeys = std::views::keys(languageMap);
    languageOptions.assign(languageKeys.begin(), languageKeys.end());

    currentProfile = "Global";
    m_GameInstallDirs = EmulatorSettings.GetAllGameInstallDirs();
    currentCategory = isGameRunning ? SettingsCategory::General : SettingsCategory::Profiles;

    bool customConfigFound = false;
    if (isGameRunning) {
        runningGameSerial = std::string(Common::ElfInfo::Instance().GameSerial());
        std::filesystem::path customConfigFile =
            Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
            (runningGameSerial + ".json");

        if (std::filesystem::exists(customConfigFile)) {
            customConfigFound = true;
            currentProfile =
                runningGameSerial + " - " + std::string(Common::ElfInfo::Instance().Title());
        }
    } else {
        GetProfileInfo();
    }

    customConfigFound ? LoadSettings(runningGameSerial) : LoadSettings("Global");
}

namespace {

/// Settings found to work well for the games tuned with this fork, on the graphics page.
struct Suggested {
    std::string_view serial;
    int readbacks_mode;
    bool direct_readbacks;
    bool readback_linear_images;
    bool red_zone_patches;
    bool fxaa;
    bool frame_generation;
};
constexpr std::array SuggestedSettings{
    Suggested{"CUSA01623", 0, false, true, false, true, true}, // God of War III Remastered
    Suggested{"CUSA00004", 2, true, false, true, false, true}, // inFamous Second Son
};

// The folder picked in the system's dialog. Its callback can run on another thread.
std::mutex picked_folder_mutex;
std::optional<std::string> picked_folder;
// Set when the system could not show its picker (no desktop portal or helper on some Linux
// setups, for instance): the built-in one takes over from then on.
std::atomic_bool system_picker_failed{false};
// Asks the folders page to open the built-in picker.
std::atomic_bool open_builtin_picker{false};

void SDLCALL OnFolderPicked(void*, const char* const* paths, int) {
    if (paths == nullptr) {
        // An error, as opposed to the user cancelling, which gives an empty list.
        LOG_WARNING(ImGui, "The system's folder picker is not available: {}", SDL_GetError());
        system_picker_failed = true;
        open_builtin_picker = true;
        return;
    }
    if (paths[0]) {
        std::scoped_lock lock{picked_folder_mutex};
        picked_folder = paths[0];
    }
}

bool IsGameFolder(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path / "eboot.bin", ec) ||
           std::filesystem::exists(path / "sce_sys" / "param.sfo", ec);
}

const Suggested* FindSuggested(const std::string& profile) {
    const auto it = std::ranges::find_if(SuggestedSettings, [&](const Suggested& suggested) {
        return profile.starts_with(suggested.serial);
    });
    return it != SuggestedSettings.end() ? &*it : nullptr;
}

} // namespace

bool SettingsWindow::HasSuggested() const {
    return FindSuggested(currentProfile) != nullptr;
}

void SettingsWindow::ApplySuggested() {
    const Suggested* suggested = FindSuggested(currentProfile);
    if (!suggested) {
        return;
    }
    // Borderless, not exclusive: exclusive fullscreen takes over the display mode and leaves
    // the window with no way to minimize or drop back to windowed on macOS.
    fullscreenModeSetting = GetComboIndex("Fullscreen (Borderless)", fullscreenModeOptions);
    presentModeSetting = GetComboIndex("Immediate", presentModeOptions);
    fsrEnabledSetting = true;
    rcasEnabledSetting = true;
    readbacksModeSetting = suggested->readbacks_mode;
    directReadbacksSetting = suggested->direct_readbacks;
    readbackLinearImagesSetting = suggested->readback_linear_images;
    windowsGuestRedZoneProtectionModeSetting = suggested->red_zone_patches;
    fxaaSetting = suggested->fxaa;
    frameGenerationSetting = suggested->frame_generation;
    currentCategory = SettingsCategory::Graphics;
}

bool SettingsWindow::HasSuggestedFor(const std::string& serial) {
    return FindSuggested(serial) != nullptr;
}

bool SettingsWindow::HasOwnSettings(const std::string& serial) {
    std::error_code ec;
    return std::filesystem::exists(
        Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) / (serial + ".json"), ec);
}

void SettingsWindow::ApplySuggestedTo(const std::string& serial, const std::string& title) {
    currentProfile = serial + " - " + title;
    LoadSettings(serial);
    ApplySuggested();
    SaveSettings(serial);
    // Leave the dialog as it was: on the global settings.
    currentProfile = "Global";
    LoadSettings("Global");
    currentCategory = SettingsCategory::Profiles;
}

void SettingsWindow::RequestGamesFolder() {
    if (system_picker_failed) {
        open_builtin_picker = true;
        return;
    }
    SDL_ShowOpenFolderDialog(&OnFolderPicked, nullptr, SDL_GetKeyboardFocus(), nullptr, false);
}

bool SettingsWindow::BuiltinPickerPending() {
    return open_builtin_picker;
}

bool SettingsWindow::ConsumeGamesFolder() {
    std::optional<std::string> picked;
    {
        std::scoped_lock lock{picked_folder_mutex};
        picked.swap(picked_folder);
    }
    if (!picked) {
        return false;
    }
    std::filesystem::path path{std::u8string(picked->begin(), picked->end())};
    // Picking a game instead of the folder that holds the games is an easy mistake: take the
    // folder above it.
    if (IsGameFolder(path) && path.has_parent_path()) {
        path = path.parent_path();
    }
    path = std::filesystem::path{path.generic_u8string()};
    for (auto& dir : m_GameInstallDirs) {
        if (dir.path == path) {
            const bool changed = !dir.enabled;
            dir.enabled = true;
            if (changed) {
                SaveInstallDirs();
            }
            return changed;
        }
    }
    GameInstallDir dir;
    dir.path = path;
    dir.enabled = true;
    m_GameInstallDirs.push_back(dir);
    SaveInstallDirs();
    return true;
}

void SettingsWindow::OpenProfile(const std::string& serial, const std::string& title) {
    currentProfile = serial + " - " + title;
    LoadSettings(serial);
    currentCategory = SettingsCategory::Graphics;
}

void SettingsWindow::Prepare() {
    uiScale = EmulatorSettings.GetBigPictureScale() / 1000.f;
}

void SettingsWindow::DeInit() {
    EmulatorSettings.Load();
    EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
    EmulatorSettings.Save();

    if (isGameRunning && !runningGameSerial.empty()) {
        EmulatorSettings.Load(runningGameSerial);
    }
}
void SettingsWindow::DrawSettings(bool* open, const std::function<void()>& applySettings) {
    BigPictureMode::Theme::Push(uiScale);

    SetupWindow();
    DrawCategoryTabs();
    DrawMainContent(open, applySettings);

    BigPictureMode::Theme::Pop();

    ImGui::End();
}

void SettingsWindow::SetupWindow() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::Begin("Settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowFontScale(uiScale);

    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }

    SettingsCategory firstCategory =
        isGameRunning ? SettingsCategory::General : SettingsCategory::Profiles;
    SettingsCategory lastCategory =
        currentProfile != "Global" ? SettingsCategory::Experimental : SettingsCategory::Log;

    // Navigate categories with Tab / R1 / L1
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        int currentIndex = static_cast<int>(currentCategory);
        currentCategory == lastCategory
            ? currentCategory = static_cast<SettingsCategory>(firstCategory)
            : currentCategory = static_cast<SettingsCategory>(currentIndex + 1);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) {
        int currentIndex = static_cast<int>(currentCategory);
        currentIndex == static_cast<int>(firstCategory)
            ? currentCategory = lastCategory
            : currentCategory = static_cast<SettingsCategory>(currentIndex - 1);
    }
}

void SettingsWindow::DrawCategoryTabs() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    float vertSize = ImGui::GetFrameHeight() + 6.0f * uiScale;
    ImGuiChildFlags child_flags = ImGuiChildFlags_NavFlattened;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::BeginChild("Categories", ImVec2(0, vertSize), child_flags, window_flags);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * uiScale, 0.0f));

    // Must add categories in enum order for L1/R1 to work correctly
    if (!isGameRunning) {
        AddCategory("Profiles", profilesTexture, SettingsCategory::Profiles);
    }

    AddCategory("General", generalTexture, SettingsCategory::General);
    AddCategory("Graphics", graphicsTexture, SettingsCategory::Graphics);
    AddCategory("Input", inputTexture, SettingsCategory::Input);
    AddCategory("Trophy", trophyTexture, SettingsCategory::Trophy);
    AddCategory("Game Folders", foldersTexture, SettingsCategory::Folders);
    AddCategory("Log", logTexture, SettingsCategory::Log);

    if (currentProfile != "Global") {
        AddCategory("Experimental", experimentalTexture, SettingsCategory::Experimental);
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

void SettingsWindow::AddCategory(std::string name,
                                 std::variant<SDL_Texture*, ImGui::RefCountedTexture> texture,
                                 SettingsCategory category) {
    // Tabs are chips: the selected one is filled with the accent colour.
    ImGui::SameLine();
    const bool selected = currentCategory == category;
    if (selected) {
        if (BigPictureMode::Theme::AccentButton(name.c_str())) {
            currentCategory = category;
        }
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, BigPictureMode::Theme::TextDim);
    const bool pressed = ImGui::Button(name.c_str());
    ImGui::PopStyleColor();
    if (pressed) {
        currentCategory = category;
    }
}

void SettingsWindow::DrawMainContent(bool* open, const std::function<void()>& applySettings) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, BigPictureMode::Theme::Panel);

    std::string centeredText;
    currentCategory == SettingsCategory::Folders
        ? centeredText = "Folders that contain your games"
        : centeredText = currentProfile == "Global" ? "Editing: Global settings (all games)"
                                                    : "Editing: " + currentProfile.substr(12);

    ImGui::Dummy(ImVec2(0.0f, 2.0f * uiScale));
    ImGui::TextDisabled("%s", centeredText.c_str());

    if (currentCategory == SettingsCategory::Profiles) {
        DrawProfileSelector();
    } else if (currentCategory == SettingsCategory::Folders) {
        DrawGameFolderManager();
    } else {
        DrawSettingsTable(currentCategory);
    }

    ImGui::PopStyleColor();

    if (HasSuggested()) {
        if (ImGui::Button("Use suggested settings")) {
            ApplySuggested();
        }
        ImGui::SameLine();
    }

    // Align buttons right
    float buttonsWidth = ImGui::CalcTextSize("Save").x + ImGui::CalcTextSize("Cancel").x +
                         ImGui::CalcTextSize("Apply").x + ImGui::GetStyle().FramePadding.x * 6.0f +
                         ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonsWidth);

    // The one action that matters stands out.
    const bool save_clicked = BigPictureMode::Theme::AccentButton("Save");
    if (save_clicked) {
        closeOnSave = true;
        ImGui::OpenPopup("Save Confirmation");
    }

    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        ImGui::OpenPopup("Save Confirmation");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save Confirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", ("Profile Saved:\n" + currentProfile).c_str());
        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(250 * uiScale, 0))) {
            std::string profile = currentProfile;
            if (currentProfile != "Global") {
                profile = currentProfile.substr(0, 9);
            }

            SaveSettings(profile);
            if (closeOnSave) {
                DeInit();
                *open = false;
                closeOnSave = false;
                ImGui::CloseCurrentPopup();
            } else {
                ImGui::CloseCurrentPopup();
            }
            if (applySettings) {
                applySettings();
            }
        }

        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        DeInit();
        *open = false;
        if (applySettings) {
            applySettings();
        }
    }
}

void SettingsWindow::DrawProfileSelector() {
    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, BigPictureMode::Theme::Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);

    ImGui::BeginChild("Profile Selection", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                      child_flags, window_flags);

    ImGui::PopStyleColor();

    // One card per set of settings: what it is, whether the game has settings of its own, and
    // which one is being edited.
    ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0f * uiScale);
    ImGui::TextUnformatted("Which settings do you want to change?");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0f * uiScale);
    ImGui::TextDisabled(
        "Global applies to every game. A game with its own settings ignores Global.");
    ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float margin = 16.0f * uiScale;
    const float line = ImGui::GetTextLineHeight();
    const float card_height = line * 2.0f + 6.0f * uiScale + 36.0f * uiScale;
    const float rounding = 22.0f * uiScale;
    const ImU32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim_color = IM_COL32(140, 140, 150, 255);
    const ImU32 accent = ImGui::GetColorU32(BigPictureMode::Theme::Accent);

    // A fully rounded label, right aligned at `right`. Returns its left edge.
    const auto pill = [&](const char* text, float right, float center_y, ImU32 fill, ImU32 color) {
        const ImVec2 size = ImGui::CalcTextSize(text);
        const float pad = 12.0f * uiScale;
        const ImVec2 min{right - size.x - pad * 2.0f, center_y - line * 0.5f - 5.0f * uiScale};
        const ImVec2 max{right, center_y + line * 0.5f + 5.0f * uiScale};
        draw_list->AddRectFilled(min, max, fill, (max.y - min.y) * 0.5f);
        draw_list->AddText({min.x + pad, center_y - line * 0.5f}, color, text);
        return min.x;
    };

    for (int i = 0; i < profileIcons.size(); i++) {
        const bool is_global = i == 0;
        const std::filesystem::path customConfigFile =
            Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
            (profileIcons[i].serial + ".json");
        const bool gameConfigExists = !is_global && std::filesystem::exists(customConfigFile);
        const std::string profileLabel =
            is_global ? "Global" : profileIcons[i].serial + " - " + profileIcons[i].title;
        const bool selected = currentProfile == profileLabel;

        ImGui::PushID(i);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x - margin;
        const float reset_width =
            gameConfigExists
                ? ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f + margin
                : 0.0f;

        const bool clicked =
            ImGui::InvisibleButton("card", ImVec2(width - reset_width, card_height));
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        const ImVec2 max{pos.x + width, pos.y + card_height};
        draw_list->AddRectFilled(pos, max,
                                 ImGui::GetColorU32(hovered ? ImVec4(0.19f, 0.19f, 0.22f, 1.0f)
                                                            : BigPictureMode::Theme::Chip),
                                 rounding);
        if (selected) {
            draw_list->AddRect(pos, max, accent, rounding, 0, 2.0f * uiScale);
        }

        const std::string title = is_global ? "Global settings" : profileIcons[i].title;
        const std::string subtitle =
            is_global
                ? "Used by every game that has no settings of its own"
                : profileIcons[i].serial + (gameConfigExists ? "  -  has its own settings"
                                                             : "  -  uses the global settings");
        const float text_x = pos.x + 24.0f * uiScale;
        const float text_y = pos.y + (card_height - line * 2.0f - 6.0f * uiScale) * 0.5f;
        draw_list->AddText({text_x, text_y}, text_color, title.c_str());
        draw_list->AddText({text_x, text_y + line + 6.0f * uiScale}, dim_color, subtitle.c_str());

        const float center_y = pos.y + card_height * 0.5f;
        float right = max.x - 18.0f * uiScale - reset_width;
        if (selected) {
            right = pill("Editing", right, center_y, accent,
                         ImGui::GetColorU32(BigPictureMode::Theme::OnAccent)) -
                    8.0f * uiScale;
        }
        if (gameConfigExists) {
            pill("Custom", right, center_y, IM_COL32(58, 58, 68, 255), text_color);
        }

        if (gameConfigExists) {
            // Goes back to the global settings for this game.
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(
                {max.x - reset_width, center_y - ImGui::GetFrameHeight() * 0.5f});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.45f, 0.45f, 1.0f));
            if (ImGui::Button("Remove")) {
                deleteProfileIndex = i;
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove this game's own settings and use Global again");
            }
        }
        ImGui::SetCursorScreenPos({pos.x - margin, max.y + 10.0f * uiScale});
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();

        if (clicked) {
            currentProfile = profileLabel;
            if (is_global) {
                LoadSettings("Global");
            } else {
                LoadSettings(profileIcons[i].serial);
                if (!gameConfigExists) {
                    SaveSettings(profileIcons[i].serial);
                }
            }
        }
    }

    if (deleteProfileIndex != -1) {
        ImGui::OpenPopup("Confirm Delete");
    }

    ImGui::PopStyleVar(3);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string title = profileIcons[deleteProfileIndex].title;
        const std::string message =
            "Remove the settings of " + title + "?\nThe game will use the global settings again.";
        const std::filesystem::path path =
            Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
            (profileIcons[deleteProfileIndex].serial + ".json");

        ImGui::Text("%s", message.c_str());
        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120 * uiScale, 0))) {
            try {
                std::filesystem::remove(path);
            } catch (const std::exception& e) {
                LOG_ERROR(ImGui, "Could not delete config file {}: {}", path.string(), e.what());
            }

            deleteProfileIndex = -1;
            currentProfile = "Global";
            LoadSettings("Global");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120 * uiScale, 0))) {
            deleteProfileIndex = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

void SettingsWindow::DrawGameFolderManager() {
    ImGuiChildFlags child_flags = ImGuiChildFlags_NavFlattened;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), child_flags,
                      window_flags);

    const float margin = 16.0f * uiScale;
    ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImGui::TextUnformatted("Where are your games?");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImGui::TextDisabled("Add the folder that holds your games, with one folder per game inside.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f * uiScale));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    if (BigPictureMode::Theme::AccentButton("Add a folder...")) {
        if (isGameRunning) {
            open_builtin_picker = true;
        } else {
            RequestGamesFolder();
        }
    }
    ConsumeGamesFolder();

    if (open_builtin_picker.exchange(false)) {
        ImGuiFileDialog::Instance()->OpenDialog("OpenFolder", "Add a folder of games", nullptr, ".",
                                                1, nullptr,
                                                ImGuiFileDialogFlags_DisableCreateDirectoryButton |
                                                    ImGuiFileDialogFlags_DontShowHiddenFiles);
    }
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
    }
    if (ImGuiFileDialog::Instance()->Display("OpenFolder", ImGuiWindowFlags_NoMove)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::scoped_lock lock{picked_folder_mutex};
            picked_folder = ImGuiFileDialog::Instance()->GetCurrentPath();
        }
        ImGuiFileDialog::Instance()->Close();
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f * uiScale));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeight();
    const float card_height = ImGui::GetFrameHeight() + 28.0f * uiScale;
    int remove = -1;
    for (int i = 0; i < m_GameInstallDirs.size(); i++) {
        ImGui::PushID(i);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x - margin;
        const ImVec2 max{pos.x + width, pos.y + card_height};
        draw_list->AddRectFilled(pos, max, ImGui::GetColorU32(BigPictureMode::Theme::Chip),
                                 22.0f * uiScale);

        // On the right: whether the folder is used, and removing it from the list.
        const float remove_width =
            ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float toggle_width = line * 1.15f * 1.85f;
        const float controls = remove_width + toggle_width + 36.0f * uiScale;

        const std::string path = Common::FS::PathToUTF8String(m_GameInstallDirs[i].path);
        ImGui::PushClipRect({pos.x, pos.y}, {max.x - controls, max.y}, true);
        draw_list->AddText({pos.x + 24.0f * uiScale, pos.y + (card_height - line) * 0.5f},
                           ImGui::GetColorU32(m_GameInstallDirs[i].enabled
                                                  ? ImGui::GetStyle().Colors[ImGuiCol_Text]
                                                  : BigPictureMode::Theme::TextDim),
                           path.c_str());
        ImGui::PopClipRect();

        const float controls_y = pos.y + (card_height - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorScreenPos({max.x - controls + 8.0f * uiScale, controls_y});
        if (BigPictureMode::Theme::Toggle("##use", &m_GameInstallDirs[i].enabled, uiScale)) {
            SaveInstallDirs();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Show the games of this folder");
        }
        ImGui::SetCursorScreenPos({max.x - remove_width - 14.0f * uiScale, controls_y});
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.45f, 0.45f, 1.0f));
        if (ImGui::Button("Remove")) {
            remove = i;
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Take this folder off the list. Nothing is deleted from disk.");
        }

        ImGui::SetCursorScreenPos({pos.x - margin, max.y + 10.0f * uiScale});
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }
    if (remove >= 0) {
        m_GameInstallDirs.erase(m_GameInstallDirs.begin() + remove);
        SaveInstallDirs();
    }
    if (m_GameInstallDirs.empty()) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
        ImGui::TextDisabled("No folders yet.");
    }

    ImGui::EndChild();
}

void SettingsWindow::DrawSettingsTable(SettingsCategory category) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f * uiScale, 10.0f * uiScale));
    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), child_flags,
                      window_flags);

    if (category == SettingsCategory::General) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCombo("Console Language", consoleLanguageSetting, languageOptions);
            AddSettingSliderInt("Volume", volumeSetting, 0, 500);
            AddSettingCheckbox("Show Splash Screen When Launching Game", showSplashSetting);
            AddSettingCombo("Audio Backend", audioBackendSetting, audioBackendOptions);

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Graphics) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCombo("Display Mode", fullscreenModeSetting, fullscreenModeOptions);
            AddSettingCombo("Present Mode", presentModeSetting, presentModeOptions);
            AddSettingSliderInt("Window Width", windowWidthSetting, 0, 8000);
            AddSettingSliderInt("Window Height", windowHeightSetting, 0, 7000);
            AddSettingCheckbox("Enable HDR", hdrAllowedSetting);
            AddSettingCheckbox("Enable FSR", fsrEnabledSetting);

            if (fsrEnabledSetting) {
                AddSettingCheckbox("Enable RCAS", rcasEnabledSetting);
            }

            if (rcasEnabledSetting && fsrEnabledSetting) {
                AddSettingSliderFloat("RCAS Attenuation", rcasAttenuationSetting, 0.0f, 3.0f, 3);
            }

            AddSettingCheckbox("Frame Generation (FSR 3)", frameGenerationSetting);
            AddSettingCheckbox("Enhance Game Quality", enhanceGameQualitySetting);
            AddSettingCheckbox("FXAA", fxaaSetting);
            AddSettingCheckbox("Direct Readbacks", directReadbacksSetting);
            AddSettingCheckbox("Render Target Sync", renderTargetSyncSetting);

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Input) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCheckbox("Enable Motion Controls", motionControlsSetting);
            AddSettingCheckbox("Enable Background Controller Input", backgroundControllerSetting);
            AddSettingCombo("Hide Cursor", cursorStateSetting, hideCursorOptions);

            if (cursorStateSetting == 1) {
                AddSettingSliderInt("Hide Cursor Idle Timeout", cursorTimeoutSetting, 1, 10);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Trophy) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCheckbox("Disable Trophy Notification", trophyPopupDisabledSetting);
            if (!trophyPopupDisabledSetting) {
                AddSettingCombo("Trophy Notification Position", trophySideSetting,
                                trophySideOptions);
                AddSettingSliderFloat("Trophy Notification Duration", trophyDurationSetting, 0.f,
                                      10.f, 1);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Log) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCheckbox("Enable Logging", logEnableSetting);
            if (logEnableSetting) {
                AddSettingCheckbox("Separate Log Files", logSeparateSetting);
                AddSettingCheckbox("Log Sync", logSyncSetting);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Experimental) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingSliderInt("Additional DMem Allocation", extraDmemSetting, 0, 20000);
            AddSettingSliderInt("Vblank Frequency", vblankFrequencySetting, 30, 360);
            AddSettingCombo("Readbacks Mode", readbacksModeSetting, readbacksModeOptions);
            AddSettingCheckbox("Enable Readback Linear Images", readbackLinearImagesSetting);
            AddSettingCheckbox("Enable Direct Memory Access", directMemoryAccessSetting);
#ifdef _WIN32
            // Windows static guest red-zone protection
            AddSettingCheckbox("Windows Guest Red Zone Protection (Requires Restart)",
                               windowsGuestRedZoneProtectionModeSetting);
#endif
            AddSettingCheckbox("Enable Devkit Console Mode", devkitConsoleSetting);
            AddSettingCheckbox("Enable PS4 Neo Mode", neoModeSetting);
            AddSettingCheckbox("Enable ShadNet", shadnetEnabledSetting);
            AddSettingCheckbox("Set Network Connected to True", connectedNetworkSetting);
            AddSettingCheckbox("Enable Shader Cache", pipelineCacheEnabledSetting);

            if (pipelineCacheEnabledSetting) {
                AddSettingCheckbox("Compress Shader Cache to Zip File",
                                   pipelineCacheArchiveSetting);
            }

            ImGui::EndTable();
        }
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void SettingsWindow::AddSettingCheckbox(std::string name, bool& value) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * uiScale);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", name.c_str());
    ImGui::TableNextColumn();
    BigPictureMode::Theme::Toggle(label.c_str(), &value, uiScale);
}

void SettingsWindow::AddSettingSliderInt(std::string name, int& value, int min, int max) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * uiScale);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", name.c_str());

    ImGui::TableNextColumn();
    ImGui::SliderInt(label.c_str(), &value, min, max);
}

void SettingsWindow::AddSettingSliderFloat(std::string name, float& value, int min, int max,
                                           int precision) {
    std::string label = "##" + name;
    std::string precisionString = "%." + std::to_string(precision) + "f";

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * uiScale);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", name.c_str());

    ImGui::TableNextColumn();
    ImGui::SliderFloat(label.c_str(), &value, min, max, precisionString.c_str());
}

void SettingsWindow::AddSettingCombo(std::string name, int& value,
                                     std::vector<std::string> options) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f * uiScale);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", name.c_str());

    ImGui::TableNextColumn();
    const char* combo_value = options[value].c_str();
    if (ImGui::BeginCombo(label.c_str(), combo_value)) {
        for (int i = 0; i < options.size(); i++) {
            const bool selected = (i == value);
            if (ImGui::Selectable(options[i].c_str(), selected))
                value = i;

            // Set the initial focus when opening the combo
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

} // namespace ImGuiEmuSettings
