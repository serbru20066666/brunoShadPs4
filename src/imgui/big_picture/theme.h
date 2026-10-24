// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <imgui.h>

namespace BigPictureMode::Theme {

/// The one colour that marks what is selected or is the main action: a bright blue, with white
/// text on top of it (OnAccent).
constexpr ImVec4 Accent{0.10f, 0.55f, 1.00f, 1.00f};
constexpr ImVec4 AccentHovered{0.27f, 0.65f, 1.00f, 1.00f};
constexpr ImVec4 OnAccent{1.00f, 1.00f, 1.00f, 1.00f};
/// Rounded slate cards on a dark slate background.
constexpr ImVec4 Background{0.090f, 0.098f, 0.122f, 1.00f};
constexpr ImVec4 Card{0.133f, 0.145f, 0.180f, 1.00f};
constexpr ImVec4 CardHovered{0.165f, 0.180f, 0.220f, 1.00f};
constexpr ImVec4 Chip{0.180f, 0.196f, 0.240f, 1.00f};
constexpr ImVec4 TextDim{0.58f, 0.61f, 0.68f, 1.00f};
constexpr ImVec4 Panel = Card;

/// Dark look made of rounded cards and pill shaped controls, shared by the game launcher and the
/// settings dialog. Every Push needs a Pop.
inline void Push(float scale) {
    const auto color = [](ImGuiCol id, const ImVec4& value) { ImGui::PushStyleColor(id, value); };
    color(ImGuiCol_WindowBg, Background);
    color(ImGuiCol_ChildBg, Card);
    color(ImGuiCol_PopupBg, ImVec4(0.150f, 0.163f, 0.200f, 1.0f));
    color(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.96f, 1.0f));
    color(ImGuiCol_TextDisabled, TextDim);
    color(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    color(ImGuiCol_Separator, ImVec4(0.20f, 0.22f, 0.27f, 1.0f));
    color(ImGuiCol_Button, Chip);
    color(ImGuiCol_ButtonHovered, ImVec4(0.225f, 0.245f, 0.300f, 1.0f));
    color(ImGuiCol_ButtonActive, ImVec4(0.270f, 0.295f, 0.360f, 1.0f));
    color(ImGuiCol_FrameBg, Chip);
    color(ImGuiCol_FrameBgHovered, ImVec4(0.215f, 0.235f, 0.285f, 1.0f));
    color(ImGuiCol_FrameBgActive, ImVec4(0.250f, 0.272f, 0.330f, 1.0f));
    color(ImGuiCol_Header, ImVec4(0.215f, 0.235f, 0.285f, 1.0f));
    color(ImGuiCol_HeaderHovered, ImVec4(0.250f, 0.272f, 0.330f, 1.0f));
    color(ImGuiCol_HeaderActive, ImVec4(0.290f, 0.315f, 0.380f, 1.0f));
    color(ImGuiCol_TableRowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    color(ImGuiCol_TableRowBgAlt, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    color(ImGuiCol_TableBorderLight, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    color(ImGuiCol_TableBorderStrong, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    color(ImGuiCol_ScrollbarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    color(ImGuiCol_ScrollbarGrab, ImVec4(0.250f, 0.272f, 0.330f, 1.0f));
    color(ImGuiCol_CheckMark, Accent);
    color(ImGuiCol_SliderGrab, Accent);
    color(ImGuiCol_SliderGrabActive, AccentHovered);
    // White: the accent ring vanished around the accent-filled Play buttons.
    color(ImGuiCol_NavCursor, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 24.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 18.0f * scale);
    // Larger than any control: everything with a frame becomes a pill.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 100.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 100.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 100.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * scale, 12.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.0f * scale, 9.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(14.0f * scale, 9.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f * scale, 24.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 16.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0f * scale);
}

inline void Pop() {
    ImGui::PopStyleVar(15);
    ImGui::PopStyleColor(26);
}

/// Around the last item while it has the focus (moved to with the pad or the keyboard): a thick
/// white ring, and a light flash while it is held down, so a pad player sees where they are and
/// that the press was taken.
inline void FocusRing() {
    if (!ImGui::IsItemFocused()) {
        return;
    }
    const float s = ImGui::GetFontSize() / 20.0f;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 pad{4.0f * s, 4.0f * s};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (ImGui::IsItemActive()) {
        draw->AddRectFilled(min, max, IM_COL32(255, 255, 255, 70), 100.0f * s);
    }
    draw->AddRect(ImVec2(min.x - pad.x, min.y - pad.y), ImVec2(max.x + pad.x, max.y + pad.y),
                  IM_COL32(255, 255, 255, 255), 100.0f * s, 0, 3.5f * s);
}

/// A pill button with the focus ring.
inline bool Button(const char* label) {
    const bool pressed = ImGui::Button(label);
    FocusRing();
    return pressed;
}

/// A pill button filled with the accent colour, for the main action of a screen.
inline bool AccentButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentHovered);
    ImGui::PushStyleColor(ImGuiCol_Text, OnAccent);
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    FocusRing();
    return pressed;
}

/// An on/off switch, drawn as a pill with a knob. Returns true when it was toggled.
inline bool Toggle(const char* id, bool* value, float scale) {
    // Sized from the text, and centred on the row like any other framed control.
    const float height = ImGui::GetTextLineHeight() * 1.15f;
    const float width = height * 1.85f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - height) * 0.5f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (pressed) {
        *value = !*value;
    }
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec4 track =
        *value ? (hovered ? AccentHovered : Accent)
               : (hovered ? ImVec4(0.29f, 0.31f, 0.38f, 1.0f) : ImVec4(0.22f, 0.24f, 0.29f, 1.0f));
    draw_list->AddRectFilled(pos, {pos.x + width, pos.y + height}, ImGui::GetColorU32(track),
                             height * 0.5f);
    const float radius = height * 0.5f - height * 0.16f;
    const float knob_x = *value ? pos.x + width - height * 0.5f : pos.x + height * 0.5f;
    draw_list->AddCircleFilled(
        {knob_x, pos.y + height * 0.5f}, radius,
        ImGui::GetColorU32(*value ? OnAccent : ImVec4(0.62f, 0.62f, 0.67f, 1.0f)));
    return pressed;
}

} // namespace BigPictureMode::Theme
