// rr6 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

#include "icon.generated.h"

#include "platform/silent_audio.h"
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, rr6_null_audio);

#ifdef RR6_NATIVE_GPU
#include <rex/logging.h>

#include "core/shutdown.h"
#include "gpu/device.h"
#include "gpu/imgui_overlay_drawer.h"
#include "gpu/settings.h"
#include "gpu/settings_overlay.h"
#include "platform/guest_abort_log.h"
#endif

class Rr6App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Rr6App>(new Rr6App(ctx, "rr6",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
#ifdef RR6_NATIVE_GPU
      // Null graphics makes Runtime::Setup skip the SDK GPU. The plume
      // renderer comes up in OnPreLaunchModule instead.
      config.graphics = nullptr;
      config.gpu_plugin.clear();
#else
      config.gpu_plugin = "xenos";
#endif
      if (REXCVAR_GET(rr6_null_audio))
        config.audio_factory = REX_AUDIO_BACKEND(rr6::platform::SilentAudioSystem);
#ifdef _WIN32
      timeBeginPeriod(1);
#endif
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
      config_path_ = paths.config_path;
      const auto base_dir = paths.config_path.parent_path();
      if (paths.game_data_root.empty()) { // Use default assets directory path if one isn't provided!
          if (std::filesystem::is_regular_file(base_dir / "assets/default.xex")) {
              paths.game_data_root = base_dir / "assets";
          }
      }
      paths.user_data_root = base_dir / "user";
      paths.cache_root = base_dir / "cache";
      paths.update_data_root = base_dir / "update";
  }

  void OnPostSetup() override{
    window()->SetIcon(icon::kIconPNG, icon::kIconPNGSize);
    window()->SetTitle("Ridge Racer 6");
  }

#ifdef RR6_NATIVE_GPU
  // Binds to the plume device lazily, once OnPreLaunchModule creates it. The
  // SDK owns and tears this down.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return std::make_unique<rr6::gpu::ImGuiOverlayDrawer>();
  }

  // The first hook after rex::cvar::LoadConfig: the renderer's settings adopt
  // the config file and command line here and follow later cvar writes.
  // Before this they all sat at their compiled defaults.
  void OnPostInitLogging() override { rr6::gpu::Settings::Get().Init(); }

  void OnPreLaunchModule() override {
    rr6::platform::InstallGuestAbortLogger();
    // Registered here, where the UI thread is pumping: the shutdown sequence
    // has to run on it.
    rr6::SetShutdownDispatcher([this](std::function<void()> fn) {
      return app_context().CallInUIThread(std::move(fn));
    });
    rr6::SetShutdownUIPump(
        [this] { app_context().ExecutePendingFunctionsFromUIThread(); });

    if (!rr6::gpu::Video::CreateHostDevice(window())) {
      REXLOG_ERROR("rr6: native renderer init failed");
      app_context().QuitFromUIThread();
      return;
    }
    rr6::gpu::Video::SetOverlayDrawHook(
        [this](plume::RenderCommandList* cmd, plume::RenderFramebuffer* fb,
               u32 w, u32 h) {
          if (!imgui_drawer() || rr6::IsShuttingDown() ||
              !imgui_drawer()->HasDialogs())
            return;
          app_context().CallInUIThreadSynchronous([this, cmd, fb, w, h] {
            rr6::gpu::ReblueUIDrawContext ctx(w, h, cmd, fb);
            imgui_drawer()->Draw(ctx);
          });
        });
  }

  // F4 opens rr6's short settings menu in place of the SDK's full one.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rr6::gpu::SettingsOverlay::InstallBind(drawer, config_path_);
  }

  void OnWindowPixelSizeChanged(u32, u32) override {
    rr6::gpu::Video::RequestResize();
  }

  bool OnWindowCloseRequested() override {
    rr6::RequestShutdown(rr6::ShutdownReason::WindowClose);
    return false;  // unreachable
  }
#endif

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

 private:
  // Where the settings menus save (OnConfigurePaths).
  std::filesystem::path config_path_;
};
