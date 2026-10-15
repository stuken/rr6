// rr6 - ReXGlue Recompiled Project
//
// Draw hooks for the native renderer. The draw entry points are replaced (the
// guest bodies only build PM4 for the discard ring), while BeginVertices,
// EndVertices and DrawVerticesUP wrap the guest. XDK 2135's DrawVerticesUP
// calls BeginVertices but closes the packet inline rather than through
// EndVertices, so both ends record the pending immediate-mode draw. Vertex and index buffers are still guest-owned, so each draw
// binds host mirrors of whatever the device has set (gpu/guest_buffers.h).
// The recording sequence follows reblue's gpu/hooks/draw.cpp (Copyright (c)
// 2026 Tom Clay, BSD 3-Clause).

#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/system/thread_state.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "core/settings.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/guest_buffers.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/perf_stats.h"

REXCVAR_DEFINE_INT32(rr6_trace_frame, 0, kCvarGroup,
                     "Log every draw and resolve of present frame N (0 = off)");

REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_EndVertices);
REX_EXTERN(__imp__D3DDevice_DrawVerticesUP);

namespace rr6::gpu {
u32 PresentCount();
u32 BoundRenderTargetVa();
}

namespace {

namespace xe = rex::graphics::xenos;
using rr6::gpu::D3DDevice;
using rr6::gpu::GuestBuffer;
using rr6::gpu::ResourceType;
using rr6::gpu::Video;

using rr6::gpu::D3DIndexBuffer;
using rr6::gpu::D3DVertexBuffer;

// Draw counters, reported by LogDrawStats from the Present hook.
struct DrawStats {
  std::atomic<u32> issued{0};
  std::atomic<u32> recorded{0};
  std::atomic<u32> no_framebuffer{0};
  std::atomic<u32> no_state{0};
  std::atomic<u32> no_index_buffer{0};
  std::atomic<u32> textures_resolved{0};
  std::atomic<u32> textures_unresolved{0};
  std::atomic<u32> resolves{0};
  std::mutex rt_mutex;
  std::unordered_map<u64, u32> draws_per_rt; // (w << 32 | h) -> draws
};
DrawStats g_stats;

struct DrawArgs {
  bool indexed = false;
  u32 vertexOrIndexCount = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  u32 startVertex = 0;
};

plume::RenderPrimitiveTopology MapPrimitiveType(u32 prim) {
  switch (static_cast<xe::PrimitiveType>(prim)) {
  // Points are one guest pixel: no point sprite sizes, and DispatchDraw covers
  // the host block at a resolution scale. Lines are one host pixel wide,
  // unscaled; RR6 draws none.
  case xe::PrimitiveType::kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case xe::PrimitiveType::kLineList:
    RR6_WARN_FIRST(1, "MapPrimitiveType: line list drawn one pixel wide");
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case xe::PrimitiveType::kLineStrip:
    RR6_WARN_FIRST(1, "MapPrimitiveType: line strip drawn one pixel wide");
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case xe::PrimitiveType::kTriangleList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleFan:
    // Expanded through Video::FanExpansionIBView.
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  case xe::PrimitiveType::kQuadList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  default: {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4)
      RR6_WARN("MapPrimitiveType: unmapped primitive type {} drawn as a "
               "triangle list",
               prim);
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  }
}

// Pixel samplers 0..15: device.textures[s] pairs with fetch constant s.
void BindGuestTextures(const D3DDevice &dev) {
  for (u32 i = 0; i < 16; ++i) {
    if (const u32 tex_va = dev.textures[i]) {
      auto *tex = rr6::gpu::ResolveOrMirrorGuestTexture(tex_va);
      (tex ? g_stats.textures_resolved : g_stats.textures_unresolved)
          .fetch_add(1, std::memory_order_relaxed);
      Video::SetTexture(i, tex);
    }
  }
}

void CountRenderTarget() {
  auto &s = rr6::gpu::state();
  std::lock_guard rt_lock(g_stats.rt_mutex);
  const u64 key = s.render_target ? (u64(s.render_target->width) << 32 |
                                     s.render_target->height)
                                  : 0;
  ++g_stats.draws_per_rt[key];
}

// The guest return addresses above a hook: lr (if non-zero) first, then the
// back chain. Each frame's first word links to the caller's, and prologues
// save LR at -8 from the caller's stack pointer.
std::string GuestCallChain(u32 sp, u32 lr, u32 depth) {
  std::string chain;
  if (lr)
    chain += std::format(" {:08X}", lr);
  for (u32 i = 0; sp && i < depth; ++i) {
    // The outermost frame links past the top of the stack: checked reads.
    const auto *link = rr6::mem::try_at<const be_u32>(sp);
    if (!link || !u32(*link))
      break;
    sp = u32(*link);
    if (const auto *saved = rr6::mem::try_at<const be_u32>(sp - 8))
      chain += std::format(" {:08X}", u32(*saved));
  }
  return chain;
}

// rr6_trace_frame=N logs every draw of present frame N.
bool TracingThisFrame() {
  const i32 wanted = REXCVAR_GET(rr6_trace_frame);
  return wanted > 0 && rr6::gpu::PresentCount() + 1 == static_cast<u32>(wanted);
}

void TraceDraw(const char *path, const D3DDevice &dev, u32 prim, u32 count) {
  auto &s = rr6::gpu::state();
  auto hash = [](const rr6::gpu::GuestShader *gs) -> u64 {
    return gs && gs->shaderCacheEntry ? gs->shaderCacheEntry->hash : 0;
  };
  std::string tex;
  for (u32 i = 0; i < 16; ++i) {
    const u32 va = dev.textures[i];
    if (!va)
      continue;
    const char *kind = rr6::gpu::FindResolveTarget(va) ? "resolve" : "mirror";
    const auto &fc = dev.fetchConstants[i].dword;
    tex += std::format(" t{}=0x{:08X}({} fmt={} {}x{} swz={:03X} sign={:02X} exp={} dim={})",
                       i, va, kind, u32(fc[1]) & 0x3F,
                       (u32(fc[2]) & 0x1FFF) + 1,
                       ((u32(fc[2]) >> 13) & 0x1FFF) + 1,
                       (u32(fc[3]) >> 1) & 0xFFF, (u32(fc[0]) >> 2) & 0xFF,
                       static_cast<i32>(u32(fc[3]) << 13) >> 26,
                       (u32(fc[5]) >> 9) & 3);
  }
  // The game code that issued the draw.
  if (auto *ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
    const PPCContext &ctx = *ts->context();
    tex += " chain:" + GuestCallChain(ctx.r1.u32, u32(ctx.lr), 6);
  }
  RR6_INFO("[trace] {} prim={} n={} rt={}x{} vs={:016X} ps={:016X} "
           "blend={}:{:08X} depth={:08X} cull={:X} color={:X} aref={:.3f} "
           "poly={:X}:{:.3f},{:.6f} vtx={:X} vte={:X} sc={:08X}:{:08X} ucp={:X}:{:.3f},{:.3f},{:.3f},{:.3f} vp={},{} {}x{} vso={:.1f},{:.1f},{:.1f},{:.1f},{:.3f},{:.3f}{}",
           path, prim, count, s.render_target ? s.render_target->width : 0,
           s.render_target ? s.render_target->height : 0,
           hash(s.vertex_shader), hash(s.pixel_shader),
           (u32(dev.blendFlags) >> 31), u32(dev.rbBlendControl0),
           u32(dev.rbDepthControl), u32(dev.paSuScModeCntl) & 7u,
           u32(dev.rbColorControl), float(dev.rbAlphaRef),
           (u32(dev.paSuScModeCntl) >> 11) & 7u, float(dev.polyOffsetFrontScale),
           float(dev.polyOffsetFrontOffset), u32(dev.paSuVtxCntl), u32(dev.paClVteCntl),
           u32(dev.paScWindowScissorTL), u32(dev.paScWindowScissorBR),
           u32(dev.paClClipCntl) & 0x3Fu, float(dev.clipPlanes[0][0]),
           float(dev.clipPlanes[0][1]), float(dev.clipPlanes[0][2]),
           float(dev.clipPlanes[0][3]),
           u32(dev.viewport.X), u32(dev.viewport.Y), u32(dev.viewport.Width),
           u32(dev.viewport.Height), float(dev.viewportScaleOffset[0]),
           float(dev.viewportScaleOffset[1]), float(dev.viewportScaleOffset[2]),
           float(dev.viewportScaleOffset[3]), float(dev.viewportScaleOffset[4]),
           float(dev.viewportScaleOffset[5]), tex);
}

// The PS constants of a full-screen pass, where post grading and filter taps
// live.
void TracePixelConstants(const D3DDevice &dev) {
  std::string consts;
  for (u32 c : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 32u, 33u, 34u}) {
    const auto &v = dev.psFloatConstants[c];
    consts += std::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", c, float(v[0]),
                          float(v[1]), float(v[2]), float(v[3]));
  }
  RR6_INFO("[trace]   ps consts{}", consts);
}

