// rr6 - ReXGlue Recompiled Project

#include "gpu/settings_overlay.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/settings_overlay.h>

namespace rr6::gpu {

namespace {

struct Choice {
  const char *label;
  const char *value;
};

constexpr float kLabelWidth = 230.0f;
constexpr float kWidgetWidth = 150.0f;

// Toggled only by the bind. A plain pointer, so no static destructor touches
// the ImGui drawer after the app has torn it down: rr6 ends the process
// without unwinding (core/shutdown.cpp).
SettingsOverlay *g_overlay = nullptr;

bool RequiresRestart(const rex::cvar::FlagEntry *info) {
  return info && info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart;
}

// The label column: plain words, with the cvar's own description and name on
// hover, and "(restart)" after the widget for options read only at start-up.
void Label(const char *label, const rex::cvar::FlagEntry *info) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (info && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s\n[%s]", info->description.c_str(),
                      info->name.c_str());
  ImGui::SameLine(kLabelWidth);
  ImGui::SetNextItemWidth(kWidgetWidth);
}

void RestartNote(const rex::cvar::FlagEntry *info) {
  if (!RequiresRestart(info))
    return;
  ImGui::SameLine();
  ImGui::TextDisabled("(restart)");
}

void Checkbox(const char *label, const char *cvar) {
  const auto *info = rex::cvar::GetFlagInfo(cvar);
  if (!info)
    return;
  ImGui::PushID(cvar);
  Label(label, info);
  bool v = rex::cvar::GetFlagByName(cvar) == "true";
  if (ImGui::Checkbox("##v", &v))
    rex::cvar::SetFlagByName(cvar, v ? "true" : "false");
  RestartNote(info);
  ImGui::PopID();
}

// A checkbox over an integer cvar that takes `off` or `on`.
void IntCheckbox(const char *label, const char *cvar, int off, int on) {
  const auto *info = rex::cvar::GetFlagInfo(cvar);
  if (!info)
    return;
  ImGui::PushID(cvar);
  Label(label, info);
  bool v = rex::cvar::GetFlagByName(cvar) == std::to_string(on);
  if (ImGui::Checkbox("##v", &v))
    rex::cvar::SetFlagByName(cvar, std::to_string(v ? on : off));
  RestartNote(info);
  ImGui::PopID();
}

// A value outside the list (from the config or the command line) shows as
// itself until another is picked. Double cvars read back as "1.000000", so
// those match their choices by value.
void Combo(const char *label, const char *cvar,
           std::initializer_list<Choice> choices) {
  const auto *info = rex::cvar::GetFlagInfo(cvar);
  if (!info)
    return;
  ImGui::PushID(cvar);
  Label(label, info);
  const std::string current = rex::cvar::GetFlagByName(cvar);
  const bool numeric = info->type == rex::cvar::FlagType::Double;
  auto it = std::find_if(choices.begin(), choices.end(), [&](const Choice &c) {
    if (numeric)
      return std::strtod(current.c_str(), nullptr) ==
             std::strtod(c.value, nullptr);
    return current == c.value;
  });
  const char *preview = it != choices.end() ? it->label : current.c_str();
  if (ImGui::BeginCombo("##v", preview)) {
    for (const Choice &c : choices) {
      const bool selected = &c == it;
      if (ImGui::Selectable(c.label, selected))
        rex::cvar::SetFlagByName(cvar, c.value);
      if (selected)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  RestartNote(info);
  ImGui::PopID();
}

void Section(const char *title) {
  ImGui::Spacing();
  ImGui::SeparatorText(title);
}

} // namespace

SettingsOverlay::SettingsOverlay(rex::ui::ImGuiDrawer *imgui_drawer,
                                 std::filesystem::path config_path,
                                 std::function<void()> quit)
    : rex::ui::ImGuiDialog(imgui_drawer), config_path_(std::move(config_path)),
      quit_(std::move(quit)) {}

SettingsOverlay::~SettingsOverlay() = default;

void SettingsOverlay::InstallBind(rex::ui::ImGuiDrawer *imgui_drawer,
                                  std::filesystem::path config_path,
                                  std::function<void()> quit) {
  // The SDK's own F4 bind and full dialog are left as they are. F1 is free:
  // the SDK binds F3, Backtick, F4 and F7.
  rex::ui::RegisterBind(
      "bind_rr6_settings", "F1", "Toggle settings menu",
      [imgui_drawer, config_path = std::move(config_path),
       quit = std::move(quit)] {
        if (g_overlay) {
          delete g_overlay;
          g_overlay = nullptr;
        } else {
          g_overlay = new SettingsOverlay(imgui_drawer, config_path, quit);
        }
      });
}

void SettingsOverlay::OnDraw(ImGuiIO & /*io*/) {
  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  if (!ImGui::Begin("Settings##rr6", nullptr,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  Section("Display");
  Checkbox("Fullscreen", "fullscreen");
  // RR6 is fixed at 60 fps, so rr6_fps_limit stays in the full dialog.
  Checkbox("Vertical sync", "rr6_vsync");

  Section("Resolution");
  Combo("Resolution scale", "rr6_resolution_scale",
        {{"Auto (window size)", "0"},
         {"1x (720p)", "1"},
         {"2x (1440p)", "2"},
         {"3x (2160p)", "3"},
         {"4x (2880p)", "4"}});
  IntCheckbox("Supersampling (2x scale)", "rr6_supersampling", 1, 2);

  Section("Image quality");
  Checkbox("FXAA", "rr6_fxaa");
  Combo("Anisotropic filtering", "rr6_anisotropy",
        {{"Game default", "default"},
         {"2x", "2"},
         {"4x", "4"},
         {"8x", "8"},
         {"16x", "16"}});
  Combo("Detail distance", "rr6_lod_distance",
        {{"1x (console)", "1"},
         {"2x", "2"},
         {"5x", "5"},
         {"10x", "10"},
         {"20x", "20"}});
  Checkbox("16-bit output", "rr6_output_16bit");
  Checkbox("Console display correction", "rr6_display_correction");

  ImGui::Spacing();
  ImGui::Separator();
  if (!rex::cvar::GetPendingRestartFlags().empty())
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                       "Restart the game to apply the changes marked (restart).");
  if (ImGui::Button("Save"))
    rex::cvar::SaveConfig(config_path_);
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());
  // The SDK's bind, read each frame so a rebind shows here.
  const std::string all_key = rex::cvar::GetFlagByName("bind_settings");
  if (!all_key.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s: all settings", all_key.c_str());
  }

  // Right-aligned on the same row, away from Save.
  const char *quit_label = "Quit game";
  const float quit_width = ImGui::CalcTextSize(quit_label).x +
                           ImGui::GetStyle().FramePadding.x * 2.0f;
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                       std::max(0.0f, ImGui::GetContentRegionAvail().x -
                                          quit_width));
  if (ImGui::Button(quit_label))
    ImGui::OpenPopup("Quit game?##rr6");

  if (ImGui::BeginPopupModal("Quit game?##rr6", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Quit the game? Unsaved progress is lost.");
    ImGui::Spacing();
    if (ImGui::Button("Quit") && quit_) {
      ImGui::CloseCurrentPopup();
      quit_();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
      ImGui::CloseCurrentPopup();
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
  }

  ImGui::End();
}

} // namespace rr6::gpu
