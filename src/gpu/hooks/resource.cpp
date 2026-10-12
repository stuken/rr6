// rr6 - ReXGlue Recompiled Project
//
// Resource lifetime for the native renderer. RR6's D3D still creates its own
// buffers, textures and surfaces, and the host mirrors them by guest address
// (gpu/guest_buffers.h, gpu/native_texture_mirror.h, hooks/state.cpp).
// D3DResource_Destroy, which D3DResource_Release and the device's own
// unbinding call once a count reaches zero, is where those addresses die, so
// it drops the mirrors before the guest frees the memory. Without it a texture
// created later at a freed header's address, with the same fetch constant (a
// course reloaded into the same heap), would keep the old mirror, and a freed
// render texture's resolve target would shadow whatever replaced it.
//
// Course and menu textures never reach D3DResource_Destroy: RR6 builds their
// headers inside loaded data and frees the memory wholesale. So XPhysicalAlloc
// allocations are tracked too, and freeing one drops every texture and buffer
// mirror whose data lay inside it.

#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"
#include "gpu/d3d.h"
#include "gpu/guest_buffers.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/resources.h"

REXCVAR_DEFINE_BOOL(rr6_log_destroy, false, kCvarGroup,
                    "Log every D3DResource_Destroy and the mirror it drops");

REX_EXTERN(__imp__D3DResource_Destroy);
// XPhysicalAlloc(size, address, alignment, protect) -> va, and XPhysicalFree(va)
// (MmAllocatePhysicalMemoryEx / MmFreePhysicalMemory, their only callers).
REX_EXTERN(__imp__sub_82248480);
REX_EXTERN(__imp__sub_82248508);

namespace rr6::gpu {
bool ForgetGuestSurface(u32 surface_va);
}

namespace {

// D3DResource_Destroy (0x82254720) switches on Common bits 16-18.
enum : u32 {
  kTypeVertexBuffer = 1,
  kTypeIndexBuffer = 2,
  kTypeTexture = 3,
  kTypeSurface = 4,
};

// The buffer header past the common D3DResource fields: a vertex buffer keeps
// its fetch constant's first two words at +12/+16 (base | type, size), an
// index buffer its raw base and size (hooks/draw.cpp reads the same words).
struct GuestBufferWords {
  be_u32 Common;
  be_u32 ReferenceCount;
  be_u32 Fence;
  be_u32 word12;
  be_u32 word16;
};

const char *TypeName(u32 type) {
  switch (type) {
  case kTypeVertexBuffer:
    return "vertex buffer";
  case kTypeIndexBuffer:
    return "index buffer";
  case kTypeTexture:
    return "texture";
  case kTypeSurface:
    return "surface";
  default:
    return "resource";
  }
}

void EvictMirrors(u32 va) {
  const auto *hdr = rr6::mem::at<const GuestBufferWords>(va);
  if (!hdr)
    return;
  const u32 type = (u32(hdr->Common) >> 16) & 7u;
  bool evicted = false;
  switch (type) {
  case kTypeVertexBuffer:
    evicted = rr6::gpu::EvictGuestBuffer(u32(hdr->word12) & ~0x3u,
                                         rr6::gpu::ResourceType::VertexBuffer);
    break;
  case kTypeIndexBuffer:
    evicted = rr6::gpu::EvictGuestBuffer(u32(hdr->word12),
                                         rr6::gpu::ResourceType::IndexBuffer);
    break;
  case kTypeTexture:
    evicted = rr6::gpu::EvictGuestTexture(va);
    break;
  case kTypeSurface:
    evicted = rr6::gpu::ForgetGuestSurface(va);
    break;
  default:
    break;
  }
  if (REXCVAR_GET(rr6_log_destroy))
    RR6_INFO("D3DResource_Destroy 0x{:08X}: {} (Common 0x{:08X}){}", va,
             TypeName(type), u32(hdr->Common), evicted ? ", mirror dropped" : "");
}

// Live XPhysicalAlloc allocations, virtual base -> size. Fetch constants and
// buffer headers name their data by these virtual addresses.
std::mutex g_alloc_mutex;
std::unordered_map<u32, u32> g_allocs;

} // namespace

// (resource). Runs before the guest frees the header and its memory.
REX_HOOK_RAW(D3DResource_Destroy) {
  EvictMirrors(ctx.r3.u32);
  __imp__D3DResource_Destroy(ctx, base);
}

REX_HOOK_RAW(sub_82248480) {
  const u32 size = ctx.r3.u32;
  __imp__sub_82248480(ctx, base);
  if (!ctx.r3.u32 || !size)
    return;
  std::lock_guard lock(g_alloc_mutex);
  g_allocs[ctx.r3.u32] = size;
}

REX_HOOK_RAW(sub_82248508) {
  const u32 va = ctx.r3.u32;
  u32 size = 0;
  {
    std::lock_guard lock(g_alloc_mutex);
    if (auto it = g_allocs.find(va); it != g_allocs.end()) {
      size = it->second;
      g_allocs.erase(it);
    }
  }
  if (size) {
    const u32 textures = rr6::gpu::EvictGuestTexturesInRange(va, va + size);
    const u32 buffers = rr6::gpu::EvictGuestBuffersInRange(va, va + size);
    if (REXCVAR_GET(rr6_log_destroy) && (textures || buffers))
      RR6_INFO("XPhysicalFree 0x{:08X} ({} bytes): {} texture and {} buffer "
               "mirrors dropped",
               va, size, textures, buffers);
  }
  __imp__sub_82248508(ctx, base);
}