// Binds host mirrors of the device's vertex streams and index buffer.
void BindGuestGeometry(const D3DDevice &dev, bool indexed) {
  for (u32 i = 0; i < 16; ++i) {
    const u32 offset = dev.streams[i][0];
    const u32 vb_va = dev.streams[i][1];
    const u32 stride = u32(dev.streamStrides[i]) * 4;
    const auto *vb = rr6::mem::at<const D3DVertexBuffer>(vb_va);
    if (!vb_va || !vb) {
      Video::SetVertexStream(i, {}, 0, stride);
      continue;
    }
    const u32 base = u32(vb->Fetch0) & ~0x3u;
    const u32 size = u32(vb->Fetch1) & 0x03FFFFFCu;
    GuestBuffer *mirror =
        rr6::gpu::MirrorGuestBuffer(base, size, ResourceType::VertexBuffer, 4,
                                    plume::RenderFormat::UNKNOWN);
    if (!mirror || !mirror->hasBuffer() || offset >= size) {
      Video::SetVertexStream(i, {}, 0, stride);
      continue;
    }
    Video::SetVertexStream(i, mirror->bufferRef(offset), size - offset, stride);
  }
  BindGuestTextures(dev);
  if (!indexed)
    return;
  const auto *ib = rr6::mem::at<const D3DIndexBuffer>(dev.indexBuffer);
  GuestBuffer *mirror = nullptr;
  if (dev.indexBuffer && ib) {
    const bool index32 =
        (u32(ib->resource.Common) & rr6::gpu::kCommonIndex32) != 0;
    mirror = rr6::gpu::MirrorGuestBuffer(
        ib->Address, ib->Size, ResourceType::IndexBuffer, index32 ? 4 : 2,
        index32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  }
  Video::SetIndices(mirror);
}

// Expands the guest's indexed quad list into a triangle list (each quad
// i0 i1 i2 i3 as i0 i1 i2, i0 i2 i3, the order QuadlistExpansionIBView uses)
// in the upload heap and draws it. Caller holds the state mutex.
bool DrawIndexedQuadList(const D3DDevice &dev, const DrawArgs &args, u32 quads) {
  const auto *ibh = rr6::mem::at<const D3DIndexBuffer>(dev.indexBuffer);
  if (!quads || !dev.indexBuffer || !ibh)
    return false;
  const bool index32 =
      (u32(ibh->resource.Common) & rr6::gpu::kCommonIndex32) != 0;
  const u32 index_size = index32 ? 4u : 2u;
  const u32 ib_base = u32(ibh->Address);
  const u32 ib_size = u32(ibh->Size);
  if (u64(args.startIndex + quads * 4) * index_size > ib_size)
    return false;
  const auto *src =
      rr6::mem::at<const u8>(ib_base + args.startIndex * index_size);
  if (!src)
    return false;
  std::vector<u32> tris(size_t(quads) * 6);
  auto read = [&](u32 i) -> u32 {
    return index32 ? u32(reinterpret_cast<const be_u32 *>(src)[i])
                   : u32(u16(reinterpret_cast<const be_u16 *>(src)[i]));
  };
  for (u32 q = 0; q < quads; ++q) {
    const u32 i0 = read(q * 4), i1 = read(q * 4 + 1), i2 = read(q * 4 + 2),
              i3 = read(q * 4 + 3);
    u32 *o = tris.data() + size_t(q) * 6;
    o[0] = i0;
    o[1] = i1;
    o[2] = i2;
    o[3] = i0;
    o[4] = i2;
    o[5] = i3;
  }
  const u32 bytes = static_cast<u32>(tris.size() * sizeof(u32));
  auto alloc = rr6::gpu::UploadHostBytes(tris.data(), bytes, 4);
  if (!alloc.memory)
    return false;
  auto &s = rr6::gpu::state();
  const plume::RenderIndexBufferView view(alloc.ref, bytes,
                                          plume::RenderFormat::R32_UINT);
  s.command_list->setIndexBuffer(&view);
  s.command_list->drawIndexedInstanced(quads * 6, 1, 0, args.baseVertexIndex,
                                       0);
  return true;
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const DrawArgs &args) {
  rr6::gpu::perf::Scope timer(rr6::gpu::perf::Counter::Draw);
  RR6_CPU_ZONE("Draw");
  const auto *dev = rr6::mem::at<const D3DDevice>(device_guest);
  if (!dev || !args.vertexOrIndexCount)
    return;
  g_stats.issued.fetch_add(1, std::memory_order_relaxed);
  if (TracingThisFrame()) {
    TraceDraw(args.indexed ? "DrawIndexed" : "Draw", *dev, primitive_type,
              args.vertexOrIndexCount);
    // Full-screen passes: their PS constants (post grading lives there).
    if (!args.indexed && args.vertexOrIndexCount <= 6)
      TracePixelConstants(*dev);
    // The first few scene draws: every non-zero VS/PS float constant below
    // c64, enough to sanity-check lighting and fog parameters.
    static u32 s_dumped_frame = 0, s_dumped = 0;
    if (s_dumped_frame != rr6::gpu::PresentCount()) {
      s_dumped_frame = rr6::gpu::PresentCount();
      s_dumped = 0;
    }
    if (args.indexed && s_dumped++ < 3) {
      for (int stage = 0; stage < 2; ++stage) {
        std::string consts;
        for (u32 c = 0; c < 64; ++c) {
          const auto &v = stage ? dev->psFloatConstants[c] : dev->vsFloatConstants[c];
          const float f[4] = {float(v[0]), float(v[1]), float(v[2]), float(v[3])};
          if (f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0)
            continue;
          consts += std::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", c, f[0],
                                f[1], f[2], f[3]);
        }
        RR6_INFO("[trace]   {} consts{}", stage ? "ps" : "vs", consts);
      }
    }
  }
  BindGuestGeometry(*dev, args.indexed);

  // One lock across the whole recording sequence, as in reblue: the
  // per-frame command list is single-producer.
  auto &s = rr6::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  Video::OpenCommandListLocked();
  Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(primitive_type));
  if (!Video::BindDrawFramebufferLocked()) {
    g_stats.no_framebuffer.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!Video::FlushRenderStateLocked(device_guest)) {
    g_stats.no_state.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto *cmd_list = s.command_list;
  if (!cmd_list)
    return;

  if (static_cast<xe::PrimitiveType>(primitive_type) ==
      xe::PrimitiveType::kQuadList) {
    u32 quads = args.vertexOrIndexCount / 4;
    quads = std::min(quads, Video::QuadlistMaxQuads());
    const auto *ib = Video::QuadlistExpansionIBView();
    if (args.indexed) {
      // An indexed quad list (the nitrous radial blur's grid) names its own
      // corners, so split the guest's quads rather than consecutive vertices.
      if (DrawIndexedQuadList(*dev, args, quads))
        s.dirtyStates.indices = true;
    } else if (quads && ib) {
      const i32 first = args.indexed ? args.baseVertexIndex
                                     : static_cast<i32>(args.startVertex);
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, first, 0);
      s.dirtyStates.indices = true;
    }
  } else if (static_cast<xe::PrimitiveType>(primitive_type) ==
                 xe::PrimitiveType::kTriangleFan &&
             !args.indexed) {
    const u32 tris = std::min(args.vertexOrIndexCount >= 3
                                  ? args.vertexOrIndexCount - 2
                                  : 0u,
                              Video::FanMaxTriangles());
    if (const auto *ib = Video::FanExpansionIBView(); tris && ib) {
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(tris * 3, 1, 0,
                                     static_cast<i32>(args.startVertex), 0);
      s.dirtyStates.indices = true;
    }
  } else if (args.indexed) {
    if (static_cast<xe::PrimitiveType>(primitive_type) ==
        xe::PrimitiveType::kTriangleFan) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4)
        RR6_WARN("indexed triangle fan drawn as a triangle list");
    }
    if (s.index_view.buffer.ref == nullptr) {
      g_stats.no_index_buffer.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    cmd_list->drawIndexedInstanced(args.vertexOrIndexCount, 1, args.startIndex,
                                   args.baseVertexIndex, 0);
  } else if (static_cast<xe::PrimitiveType>(primitive_type) ==
                 xe::PrimitiveType::kPointList &&
             rr6::gpu::TargetScaleLocked(s) > 1) {
    // A one-pixel guest point covers scale x scale host pixels. Host points
    // are one pixel, so draw the list once per host pixel of the block, each
    // pass moved onto that pixel's centre. RR6's only point draw is the sun
    // visibility count: 64 additive points summing the probe into one pixel
    // that the glow reads back bilinearly, so every host pixel of the block
    // needs the full sum.
    const u32 scale = rr6::gpu::TargetScaleLocked(s);
    const float centre = 0.5f * float(scale) - 0.5f;
    for (u32 y = 0; y < scale; ++y) {
      for (u32 x = 0; x < scale; ++x) {
        rr6::gpu::SetHostPixelNudge(float(x) - centre, float(y) - centre);
        if (!Video::FlushRenderStateLocked(device_guest))
          break;
        cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex,
                                0);
      }
    }
    rr6::gpu::SetHostPixelNudge(0.0f, 0.0f);
  } else {
    cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex, 0);
  }
  g_stats.recorded.fetch_add(1, std::memory_order_relaxed);
  CountRenderTarget();
}

