// rr6 - ReXGlue Recompiled Project
//
// Shader hooks for the native renderer. The guest still builds its own shader
// objects: RR6's unhooked D3D code (draws, the GPR allocation, the literal
// constant copies) reads them through device+0x4FE8 / +0x3290 as real XDK
// objects. Alongside each one the host builds a GuestShader from the
// XenosRecomp cache, keyed by the guest object's address, and Set*Shader
// tells Video which host shader is bound. Once draws are hooked the guest
// copies can go.

#include <atomic>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/resources.h"

REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_EXTERN(__imp__D3DDevice_CreatePixelShader);
REX_EXTERN(__imp__D3DDevice_SetVertexShader);
REX_EXTERN(__imp__D3DDevice_SetPixelShader);

namespace {

using rr6::gpu::GuestShader;
using rr6::gpu::ResourceType;

std::mutex g_mutex;
std::unordered_map<u32, GuestShader *> g_by_guest; // guest object VA -> host

struct Stats {
  std::atomic<u32> created{0};
  std::atomic<u32> cache_hits{0};
  std::atomic<u32> host_built{0};
};
Stats g_vs, g_ps;

// RR6's loader (0x82222900) creates every shader once at boot, 122 of each.
constexpr u32 kBootShadersPerStage = 122;

void LogStageSummary(const char *stage, const Stats &st) {
  RR6_INFO("{} shaders: {} created, {} recompiled cache hits, {} host shaders "
           "built",
           stage, st.created.load(), st.cache_hits.load(),
           st.host_built.load());
}

void TrackCreated(u32 guest_obj, u32 container_va, ResourceType type) {
  auto &st = type == ResourceType::VertexShader ? g_vs : g_ps;
  const auto *container = rr6::mem::at<const be_u32>(container_va);
  GuestShader *gs = nullptr;
  if (container)
    gs = rr6::gpu::CreateShader(container, type);
  if (gs) {
    if (gs->shaderCacheEntry)
      st.cache_hits.fetch_add(1, std::memory_order_relaxed);
    // Building the host shader here proves the cached blob loads on this
    // device at boot rather than at the first draw that needs it.
    if (rr6::gpu::GetOrLinkShader(gs, 0))
      st.host_built.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    g_by_guest[guest_obj] = gs;
  }
  if (st.created.fetch_add(1, std::memory_order_relaxed) + 1 ==
      kBootShadersPerStage)
    LogStageSummary(type == ResourceType::VertexShader ? "VS" : "PS", st);
}

GuestShader *Lookup(u32 guest_obj) {
  if (!guest_obj)
    return nullptr;
  std::lock_guard lock(g_mutex);
  auto it = g_by_guest.find(guest_obj);
  return it != g_by_guest.end() ? it->second : nullptr;
}

} // namespace

// (pFunction) -> D3DVertexShader*.
REX_HOOK_RAW(D3DDevice_CreateVertexShader) {
  const u32 container = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexShader(ctx, base);
  if (ctx.r3.u32)
    TrackCreated(ctx.r3.u32, container, ResourceType::VertexShader);
}

REX_HOOK_RAW(D3DDevice_CreatePixelShader) {
  const u32 container = ctx.r3.u32;
  __imp__D3DDevice_CreatePixelShader(ctx, base);
  if (ctx.r3.u32)
    TrackCreated(ctx.r3.u32, container, ResourceType::PixelShader);
}

// (device, shader). The guest body still runs so the device keeps its own
// view (and its literal constant copies) consistent.
REX_HOOK_RAW(D3DDevice_SetVertexShader) {
  rr6::gpu::Video::SetVertexShader(Lookup(ctx.r4.u32));
  __imp__D3DDevice_SetVertexShader(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetPixelShader) {
  rr6::gpu::Video::SetPixelShader(Lookup(ctx.r4.u32));
  __imp__D3DDevice_SetPixelShader(ctx, base);
}
