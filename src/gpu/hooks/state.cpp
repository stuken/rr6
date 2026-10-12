// rr6 - ReXGlue Recompiled Project
//
// Render target, depth and vertex declaration hooks. Like the shader hooks
// these wrap the guest originals: RR6's D3D keeps its own surfaces and
// declarations, the host keeps a mirror keyed by the guest object, and the
// binding state follows reblue's gpu/hooks/state.cpp (Copyright (c) 2026 Tom
// Clay, BSD 3-Clause).

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/shaders/shader_constants.h"
#include "gpu/surface_registry.h"
#include "gpu/vertex_declaration.h"

REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice_CreateVertexDeclaration);
REX_EXTERN(__imp__D3DDevice_SetVertexDeclaration);
REX_EXTERN(__imp__D3DSurface_GetDesc);

namespace rr6::gpu {
void NoteDefaultSurfaces(u32 back_buffer, u32 depth);
bool ForgetGuestSurface(u32 surface_va);
bool TraceFrameActive();
}

namespace {

using rr6::gpu::GuestTexture;
using rr6::gpu::GuestVertexDeclaration;

std::mutex g_mutex;
std::unordered_map<u32, GuestTexture *> g_surfaces; // guest surface -> host
std::unordered_map<u32, GuestVertexDeclaration *> g_decls;
u32 g_default_back_buffer = 0;
std::atomic<u32> g_bound_render_target{0};

// D3DSURFACE_DESC as XDK 2135's D3DSurface_GetDesc (0x82252FC0) writes it.
struct SurfaceDesc {
  be_u32 Format;
  be_u32 Type;
  be_u32 Usage;
  be_u32 Pool;
  be_u32 MultiSampleType;
  be_u32 MultiSampleQuality;
  be_u32 Width;
  be_u32 Height;
};

// Runs the guest's own GetDesc on an isolated frame, with the descriptor on
// the guest stack below the caller's live frames.
bool GuestSurfaceDesc(PPCContext &ctx, u8 *base, u32 surface,
                      SurfaceDesc &out) {
  rex::CallFrame frame(ctx);
  const u32 desc_va = (ctx.r1.u32 - 0x100) & ~0xFu;
  frame.ctx.r1.u64 = desc_va - 0x80;
  frame.ctx.r3.u64 = surface;
  frame.ctx.r4.u64 = desc_va;
  __imp__D3DSurface_GetDesc(frame.ctx, base);
  const auto *desc = rr6::mem::at<const SurfaceDesc>(desc_va);
  if (!desc)
    return false;
  out = *desc;
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    RR6_INFO("D3DSurface_GetDesc(0x{:08X}): format=0x{:08X} type={} {}x{} "
             "msaa={}",
             surface, u32(out.Format), u32(out.Type), u32(out.Width),
             u32(out.Height), u32(out.MultiSampleType));
  }
  return out.Width != 0 && out.Height != 0;
}

GuestTexture *HostSurface(PPCContext &ctx, u8 *base, u32 surface) {
  if (!surface)
    return nullptr;
  if (surface == g_default_back_buffer)
    return rr6::gpu::state().back_buffer_surface;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_surfaces.find(surface); it != g_surfaces.end())
      return it->second;
  }
  SurfaceDesc desc{};
  if (!GuestSurfaceDesc(ctx, base, surface, desc))
    return nullptr;
  // The host surface is the guest size times the resolution scale.
  const u32 scale = rr6::gpu::Output::ResolutionScale();
  GuestTexture *host = rr6::gpu::SurfaceRegistry::Get(
      u32(desc.Width) * scale, u32(desc.Height) * scale, desc.Format, 1);
  std::lock_guard lock(g_mutex);
  g_surfaces[surface] = host;
  return host;
}

} // namespace

namespace rr6::gpu {

// The device hook creates the default back buffer through the guest and
// names it here, so binding it targets the host back buffer Present shows.
void NoteDefaultSurfaces(u32 back_buffer, u32 /*depth*/) {
  g_default_back_buffer = back_buffer;
}

u32 BoundRenderTargetVa() {
  return g_bound_render_target.load(std::memory_order_relaxed);
}

// D3DResource_Destroy on a surface. The host surface goes back to the
// registry, which reissues a free one to the next surface of the same size and
// format and retires it after it idles, so a surface created later at the
// same address is mapped afresh.
bool ForgetGuestSurface(u32 surface_va) {
  GuestTexture *host = nullptr;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_surfaces.find(surface_va);
    if (it == g_surfaces.end())
      return false;
    host = it->second;
    g_surfaces.erase(it);
  }
  if (host && host != state().back_buffer_surface)
    host->x360.as_surface.resource.ReferenceCount = 0u;
  return true;
}

} // namespace rr6::gpu