u32 D3DDevice_DrawVertices_hook(u32 device, u32 primitive_type,
                                u32 start_vertex, u32 vertex_count) {
  DrawArgs args;
  args.vertexOrIndexCount = vertex_count;
  args.startVertex = start_vertex;
  DispatchDraw(device, primitive_type, args);
  return 0;
}

// All five arguments are marshaled: IndexCount is in r7.
u32 D3DDevice_DrawIndexedVertices_hook(u32 device, u32 primitive_type,
                                       u32 base_vertex, u32 start_index,
                                       u32 index_count) {
  DrawArgs args;
  args.indexed = true;
  args.vertexOrIndexCount = index_count;
  args.baseVertexIndex = static_cast<i32>(base_vertex);
  args.startIndex = start_index;
  DispatchDraw(device, primitive_type, args);
  return 0;
}

// BeginVertices hands the game a pointer it fills, EndVertices draws it. The
// guest allocator returns discard command memory that stays valid until the
// next wrap, which is far longer than one Begin/End pair.
struct PendingUP {
  u32 device = 0;
  u32 primitive_type = 0;
  u32 vertex_count = 0;
  u32 stride = 0;
  u32 data = 0;
};
thread_local PendingUP g_pending;

} // namespace

namespace rr6::gpu {

// For the other hooks' rr6_trace_frame lines.
bool TraceFrameActive() { return TracingThisFrame(); }

// Draw totals so far, logged by the Present hook under rr6_devmode.
void LogDrawStats(u32 frame) {
  RR6_DEV_INFO("draws after frame {}: {} issued, {} recorded, skipped {} no "
           "framebuffer, {} state, {} no index buffer",
           frame, g_stats.issued.load(), g_stats.recorded.load(),
           g_stats.no_framebuffer.load(), g_stats.no_state.load(),
           g_stats.no_index_buffer.load());
  std::string rts;
  {
    std::lock_guard lock(g_stats.rt_mutex);
    for (const auto &[size, n] : g_stats.draws_per_rt)
      rts += std::format(" {}x{}:{}", size >> 32, size & 0xFFFFFFFFu, n);
  }
  RR6_DEV_INFO("  textures {} resolved, {} unresolved, {} resolves; draws per "
               "RT:{}",
               g_stats.textures_resolved.load(),
               g_stats.textures_unresolved.load(), g_stats.resolves.load(), rts);
}

} // namespace rr6::gpu

