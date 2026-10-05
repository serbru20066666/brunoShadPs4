// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <variant>
#include <SDL3/SDL.h>
#include <imgui.h>

#include "big_picture.h"
#include "core/emulator_settings.h"
#include "imgui/imgui_texture.h"

namespace ImGuiEmuSettings {

class SettingsWindow {

public:
    SettingsWindow(bool gameRunning);
    void Prepare();
    /// Opens the dialog on the list of game folders.
    void OpenFolders() {
        currentCategory = SettingsCategory::Folders;
    }
    /// Opens the dialog on the graphics settings of one game.
    void OpenProfile(const std::string& serial, const std::string& title);

    /// Opens the system's own folder picker to add a folder of games.
    static void RequestGamesFolder();
    /// Whether the built-in picker is waiting to be shown, because the system's is not
    /// available. The launcher opens the folders page for it.
    static bool BuiltinPickerPending();
    /// Adds the folder picked with RequestGamesFolder, if one arrived. Returns true when the
    /// list of folders changed.
    bool ConsumeGamesFolder();

    /// Whether there are settings known to work well for this game.
    static bool HasSuggestedFor(const std::string& serial);
    /// Whether the game already has settings of its own.
    static bool HasOwnSettings(const std::string& serial);
    /// Gives the game its own settings, set to the suggested ones, and saves them.
    void ApplySuggestedTo(const std::string& serial, const std::string& title);
    void DrawSettings(bool* open, const std::function<void()>& applySettings);

private:
    enum class SettingsCategory {
        Profiles,
        General,
        Graphics,
        Input,
        Trophy,
        Folders,
        Log,
        Experimental,
        Patches,
    };

    void SaveSettings(std::string profile);
    void LoadSettings(std::string profile);
    void SaveInstallDirs();
    /// Fills the dialog with the settings known to work well for the selected game.
    void ApplySuggested();
    bool HasSuggested() const;

    void SetupWindow();
    void DeInit();
    void GetProfileInfo();

    void DrawMainContent(bool* open, const std::function<void()>& applySettings);
    void DrawSettingsTable(SettingsCategory);
    void DrawProfileSelector();
    void DrawGameFolderManager();
    void DrawPatchManager();
    void LoadGamePatches();
    void SetGamePatchEnabled(size_t index, bool enabled);
    void InstallPatchFile(const std::filesystem::path& picked);
    void DrawCategoryTabs();
    void AddCategory(std::string name, std::variant<SDL_Texture*, ImGui::RefCountedTexture> texture,
                     SettingsCategory category);

    void AddSettingCheckbox(std::string name, bool& value);
    void AddSettingNote(std::string text);

public:
    /// The name of the patch in the file named after the game in the patches folder: what the
    /// launcher applies when the game starts. Empty when the game has no such file.
    static std::string GamePatchName(const std::string& serial);

private:
    void AddSettingSliderInt(std::string name, int& value, int min, int max);
    void AddSettingSliderFloat(std::string name, float& value, int min, int max, int precision);
    void AddSettingCombo(std::string name, int& value, std::vector<std::string> options);
    int GetComboIndex(std::string selection, std::vector<std::string> options);

    std::vector<BigPictureMode::IconInfo> profileIcons = {};
    std::vector<GameInstallDir> m_GameInstallDirs = {};

    float uiScale = 1.0f;
    SettingsCategory currentCategory = SettingsCategory::Profiles;
    std::string currentProfile = "Global";

    std::string runningGameSerial = "";
    bool isGameRunning = false;
    bool closeOnSave = false;
    int deleteProfileIndex = -1;

    std::variant<SDL_Texture*, ImGui::RefCountedTexture> profilesTexture;
    std::variant<SDL_Texture*, ImGui::RefCountedTexture> generalTexture;
    std::variant<SDL_Texture*, ImGui::RefCountedTexture> experimentalTexture;
    std::variant<SDL_Texture*, ImGui::RefCountedTexture> graphicsTexture;
    std::variant<SDL_Texture*, ImGui::RefCountedTexture> inputTexture;
    std::variant<SDL_Texture*, ImGui::RefCountedTexture> trophyTexture;
    std::variant<SDL_Texture*, ImGui::RefCountedTexture> logTexture;
    std::variant<SDL_Texture*, ImGui::RefCountedTexture> foldersTexture;