namespace {
// Xenos depth shares the colour surface's EDRAM pitch, so a depth surface
// smaller than its colour target still covers it: RR6 pairs its 1280x720
// depth with the 1440x800 race target. D3D12 clips rendering to the smallest
// attachment, so the host depth texture grows to cover whatever colour target
// it is bound with (once; it keeps the larger size). Called outside the state
// lock, which ResizeTexture takes.
void GrowDepthToCoverTarget() {
  auto &s = rr6::gpu::state();
  GuestTexture *rt = s.render_target;
  GuestTexture *ds = s.depth_stencil;
  if (!rt || !ds || !ds->texture || rt->sampleCount != ds->sampleCount)
    return;
  if (ds->width >= rt->width && ds->height >= rt->height)
    return;
  const u32 w = std::max(ds->width, rt->width);
  const u32 h = std::max(ds->height, rt->height);
  RR6_INFO("depth surface grows {}x{} -> {}x{} to cover its render target",
           ds->width, ds->height, w, h);
  rr6::gpu::Video::ResizeTexture(ds, w, h);
  s.draw_framebuffer_bound = false;
}
} // namespace

// (device, index, surface)
REX_HOOK_RAW(D3DDevice_SetRenderTarget) {
  const u32 device_va = ctx.r3.u32;
  const u32 index = ctx.r4.u32;
  const u32 surface_va = ctx.r5.u32;
  __imp__D3DDevice_SetRenderTarget(ctx, base);
  // MRT is not modeled, as in reblue.
  if (index != 0)
    return;
  g_bound_render_target.store(surface_va, std::memory_order_relaxed);
  GuestTexture *surface = HostSurface(ctx, base, surface_va);
  if (rr6::gpu::TraceFrameActive()) {
    SurfaceDesc desc{};
    if (surface_va)
      GuestSurfaceDesc(ctx, base, surface_va, desc);
    RR6_INFO("[trace] SetRenderTarget 0x{:08X} -> {}x{} guest fmt=0x{:08X} "
             "host fmt={} lr=0x{:08X}",
             surface_va, surface ? surface->width : 0,
             surface ? surface->height : 0, u32(desc.Format),
             surface ? static_cast<int>(surface->format) : -1, u32(ctx.lr));
  }
  auto &s = rr6::gpu::state();
  if (s.render_target != surface)
    s.draw_framebuffer_bound = false;
  rr6::gpu::Video::SetDirtyValue<GuestTexture *>(
      s.dirtyStates.renderTargetAndDepthStencil, s.render_target, surface);
  rr6::gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.renderTargetFormat,
      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  rr6::gpu::Video::SetDirtyValue<plume::RenderSampleCounts>(
      s.dirtyStates.pipelineState, s.pipelineState.sampleCount,
      surface ? surface->sampleCount : plume::RenderSampleCount::COUNT_1);
  rr6::gpu::Video::SetDefaultViewport(
      rr6::mem::at<rr6::gpu::D3DDevice>(device_va), surface);
  GrowDepthToCoverTarget();
}

// (device, surface)
REX_HOOK_RAW(D3DDevice_SetDepthStencilSurface) {
  const u32 device_va = ctx.r3.u32;
  const u32 surface_va = ctx.r4.u32;
  __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
  GuestTexture *surface = HostSurface(ctx, base, surface_va);
  if (surface && !rr6::gpu::IsDepthFormat(surface->format))
    surface = nullptr;
  auto &s = rr6::gpu::state();
  if (s.depth_stencil != surface)
    s.draw_framebuffer_bound = false;
  rr6::gpu::Video::SetDirtyValue<GuestTexture *>(
      s.dirtyStates.renderTargetAndDepthStencil, s.depth_stencil, surface);
  rr6::gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.depthStencilFormat,
      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  if (surface)
    s.scene_depth = surface;
  // No viewport reset: on Xenos D3D binding a depth surface leaves the
  // viewport the render target set, and RR6's race depth is smaller than its
  // 1440x800 colour target.
  (void)device_va;
  GrowDepthToCoverTarget();
}

// (pVertexElements) -> D3DVertexDeclaration*
REX_HOOK_RAW(D3DDevice_CreateVertexDeclaration) {
  const u32 elements_va = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexDeclaration(ctx, base);
  const u32 decl_va = ctx.r3.u32;
  auto *elements = rr6::mem::at<rr6::gpu::GuestVertexElement>(elements_va);
  if (!decl_va || !elements)
    return;
  if (auto *decl = rr6::gpu::CreateVertexDeclaration(elements)) {
    std::lock_guard lock(g_mutex);
    g_decls[decl_va] = decl;
  }
}

// (device, declaration)
REX_HOOK_RAW(D3DDevice_SetVertexDeclaration) {
  const u32 decl_va = ctx.r4.u32;
  __imp__D3DDevice_SetVertexDeclaration(ctx, base);
  GuestVertexDeclaration *decl = nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_decls.find(decl_va); it != g_decls.end())
      decl = it->second;
  }
  // The recompiled VS guards its R11G11B10 normal decode on this bit.
  auto &s = rr6::gpu::state();
  u32 spec =
      s.pipelineState.specConstants & ~rr6::gpu::kSpecConstantR11G11B10Normal;
  if (decl && decl->hasR11G11B10Normal)
    spec |= rr6::gpu::kSpecConstantR11G11B10Normal;
  rr6::gpu::Video::SetDirtyValue<u32>(s.dirtyStates.pipelineState,
                                      s.pipelineState.specConstants, spec);
  rr6::gpu::Video::SetVertexDeclaration(decl);
}