// (device, primitive type, vertex count, stride) -> vertex pointer
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  PendingUP p{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0};
  __imp__D3DDevice_BeginVertices(ctx, base);
  p.data = ctx.r3.u32;
  if (TracingThisFrame())
    RR6_INFO("[trace] BeginVertices prim={} n={} stride={} data=0x{:08X}",
             p.primitive_type, p.vertex_count, p.stride, p.data);
  g_pending = p;
}

namespace {
// Records the draw BeginVertices left pending, once its vertices are written.
void FlushPendingUP() {
  const PendingUP p = g_pending;
  g_pending = {};
  if (!p.data || !p.vertex_count || !p.stride)
    return;
  auto alloc = rr6::gpu::UploadGuestBytesByteSwap32(
      p.data, p.vertex_count * p.stride, /*alignment=*/4);
  if (!alloc.memory)
    return;
  if (const auto *dev = rr6::mem::at<const D3DDevice>(p.device)) {
    if (TracingThisFrame()) {
      TraceDraw("DrawUP", *dev, p.primitive_type, p.vertex_count);
      if (p.vertex_count <= 6) {
        std::string verts;
        const auto *f = rr6::mem::at<const be_f32>(p.data);
        for (u32 i = 0; f && i < p.vertex_count * p.stride / 4; ++i)
          verts += std::format("{}{:.4f}", i % (p.stride / 4) ? "," : " | ",
                               float(f[i]));
        RR6_INFO("[trace]   verts{}", verts);
        TracePixelConstants(*dev);
      }
    }
    BindGuestTextures(*dev);
  }
  Video::SetVertexStream(0, alloc.ref, alloc.size, p.stride);
  // Streams 1..15 of the device must not leak into an immediate-mode draw.
  for (u32 i = 1; i < 16; ++i)
    Video::SetVertexStream(i, {}, 0, 0);

  auto &s = rr6::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  Video::OpenCommandListLocked();
  Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(p.primitive_type));
  g_stats.issued.fetch_add(1, std::memory_order_relaxed);
  if (!Video::BindDrawFramebufferLocked()) {
    g_stats.no_framebuffer.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!Video::FlushRenderStateLocked(p.device) || !s.command_list) {
    g_stats.no_state.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_stats.recorded.fetch_add(1, std::memory_order_relaxed);
  CountRenderTarget();
  if (static_cast<xe::PrimitiveType>(p.primitive_type) ==
      xe::PrimitiveType::kQuadList) {
    const u32 quads = std::min(p.vertex_count / 4, Video::QuadlistMaxQuads());
    if (const auto *ib = Video::QuadlistExpansionIBView(); quads && ib) {
      s.command_list->setIndexBuffer(ib);
      s.command_list->drawIndexedInstanced(quads * 6, 1, 0, 0, 0);
      s.dirtyStates.indices = true;
    }
  } else if (static_cast<xe::PrimitiveType>(p.primitive_type) ==
             xe::PrimitiveType::kTriangleFan) {
    const u32 tris = std::min(p.vertex_count >= 3 ? p.vertex_count - 2 : 0u,
                              Video::FanMaxTriangles());
    if (const auto *ib = Video::FanExpansionIBView(); tris && ib) {
      s.command_list->setIndexBuffer(ib);
      s.command_list->drawIndexedInstanced(tris * 3, 1, 0, 0, 0);
      s.dirtyStates.indices = true;
    }
  } else {
    s.command_list->drawInstanced(p.vertex_count, 1, 0, 0);
  }
}

} // namespace

