// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace BigPictureMode {

/// Languages the interface is available in, as stored in the settings. "System" follows the
/// language of the operating system.
inline constexpr const char* GuiLanguageSystem = "System";
inline constexpr const char* GuiLanguageEnglish = "English";
inline constexpr const char* GuiLanguageSpanish = "Español";

/// Chooses the language of the interface from the stored setting.
void SetGuiLanguage(std::string_view language);

/// Returns the text in the language of the interface, or the text itself when there is no
/// translation for it. An ImGui identifier after "##" is kept as it is. The result stays valid
/// for the life of the program.
const char* Tr(std::string_view english);

} // namespace BigPictureMode
