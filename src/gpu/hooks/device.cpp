// rr6 - ReXGlue Recompiled Project
//
// Device lifecycle hooks for the native renderer: Direct3D_CreateDevice builds
// the guest D3DDevice while plume owns the real swapchain, Clear and Present
// drive the host frame, and D3D__MakeSpace turns the guest command ring into a
// discard buffer. Modelled on reblue's gpu/hooks/device.cpp (Copyright (c) 2026
// Tom Clay, BSD 3-Clause), against XDK 2.0.2135's layout (config/d3d.toml).

#include <atomic>

#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"

namespace rr6::gpu {
void NoteDefaultSurfaces(u32 back_buffer, u32 depth);
void AdvanceGuestBufferFrame();
void LogDrawStats(u32 frame);
bool TraceFrameActive();
} // namespace rr6::gpu

namespace {

// Nothing consumes the guest's PM4 ring: every guest D3D function still
// running unhooked writes its packets here and D3D__MakeSpace rewinds them.
// The limit leaves half the buffer as headroom for the largest single batch
// written between two limit checks.
constexpr u32 kDiscardRingSize = 4u << 20;
constexpr u32 kDiscardRingLimit = kDiscardRingSize / 2;
u32 g_discard_ring = 0;

// Command memory D3D__AllocCommandMemory hands out (inline vertex data,
// clear and resolve constants), bump-allocated and wrapped. The guest only
// writes it, so reuse after a wrap is harmless while nothing reads it back.
constexpr u32 kDiscardCommandSize = 8u << 20;
u32 g_discard_command = 0;
u32 g_discard_command_used = 0;

// xboxkrnl's VdGlobalDevice (ordinal 0x1BE), imported at this thunk. The real
// D3DDevice_Initialize publishes the device there and D3D's resource code
// reads it back (title processes; system processes use VdGlobalXamDevice).
constexpr u32 kVdGlobalDeviceImport = 0x82000598;

REX_IMPORT(__imp__D3D__InitializeStateTables, InitializeStateTables,
           void(u32));
REX_IMPORT(__imp__D3DDevice_CreateSurface, CreateSurface,
           u32(u32, u32, u32, u32, u32));
REX_IMPORT(__imp__D3DDevice_SetRenderTarget, SetRenderTarget,
           void(u32, u32, u32));
REX_IMPORT(__imp__D3DDevice_SetDepthStencilSurface, SetDepthStencilSurface,
           void(u32, u32));

// D3DPRESENT_PARAMETERS, the fields the device's default surfaces need.
struct PresentParameters {
  be_u32 BackBufferWidth;        // +0x00
  be_u32 BackBufferHeight;       // +0x04
  be_u32 BackBufferFormat;       // +0x08
  be_u32 BackBufferCount;        // +0x0C
  be_u32 MultiSampleType;        // +0x10
  be_u32 MultiSampleQuality;     // +0x14
  be_u32 SwapEffect;             // +0x18
  be_u32 hDeviceWindow;          // +0x1C
  be_u32 Windowed;               // +0x20
  be_u32 EnableAutoDepthStencil; // +0x24
  be_u32 AutoDepthStencilFormat; // +0x28
};

// The real D3DDevice_Initialize binds a back buffer at RT0 (and a depth
// buffer when asked) before returning, and RR6 reads RT0 back straight away
// (D3DDevice_GetRenderTarget then D3DResource_Release, 0x82221A18). The
// guest's own CreateSurface/Set* keep its bookkeeping consistent; the device
// holds the creation references.
void BindDefaultSurfaces(u32 device_va, u32 params_va) {
  const auto *pp = rr6::mem::at<const PresentParameters>(params_va);
  if (!pp)
    return;
  const u32 width = pp->BackBufferWidth ? u32(pp->BackBufferWidth) : 1280u;
  const u32 height = pp->BackBufferHeight ? u32(pp->BackBufferHeight) : 720u;
  const u32 back = CreateSurface(width, height, pp->BackBufferFormat,
                                 pp->MultiSampleType, 0);
  if (back)
    SetRenderTarget(device_va, 0, back);
  u32 depth = 0;
  if (pp->EnableAutoDepthStencil) {
    depth = CreateSurface(width, height, pp->AutoDepthStencilFormat,
                          pp->MultiSampleType, 0);
    if (depth)
      SetDepthStencilSurface(device_va, depth);
  }
  rr6::gpu::NoteDefaultSurfaces(back, depth);
  RR6_INFO("Direct3D_CreateDevice: {}x{} back buffer 0x{:08X} (format "
           "0x{:08X}), depth 0x{:08X} (format 0x{:08X})",
           width, height, back, u32(pp->BackBufferFormat), depth,
           u32(pp->AutoDepthStencilFormat));
}

u32 Direct3D_CreateDevice_hook(u32 /*adapter*/, u32 /*device_type*/,
                               u32 /*focus_window*/, u32 behavior_flags,
                               u32 present_params, mapped_u32 out_device) {
  auto *memory = REX_KERNEL_MEMORY();
  const u32 device_va =
      memory->SystemHeapAlloc(rr6::gpu::kD3DDeviceAllocSize, 0x80);
  if (!g_discard_ring)
    g_discard_ring = memory->SystemHeapAlloc(kDiscardRingSize, 0x1000);
  if (!g_discard_command)
    g_discard_command = memory->SystemHeapAlloc(kDiscardCommandSize, 0x1000);
  if (!device_va || !g_discard_ring || !g_discard_command) {
    RR6_ERROR("Direct3D_CreateDevice: guest allocation failed");
    return 0x8007000E; // E_OUTOFMEMORY, as the guest returns
  }
  memory->Zero(device_va, rr6::gpu::kD3DDeviceAllocSize);

  auto *device = rr6::mem::at<rr6::gpu::D3DDevice>(device_va);
  device->m_pRing = g_discard_ring;
  device->m_pRingEnd = g_discard_ring + kDiscardRingSize;
  device->m_pRingLimit = g_discard_ring + kDiscardRingLimit;
  device->m_ReferenceCount = 1;
  device->behaviorFlags = behavior_flags;

  if (auto *global = rr6::mem::at<const be_u32>(kVdGlobalDeviceImport)) {
    if (auto *slot = rr6::mem::at<be_u32>(global->get()))
      *slot = device_va;
  }

  // The guest's own seeding: copies both dispatch tables in, calls every Set
  // with its default so the register shadows start valid, and clears all
  // texture slots. Its packets land in the discard ring.
  InitializeStateTables(device_va);
  BindDefaultSurfaces(device_va, present_params);

  device->viewport.Width = 1280;
  device->viewport.Height = 720;
  device->viewport.MaxZ = 1.0f;

  if (out_device)
    *out_device = device_va;
  RR6_INFO("Direct3D_CreateDevice: guest device at 0x{:08X}", device_va);
  return 0;
}

// The Xenon ABI reserves a GPR slot for every float argument, so Z in f1 also
// consumes r8 while SDK marshaling numbers integer args without skipping it.
// The placeholder absorbs r8 so Stencil reads r9 (0x82253D50 passes r9 on).
u32 D3DDevice_Clear_hook(u32 /*device*/, u32 count, u32 rects, u32 flags,
                         u32 color, f64 z, u32 /*z_gpr_slot*/, u32 stencil) {
  if (rr6::gpu::TraceFrameActive())
    RR6_INFO("[trace] Clear flags=0x{:X} color=0x{:08X} z={}", flags, color, z);
  // D3DRECT {x1,y1,x2,y2}. Rects aren't modeled: the whole target clears.
  if (count && rects) {
    if (const auto *r = rr6::mem::at<const be_i32>(rects))
      RR6_WARN_FIRST(4, "Clear: {} rect(s) ignored, first {},{}-{},{} "
                     "flags=0x{:X}", count, i32(r[0]), i32(r[1]), i32(r[2]),
                     i32(r[3]), flags);
  }
  rr6::gpu::Video::Clear(flags, color, static_cast<float>(z), stencil);
  return 0;
}

// XDK 2135's per-frame entry: resolves the back buffer to the front buffer and
// swaps. Plume owns the swapchain, so the host presents directly.
std::atomic<u32> g_presents{0};

u32 D3DDevice_Present_hook(u32 /*device*/) {
  const u32 s_presents = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (s_presents == 1)
    RR6_INFO("D3DDevice_Present: first frame");
  if (s_presents == 60 || (s_presents % 600) == 0)
    rr6::gpu::LogDrawStats(s_presents);
  rr6::gpu::Video::Present(nullptr);
  rr6::gpu::AdvanceGuestBufferFrame();
  return 0;
}

// Callers compare the ring pointer against the limit and call this when a
// batch would cross it, then write from the returned pointer.
u32 D3D__MakeSpace_hook(rex::MappedPtr<rr6::gpu::D3DDevice> device) {
  if (device) {
    device->m_pRing = g_discard_ring;
    device->m_pRingEnd = g_discard_ring + kDiscardRingSize;
  }
  return g_discard_ring;
}

// The real allocator asks the callback D3DDevice_Initialize installs at
// device+0x3534 for space in the next ring segment. With no segments to
// manage, hand out discard memory with the requested alignment instead.
u32 D3D__AllocCommandMemory_hook(u32 /*device*/, u32 dwords, u32 align) {
  const u32 bytes = dwords * 4;
  const u32 alignment = align ? align : 4;
  if (bytes > kDiscardCommandSize / 2) {
    RR6_ERROR("D3D__AllocCommandMemory: {} bytes exceeds the discard buffer",
              bytes);
    return 0;
  }
  u32 offset = (g_discard_command_used + alignment - 1) & ~(alignment - 1);
  if (offset + bytes > kDiscardCommandSize)
    offset = 0;
  g_discard_command_used = offset + bytes;
  return g_discard_command + offset;
}

} // namespace

namespace rr6::gpu {

// Frames the guest has presented, for tools that script against frame
// numbers (platform/input_script.cpp).
u32 PresentCount() { return g_presents.load(std::memory_order_relaxed); }

} // namespace rr6::gpu

REX_HOOK(D3D__AllocCommandMemory, D3D__AllocCommandMemory_hook);
REX_HOOK(Direct3D_CreateDevice, Direct3D_CreateDevice_hook);
REX_HOOK(D3DDevice_Clear, D3DDevice_Clear_hook);
REX_HOOK(D3DDevice_Present, D3DDevice_Present_hook);
REX_HOOK(D3D__MakeSpace, D3D__MakeSpace_hook);
