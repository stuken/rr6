// rr6 - ReXGlue Recompiled Project
//
// Global Rankings (Global Time Attack) asks "An Xbox Live connection is
// required in order to proceed. Attempt to reconnect?". NO returns to the
// menu. YES froze it: the menu kept animating but ignored all input.
//
// The screen's update, sub_8218A758(screen, ..., answer in r5, ...), keeps
// its state at screen+0. State 25 takes the prompt's answer: NO goes back to
// the menu, YES opens the sign-in UI (sub_8210EA30) and moves to state 26,
// which waits for the Live context's "system UI open" flag (0x823B0934) that
// the game sets from an XN_SYS_UI(true) notification, then state 27 waits for
// it to clear. The SDK's XamShowSigninUI sends only XN_SYS_SIGNINCHANGED and
// XN_SYS_UI(false), so state 26 never ends. With no Live service there is
// nothing to reconnect to, so the answer is always taken as NO.

#include <rex/hook.h>
#include <rex/types.h>

#include "core/memory_helpers.h"

REX_EXTERN(__imp__sub_8218A758);

namespace {
constexpr u32 kReconnectAnswerState = 25;
} // namespace

REX_HOOK_RAW(sub_8218A758) {
  const auto *state = rr6::mem::at<const be_u32>(ctx.r3.u32);
  if (state && u32(*state) == kReconnectAnswerState)
    ctx.r5.u64 = 0;
  __imp__sub_8218A758(ctx, base);
}
