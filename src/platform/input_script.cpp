// rr6 - ReXGlue Recompiled Project
//
// rr6_input_script: scripted controller input for unattended testing, e.g.
//   --rr6_input_script="start@900,a@1000+5,rt@1100+600"
// Each entry presses a button (or pulls a trigger) on present frame N and
// holds it for the given number of frames (default 6). A trailing 's' on
// either number counts seconds since the game first polled the pad instead.
// The xenos plugin build counts frames by wrapping the guest's Present and
// logs every 300th, so captures can be timed against the native renderer's
// frame numbers. Buttons: a b x y start
// back up down left right lb rb ls rs; triggers: lt rt. Applied to user 0 on
// top of whatever a real pad reports, and while a script is set a missing pad
// reads as connected, since remote sessions usually have none.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_STRING(rr6_input_script, "", kCvarGroup,
                      "Scripted pad input for testing: button@frame[+hold],...");

// Unattended runs over remote sessions can lose the audio endpoint, and the
// guest faults at boot when audio init fails. This swaps the SDL
// backend for a silent one (platform/silent_audio.h, read in Rr6App::OnPreSetup).
REXCVAR_DEFINE_BOOL(rr6_null_audio, false, kCvarGroup,
                    "Discard audio output (testing without an audio device)");

REX_EXTERN(__imp__XInputGetState);

#if defined(RR6_NATIVE_GPU)
namespace rr6::gpu {
u32 PresentCount();
}
#else
REX_EXTERN(__imp__D3DDevice_Present);

namespace {
std::atomic<u32> g_presents{0};
}

void Rr6XenosDumpOnPresent(); // platform/xenos_resolve_dump.cpp

REX_HOOK_RAW(D3DDevice_Present) {
  const u32 n = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % 300 == 0)
    RR6_INFO("rr6_input_script: present {}", n);
  Rr6XenosDumpOnPresent();
  __imp__D3DDevice_Present(ctx, base);
}
#endif

namespace {

struct Press {
  u16 buttons = 0;
  u8 left_trigger = 0;
  u8 right_trigger = 0;
  double start = 0; // frames, or seconds when 'seconds'
  double hold = 6;
  bool seconds = false;
};

// XINPUT_STATE.
struct InputState {
  be_u32 dwPacketNumber;
  be_u16 wButtons;
  u8 bLeftTrigger;
  u8 bRightTrigger;
  be_i16 sThumbLX;
  be_i16 sThumbLY;
  be_i16 sThumbRX;
  be_i16 sThumbRY;
};

bool ParseButton(const std::string &name, Press &p) {
  static constexpr struct {
    const char *name;
    u16 mask;
  } kButtons[] = {{"up", 0x0001},     {"down", 0x0002}, {"left", 0x0004},
                  {"right", 0x0008},  {"start", 0x0010}, {"back", 0x0020},
                  {"ls", 0x0040},     {"rs", 0x0080},   {"lb", 0x0100},
                  {"rb", 0x0200},     {"a", 0x1000},    {"b", 0x2000},
                  {"x", 0x4000},      {"y", 0x8000}};
  for (const auto &b : kButtons) {
    if (name == b.name) {
      p.buttons = b.mask;
      return true;
    }
  }
  if (name == "lt") {
    p.left_trigger = 255;
    return true;
  }
  if (name == "rt") {
    p.right_trigger = 255;
    return true;
  }
  return false;
}

const std::vector<Press> &Script() {
  static std::once_flag once;
  static std::vector<Press> presses;
  std::call_once(once, [] {
    const std::string text = REXCVAR_GET(rr6_input_script);
    size_t pos = 0;
    while (pos < text.size()) {
      size_t end = text.find(',', pos);
      if (end == std::string::npos)
        end = text.size();
      const std::string entry = text.substr(pos, end - pos);
      pos = end + 1;
      const size_t at = entry.find('@');
      if (at == std::string::npos)
        continue;
      Press p;
      if (!ParseButton(entry.substr(0, at), p)) {
        RR6_WARN("rr6_input_script: unknown button in '{}'", entry);
        continue;
      }
      const size_t plus = entry.find('+', at);
      const std::string when = entry.substr(at + 1, plus - at - 1);
      p.seconds = !when.empty() && when.back() == 's';
      p.start = std::stod(when);
      if (plus != std::string::npos)
        p.hold = std::stod(entry.substr(plus + 1));
      else if (p.seconds)
        p.hold = 0.2;
      presses.push_back(p);
    }
    if (!presses.empty())
      RR6_INFO("rr6_input_script: {} scripted press(es)", presses.size());
  });
  return presses;
}

} // namespace

// (user, state) -> ERROR_SUCCESS / ERROR_DEVICE_NOT_CONNECTED
REX_HOOK_RAW(XInputGetState) {
  const u32 user = ctx.r3.u32;
  const u32 state_va = ctx.r4.u32;
  __imp__XInputGetState(ctx, base);
  const auto &script = Script();
  if (script.empty() || user != 0)
    return;
  auto *state = rr6::mem::at<InputState>(state_va);
  if (!state)
    return;
  if (ctx.r3.u32 != 0) {
    // No pad: report a neutral connected one so the script has a target.
    std::memset(state, 0, sizeof(InputState));
    ctx.r3.u64 = 0;
  }
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point t0 = Clock::now();
  const double secs =
      std::chrono::duration<double>(Clock::now() - t0).count();
#if defined(RR6_NATIVE_GPU)
  const double frame = rr6::gpu::PresentCount();
#else
  const double frame = g_presents.load(std::memory_order_relaxed);
#endif
  u16 buttons = 0;
  u8 lt = 0, rt = 0;
  for (const Press &p : script) {
    const double now = p.seconds ? secs : frame;
    if (now >= p.start && now < p.start + p.hold) {
      buttons |= p.buttons;
      lt = std::max(lt, p.left_trigger);
      rt = std::max(rt, p.right_trigger);
    }
  }
  if (!buttons && !lt && !rt)
    return;
  state->wButtons = u16(state->wButtons) | buttons;
  state->bLeftTrigger = std::max<u8>(state->bLeftTrigger, lt);
  state->bRightTrigger = std::max<u8>(state->bRightTrigger, rt);
  state->dwPacketNumber =
      u32(state->dwPacketNumber) + 1 + static_cast<u32>(secs * 60.0);
}