    //////////////////// options for comboboxes
    const std::map<std::string, int> languageMap = {{"Arabic", 21},
                                                    {"Czech", 23},
                                                    {"Danish", 14},
                                                    {"Dutch", 6},
                                                    {"English (United Kingdom)", 18},
                                                    {"English (United States)", 1},
                                                    {"Finnish", 12},
                                                    {"French (Canada)", 22},
                                                    {"French (France)", 2},
                                                    {"German", 4},
                                                    {"Greek", 25},
                                                    {"Hungarian", 24},
                                                    {"Indonesian", 29},
                                                    {"Italian", 5},
                                                    {"Japanese", 0},
                                                    {"Korean", 9},
                                                    {"Norwegian (Bokmaal)", 15},
                                                    {"Polish", 16},
                                                    {"Portuguese (Brazil)", 17},
                                                    {"Portuguese (Portugal)", 7},
                                                    {"Romanian", 26},
                                                    {"Russian", 8},
                                                    {"Simplified Chinese", 11},
                                                    {"Spanish (Latin America)", 20},
                                                    {"Spanish (Spain)", 3},
                                                    {"Swedish", 13},
                                                    {"Thai", 27},
                                                    {"Traditional Chinese", 10},
                                                    {"Turkish", 19},
                                                    {"Ukrainian", 30},
                                                    {"Vietnamese", 28}};
    std::vector<std::string> languageOptions; // assigned from keys above
    const std::vector<std::string> fullscreenModeOptions = {"Windowed", "Fullscreen",
                                                            "Fullscreen (Borderless)"};
    const std::vector<std::string> audioBackendOptions = {"SDL", "OpenAL"};
    const std::vector<std::string> guiLanguageOptions = {"System", "English", "Español"};
    int guiLanguageSetting{};
    const std::vector<std::string> presentModeOptions = {"Mailbox", "Fifo", "Immediate"};
    const std::vector<std::string> hideCursorOptions = {"Never", "Idle", "Always"};
    const std::vector<std::string> trophySideOptions = {"left", "right", "top", "bottom"};
    const std::vector<std::string> readbacksModeOptions = {"Disabled", "Relaxed", "Precise"};

    //////////////// Setting Variables
    //////////////// Note:: Use int for all comboboxes as needed by ImGui

    // General tab
    int consoleLanguageSetting;
    int volumeSetting;
    bool showSplashSetting;
    int audioBackendSetting;

    // Graphics tab
    int fullscreenModeSetting;
    int presentModeSetting;
    int windowWidthSetting;
    int windowHeightSetting;
    /// The sizes offered for the window, and the one chosen.
    const std::vector<std::pair<int, int>> windowSizes = {
        {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    const std::vector<std::string> windowSizeOptions = {"1280 x 720", "1600 x 900", "1920 x 1080",
                                                        "2560 x 1440", "3840 x 2160"};
    int windowSizeSetting{};
    bool hdrAllowedSetting;
    bool fsrEnabledSetting;
    bool frameGenerationSetting;
    /// The patch file of the game being edited, if it has one, and whether to use it.
    std::string gamePatchName;
    /// One patch of the patch file of the game being edited.
    struct GamePatch {
        std::string name;
        std::string author;
        std::string patch_version;
        std::string app_version;
        bool enabled{};
    };
    std::vector<GamePatch> gamePatches;
    std::string gamePatchSerial;
    bool gamePatchFileExists{};
    /// What happened with the last file that was picked, shown under the buttons.
    std::string gamePatchMessage;
    int gamePatchSetting{};
    bool enhanceGameQualitySetting;
    bool fxaaSetting;
    bool directReadbacksSetting;
    bool renderTargetSyncSetting;
    bool rcasEnabledSetting;
    float rcasAttenuationSetting;

    // Input tab
    bool motionControlsSetting;
    bool backgroundControllerSetting;
    int cursorStateSetting;
    int cursorTimeoutSetting;

    // Trophy tab
    bool trophyPopupDisabledSetting;
    int trophySideSetting;
    float trophyDurationSetting;

    // Log tab
    bool logEnableSetting;
    bool logSeparateSetting;
    bool logSyncSetting;

    // Experimental tab
    int readbacksModeSetting;
    bool readbackLinearImagesSetting;
    bool directMemoryAccessSetting;
    bool windowsGuestRedZoneProtectionModeSetting;
    bool devkitConsoleSetting;
    bool neoModeSetting;
    bool shadnetEnabledSetting;
    bool connectedNetworkSetting;
    bool pipelineCacheEnabledSetting;
    bool pipelineCacheArchiveSetting;
    int extraDmemSetting;
    int vblankFrequencySetting;
};

} // namespace ImGuiEmuSettings