// (device)
REX_HOOK_RAW(D3DDevice_EndVertices) {
  __imp__D3DDevice_EndVertices(ctx, base);
  FlushPendingUP();
}

// (device, primitive type, vertex count, data, stride)
REX_HOOK_RAW(D3DDevice_DrawVerticesUP) {
  g_pending = {};
  __imp__D3DDevice_DrawVerticesUP(ctx, base);
  FlushPendingUP();
}

// (device, flags, src rect, dest texture, dest point, level, face, clear
// color, ClearZ in f1, stencil, params). The guest body only builds PM4.
// A resolve into a texture copies the bound render target into that texture's
// host resolve target, created at the source's format. Afterwards the guest
// clears through ClearF when asked: D3DRESOLVE_CLEARRENDERTARGET (0x100)
// clears the resolved target's color from pClearColor (four floats),
// D3DRESOLVE_CLEARDEPTHSTENCIL (0x200) depth from f1 and stencil from the
// caller's stack at r1+92 (0x82258980 reads 508(r1) under a 416-byte frame).
// Depth resolves are not modeled yet, as in reblue.
constexpr u32 kResolveClearRenderTarget = 0x100;
constexpr u32 kResolveClearDepthStencil = 0x200;

u32 FloatColorToArgb(const be_f32 *c) {
  auto byte = [](float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<u32>(v * 255.0f + 0.5f);
  };
  return byte(c[3]) << 24 | byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
}

