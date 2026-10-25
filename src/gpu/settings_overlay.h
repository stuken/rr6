// rr6 - ReXGlue Recompiled Project
//
// The F4 settings menu: the few options a player needs (display, frame rate,
// resolution, image quality) in plain words, over the same cvars and config
// file as the SDK's full settings dialog, which "All settings" still opens.

#pragma once

#include <filesystem>
#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class SettingsDialog;
} // namespace rex::ui

namespace rr6::gpu {

class SettingsOverlay final : public rex::ui::ImGuiDialog {
public:
  SettingsOverlay(rex::ui::ImGuiDrawer *imgui_drawer,
                  std::filesystem::path config_path);
  ~SettingsOverlay() override;

  // Replaces the SDK's F4 bind with one that toggles this menu. Call from
  // OnCreateDialogs, after the SDK has registered its own binds.
  static void InstallBind(rex::ui::ImGuiDrawer *imgui_drawer,
                          std::filesystem::path config_path);

protected:
  void OnDraw(ImGuiIO &io) override;

private:
  std::filesystem::path config_path_;
  std::unique_ptr<rex::ui::SettingsDialog> all_settings_;
};

} // namespace rr6::gpu
