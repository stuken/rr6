// rr6 - ReXGlue Recompiled Project
//
// rr6_lod_distance: how far out RR6 keeps its detailed models. Game logic,
// not rendering, so it works with either renderer.
//
// Course and scenery models carry a level of detail record (count, first mesh
// for each of two passes, then count - 1 float switch distances). The
// selectors sub_822150E0 and sub_82215138 return the first level whose
// distance exceeds the camera distance they are given in f1, else the last.
// RR6's switch distances are 250-775 units and most models have one level.
//
// Cars pick their level in the per-frame car update, sub_82150598, from each
// car's view depth (negative, scaled by the camera's zoom) against two
// thresholds on the car manager: level 1 (full) inside +60 (-30), level 2 out
// to +64 (-50), level 3 (lowest) beyond. In a race nearly every rival ahead
// is at level 3. The camera's own car uses its own model.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_DOUBLE(rr6_lod_distance, 1.0, kCvarGroup,
                      "Multiplies the distances at which course scenery and "
                      "cars switch to lower detail models: 2 keeps full "
                      "detail twice as far out, a large value (100) always "
                      "keeps it");

REX_EXTERN(__imp__sub_822150E0);
REX_EXTERN(__imp__sub_82215138);
REX_EXTERN(__imp__sub_82150598);

namespace {

double LodDistanceScale() {
  const double scale = REXCVAR_GET(rr6_lod_distance);
  return scale > 0.0 ? scale : 1.0;
}

} // namespace

// (record, ...) with the camera distance in f1, for the two course passes.
// The caller reloads f1 before every call.
REX_HOOK_RAW(sub_822150E0) {
  ctx.f1.f64 /= LodDistanceScale();
  __imp__sub_822150E0(ctx, base);
}

REX_HOOK_RAW(sub_82215138) {
  ctx.f1.f64 /= LodDistanceScale();
  __imp__sub_82215138(ctx, base);
}

// (car manager, ...). The thresholds are scaled for the update and restored,
// so the game's own data never changes.
REX_HOOK_RAW(sub_82150598) {
  const double scale = LodDistanceScale();
  auto *near_limit = rr6::mem::at<be_f32>(ctx.r3.u32 + 60);
  auto *far_limit = rr6::mem::at<be_f32>(ctx.r3.u32 + 64);
  if (scale == 1.0 || !near_limit || !far_limit) {
    __imp__sub_82150598(ctx, base);
    return;
  }
  const float near_saved = *near_limit;
  const float far_saved = *far_limit;
  *near_limit = float(near_saved * scale);
  *far_limit = float(far_saved * scale);
  __imp__sub_82150598(ctx, base);
  *near_limit = near_saved;
  *far_limit = far_saved;
}