REX_HOOK_RAW(D3DDevice_Resolve) {
  (void)base;
  const u32 flags = ctx.r4.u32;
  const u32 dest = ctx.r6.u32;
  const u32 level = ctx.r8.u32;
  const u32 face = ctx.r9.u32;
  const u32 clear_color_va = ctx.r10.u32;
  const double clear_z = ctx.f1.f64;
  const u32 *stencil_slot = rr6::mem::at<const u32>(ctx.r1.u32 + 92);
  const u32 clear_stencil =
      stencil_slot ? u32(*reinterpret_cast<const be_u32 *>(stencil_slot)) : 0;
  ctx.r3.u64 = 0;

  rr6::gpu::GuestTexture *src = rr6::gpu::state().render_target;
  if (TracingThisFrame()) {
    // D3DRECT {x1,y1,x2,y2}, D3DPOINT {x,y}; the surface words carry the
    // EDRAM base and pitch the guest resolves from.
    std::string rect = "full", point = "0,0", words;
    if (const auto *r = rr6::mem::at<const be_u32>(ctx.r5.u32); r && ctx.r5.u32)
      rect = std::format("{},{}-{},{}", u32(r[0]), u32(r[1]), u32(r[2]), u32(r[3]));
    if (const auto *p = rr6::mem::at<const be_u32>(ctx.r7.u32); p && ctx.r7.u32)
      point = std::format("{},{}", u32(p[0]), u32(p[1]));
    point += " chain:" + GuestCallChain(ctx.r1.u32, 0, 6);
    const u32 surface_va = rr6::gpu::BoundRenderTargetVa();
    if (const auto *w = rr6::mem::at<const be_u32>(surface_va); w && surface_va)
      for (int i = 12; i < 15; ++i)
        words += std::format(" {:08X}", u32(w[i]));
    RR6_INFO("[trace] Resolve flags=0x{:X} dest=0x{:08X} src={}x{} rect={} "
             "point={} surface=0x{:08X}:{} lr=0x{:08X}",
             flags, dest, src ? src->width : 0, src ? src->height : 0, rect,
             point, surface_va, words, u32(ctx.lr));
  }
  if (dest && (flags & rr6::gpu::kResolveDepthStencil))
    RR6_WARN_FIRST(4, "Resolve: depth resolve into 0x{:08X} not modeled "
                   "(flags=0x{:X})", dest, flags);
  // The guest clears only the resolved rect; the host clears the whole target.
  if (ctx.r5.u32 &&
      (flags & (kResolveClearRenderTarget | kResolveClearDepthStencil)))
    RR6_WARN_FIRST(4, "Resolve: clear limited to a source rect clears the "
                   "whole target (flags=0x{:X})", flags);
  if (dest && !(flags & rr6::gpu::kResolveDepthStencil) && src &&
      src->texture) {
    if (auto *dst = rr6::gpu::GetOrCreateResolveTarget(dest, src->format)) {
      Video::TrackResolveSource(flags, dst, level, face);
      // D3DRECT {x1,y1,x2,y2} in surface pixels and D3DPOINT {x,y}.
      plume::RenderRect rect;
      const plume::RenderRect *rect_ptr = nullptr;
      if (const auto *r = rr6::mem::at<const be_i32>(ctx.r5.u32); r && ctx.r5.u32) {
        rect = plume::RenderRect(i32(r[0]), i32(r[1]), i32(r[2]), i32(r[3]));
        rect_ptr = &rect;
      }
      u32 point_x = 0, point_y = 0;
      if (const auto *p = rr6::mem::at<const be_u32>(ctx.r7.u32); p && ctx.r7.u32) {
        point_x = p[0];
        point_y = p[1];
      }
      Video::ResolveRtToTexture(dst, rect_ptr, point_x, point_y);
      g_stats.resolves.fetch_add(1, std::memory_order_relaxed);
    }
  }

  u32 clear_flags = 0;
  u32 color = 0;
  if (flags & kResolveClearRenderTarget) {
    clear_flags |= rr6::gpu::kClearTarget;
    if (const auto *c = rr6::mem::at<const be_f32>(clear_color_va))
      color = FloatColorToArgb(c);
  }
  if (flags & kResolveClearDepthStencil)
    clear_flags |= rr6::gpu::kClearZBuffer | rr6::gpu::kClearStencil;
  if (clear_flags)
    Video::Clear(clear_flags, color, static_cast<float>(clear_z), clear_stencil);
}

REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
