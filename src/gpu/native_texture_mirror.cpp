/**
 * @file    gpu/native_texture_mirror.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/native_texture_mirror.h"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <xxhash.h>
#include <rex/ppc.h>

#include "core/logging.h"
#include "gpu/format.h"
#include "gpu/guest_buffers.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/texture_upload.h"
#include "gpu/perf_stats.h"

namespace rr6::gpu {

namespace {

namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

std::mutex g_mirror_mutex;
std::unordered_map<u32, std::unique_ptr<GuestTexture>> g_native_mirrors;

// Asset name per live mirror, normalized to a lowercased basename, so the
// engine can find the loaded instance of a texture it serves and rewrite it.
// The sequence number distinguishes allocations that reuse a VA.
struct NamedNativeTexture {
  std::string name;
  u64 seq = 0;
};
std::unordered_map<u32, NamedNativeTexture> g_native_names;
u64 g_native_name_seq = 0;

std::string NormalizeTextureName(std::string_view raw) {
  const size_t slash = raw.find_last_of("\\/");
  if (slash != std::string_view::npos)
    raw.remove_prefix(slash + 1);
  const size_t dot = raw.rfind('.');
  if (dot != std::string_view::npos)
    raw = raw.substr(0, dot);
  std::string out(raw);
  for (char &c : out)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Evicted (guest thread) but not yet freed. Held per slot until the post-fence
// drain so freeing the texture + rewriting its descriptor can't race an
// in-flight command list.
std::vector<std::unique_ptr<GuestTexture>> g_pending_native_destroy[kNumFrames];

// The name is diagnostic only and BD passes 0 for unnamed allocations, so an
// absent or unmapped name still logs something the line can be read by.
const char *TextureName(u32 name_va) {
  const char *name = rr6::mem::str(name_va);
  return *name ? name : "?";
}

// Derived once from the fetch constant and the format's block geometry.
struct MirrorLayout {
  xe::TextureFormat format = xe::TextureFormat::k_8_8_8_8;
  xe::Endian endian = xe::Endian::kNone;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 texels_per_edge = 1;
  u32 bytes_per_block = 1;
  u32 bpb_log2 = 0;
  u32 block_w = 0;
  u32 block_h = 0;
  // Tiled mip-0 footprint in blocks, padded to the 32-block macro tile.
  u32 aligned_w = 0;
  u32 aligned_h = 0;
  u32 staging_row_bytes = 0; // 256-aligned for the D3D12 copy rule
  u32 row_width_texels = 0;
  // A dimension <= 16 sits offset inside the level-0 tile, not at its origin.
  int pack_off_x = 0;
  int pack_off_y = 0;
};

u32 FetchWidth(const xe::xe_gpu_texture_fetch_t &f) {
  return (f.dimension == xe::DataDimension::k3D ? f.size_3d.width
                                                : f.size_2d.width) +
         1u;
}

u32 FetchHeight(const xe::xe_gpu_texture_fetch_t &f) {
  return (f.dimension == xe::DataDimension::k3D ? f.size_3d.height
                                                : f.size_2d.height) +
         1u;
}

u32 FetchDepth(const xe::xe_gpu_texture_fetch_t &f) {
  return f.dimension == xe::DataDimension::k3D ? f.size_3d.depth + 1u : 1u;
}

// D3DTexture_UnlockRect reads the base address at +20 and the mip address at
// +36, dwords 1 and 5 of this fetch constant.
constexpr u32 kTextureFetchOffset = offsetof(D3DTexture, Format);

bool ReadFetchWords(u32 guest_va, u32 fc[6]) {
  const auto *words =
      rr6::mem::try_at<const be_u32>(guest_va + kTextureFetchOffset);
  if (!words)
    return false;
  for (int i = 0; i < 6; ++i)
    fc[i] = u32(words[i]);
  return true;
}

bool ReadFetch(u32 guest_va, xe::xe_gpu_texture_fetch_t &fetch) {
  u32 fc[6];
  if (!ReadFetchWords(guest_va, fc))
    return false;
  std::memcpy(&fetch, fc, sizeof(fetch));
  return true;
}

MirrorLayout MakeLayout(const xe::xe_gpu_texture_fetch_t &fetch) {
  MirrorLayout L;
  L.format = fetch.format;
  L.endian = fetch.endianness;
  L.width = FetchWidth(fetch);
  L.height = FetchHeight(fetch);
  L.depth = FetchDepth(fetch);

  const rg::FormatInfo *info = rg::FormatInfo::Get(L.format);
  L.texels_per_edge = info->block_width;
  L.bytes_per_block = info->bytes_per_block();
  L.bpb_log2 = rex::log2_floor(L.bytes_per_block);
  L.block_w = rex::align(L.width, L.texels_per_edge) / L.texels_per_edge;
  L.block_h = rex::align(L.height, L.texels_per_edge) / L.texels_per_edge;
  L.aligned_w = rex::align(L.block_w, 32u);
  L.aligned_h = rex::align(L.block_h, 32u);
  L.staging_row_bytes = rex::align(L.block_w * L.bytes_per_block, 256u);
  L.row_width_texels =
      (L.staging_row_bytes / L.bytes_per_block) * L.texels_per_edge;

  if (fetch.packed_mips) {
    u32 px = 0, py = 0, pz = 0;
    tu::GetPackedMipOffset(L.width, L.height, 1u, L.format, 0u, px, py, pz);
    L.pack_off_x = static_cast<int>(px);
    L.pack_off_y = static_cast<int>(py);
  }
  return L;
}

size_t FaceStagingSize(const MirrorLayout &L) {
  return size_t(L.staging_row_bytes) * size_t(L.block_h);
}

// Gathers each untiled block from its tiled offset. 'out' is zero-filled by
// the caller, so an out-of-range gather simply leaves its block zero.
void UntileFace(const MirrorLayout &L, const u8 *face_src, u8 *out, int off_x,
                int off_y) {
  const u32 tiled_size = L.aligned_w * L.aligned_h * L.bytes_per_block;
  const int bpb = static_cast<int>(L.bytes_per_block);
  for (int by = 0; by < static_cast<int>(L.block_h); ++by) {
    for (int bx = 0; bx < static_cast<int>(L.block_w); ++bx) {
      const i32 so =
          tu::GetTiledOffset2D(off_x + bx, off_y + by, L.aligned_w, L.bpb_log2);
      if (so < 0 || u32(so) + L.bytes_per_block > tiled_size)
        continue;
      u8 *db =
          out + size_t(by) * L.staging_row_bytes + size_t(bx) * size_t(bpb);
      tc::CopySwapBlock(L.endian, db, face_src + so, size_t(bpb));
    }
  }
}

// rr6: linear (untiled) mip 0. Rows sit fetch.pitch * 32 texels apart.
void CopyLinearFace(const MirrorLayout &L,
                    const xe::xe_gpu_texture_fetch_t &fetch, const u8 *src,
                    u8 *out) {
  const u32 pitch_texels =
      fetch.pitch ? u32(fetch.pitch) << 5 : rex::align(L.width, 32u);
  const size_t src_row_bytes =
      size_t(pitch_texels / L.texels_per_edge) * L.bytes_per_block;
  const size_t row_bytes = size_t(L.block_w) * L.bytes_per_block;
  for (u32 by = 0; by < L.block_h; ++by) {
    tc::CopySwapBlock(L.endian, out + size_t(by) * L.staging_row_bytes,
                      src + size_t(by) * src_row_bytes, row_bytes);
  }
}

// The tiled footprint pads W/H to 32 blocks and depth to 4.
GuestTexture *BuildVolumeMirror(const MirrorLayout &L, const u8 *src,
                                u32 guest_va) {
  const size_t slice_size = FaceStagingSize(L);
  const size_t src_size = size_t(L.aligned_w) * size_t(L.aligned_h) *
                          size_t(rex::align(L.depth, 4u)) *
                          size_t(L.bytes_per_block);

  std::vector<u8> staging(slice_size * L.depth, 0u);
  for (int z = 0; z < static_cast<int>(L.depth); ++z) {
    for (int by = 0; by < static_cast<int>(L.block_h); ++by) {
      for (int bx = 0; bx < static_cast<int>(L.block_w); ++bx) {
        const i32 so = tu::GetTiledOffset3D(bx, by, z, L.aligned_w, L.aligned_h,
                                            L.bpb_log2);
        if (so < 0 || size_t(so) + L.bytes_per_block > src_size)
          continue;
        u8 *db = staging.data() + size_t(z) * slice_size +
                 size_t(by) * L.staging_row_bytes +
                 size_t(bx) * L.bytes_per_block;
        tc::CopySwapBlock(L.endian, db, src + size_t(so), L.bytes_per_block);
      }
    }
  }

  GuestTexture *tex = BuildBCMirrorTextureVolume(
      L.width, L.height, L.depth, u32(L.format), staging.data(), staging.size(),
      L.staging_row_bytes, L.row_width_texels);
  if (!tex) {
    RR6_WARN("native texture va=0x{:08X}: volume mirror build failed "
            "({}x{}x{} fmt=0x{:X})",
            guest_va, L.width, L.height, L.depth, u32(L.format));
    return nullptr;
  }

  // A tfetch2D shader on a 3D resource samples slice 0, and SetTexture routes
  // the volume into both index tables.
  GuestTexture *slice0 = BuildBCMirrorTexture(
      L.width, L.height, u32(L.format), staging.data(), slice_size,
      L.staging_row_bytes, L.row_width_texels);
  if (slice0) {
    tex->companion2D.reset(slice0);
  } else {
    RR6_WARN("native texture va=0x{:08X}: slice-0 2D companion build failed "
            "({}x{} fmt=0x{:X}), 3D-as-2D fetch will read null",
            guest_va, L.width, L.height, u32(L.format));
  }
  return tex;
}

// 6 faces packed consecutively from the base address, per-face stride being
// the exact tiled mip-0 footprint with no mip tail or padding between faces.
GuestTexture *BuildCubeMirror(const MirrorLayout &L, const u8 *src,
                              u32 guest_va) {
  const size_t face_stride =
      size_t(L.aligned_w) * size_t(L.aligned_h) * size_t(L.bytes_per_block);
  const size_t face_size = FaceStagingSize(L);

  std::vector<u8> face_bufs[6];
  const u8 *face_ptrs[6];
  for (int face = 0; face < 6; ++face) {
    face_bufs[face].assign(face_size, 0u);
    UntileFace(L, src + size_t(face) * face_stride, face_bufs[face].data(),
               L.pack_off_x, L.pack_off_y);
    face_ptrs[face] = face_bufs[face].data();
  }

  GuestTexture *tex = BuildBCMirrorTextureCube(
      L.width, L.height, u32(L.format), face_ptrs, face_size,
      L.staging_row_bytes, L.row_width_texels);
  if (!tex) {
    RR6_WARN("native texture va=0x{:08X}: cube mirror build failed "
            "({}x{} fmt=0x{:X})",
            guest_va, L.width, L.height, u32(L.format));
  }
  return tex;
}

// Levels 1..mip_max live at mip_address, with a packed tail for the <=16-texel
// levels. nullptr leaves the caller on the single-level path.
GuestTexture *BuildMippedMirror(const MirrorLayout &L,
                                const xe::xe_gpu_texture_fetch_t &fetch,
                                std::vector<u8> &base_staging,
                                const plume::RenderComponentMapping *mapping) {
  const auto *mip_src = rr6::mem::at<const u8>(fetch.mip_address << 12);
  const u32 mip_max_level = std::min<u32>(
      fetch.mip_max_level, rex::log2_floor(std::max(L.width, L.height)));
  if (mip_max_level < 1u || !mip_src)
    return nullptr;

  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      xe::DataDimension::k2DOrStacked, fetch.pitch, L.width, L.height, 1u,
      /*is_tiled=*/true, L.format,
      /*has_packed_levels=*/fetch.packed_mips != 0u, /*has_base=*/true,
      mip_max_level);

  // packed_level 0 means the base is in the tail too, so there is no chain.
  if (layout.packed_level == 0u)
    return nullptr;

  std::vector<std::vector<u8>> level_bufs(mip_max_level);
  std::vector<BCMipLevel> mip_levels;
  mip_levels.reserve(mip_max_level + 1u);
  mip_levels.push_back({base_staging.data(), base_staging.size(), L.width,
                        L.height, L.row_width_texels});

  for (u32 lvl = 1u; lvl <= mip_max_level; ++lvl) {
    const u32 lw = std::max(L.width >> lvl, 1u);
    const u32 lh = std::max(L.height >> lvl, 1u);
    const int lwb = int(rex::align(lw, L.texels_per_edge) / L.texels_per_edge);
    const int lhb = int(rex::align(lh, L.texels_per_edge) / L.texels_per_edge);
    const u32 dst_row = rex::align(u32(lwb) * L.bytes_per_block, 256u);
    const u32 dst_row_width_texels =
        (dst_row / L.bytes_per_block) * L.texels_per_edge;

    const bool packed =
        layout.packed_level != UINT32_MAX && lvl >= layout.packed_level;
    const u32 storage_level = packed ? layout.packed_level : lvl;
    const tu::TextureGuestLayout::Level &sl = layout.mips[storage_level];
    const u32 pitch_blocks = sl.row_pitch_bytes / L.bytes_per_block;
    const u8 *level_src = mip_src + layout.mip_offsets_bytes[storage_level];

    u32 px = 0, py = 0, pz = 0;
    if (packed) {
      tu::GetPackedMipOffset(L.width, L.height, 1u, L.format, lvl, px, py, pz);
    }

    std::vector<u8> &dst = level_bufs[lvl - 1u];
    dst.assign(size_t(dst_row) * size_t(lhb), 0u);
    for (int by = 0; by < lhb; ++by) {
      for (int bx = 0; bx < lwb; ++bx) {
        const i32 so = tu::GetTiledOffset2D(i32(px) + bx, i32(py) + by,
                                            pitch_blocks, L.bpb_log2);
        if (so < 0 || u32(so) + L.bytes_per_block > sl.level_data_extent_bytes)
          continue;
        u8 *db =
            dst.data() + size_t(by) * dst_row + size_t(bx) * L.bytes_per_block;
        tc::CopySwapBlock(L.endian, db, level_src + so, L.bytes_per_block);
      }
    }
    mip_levels.push_back(
        {dst.data(), dst.size(), lw, lh, dst_row_width_texels});
  }

  return BuildBCMirrorTexture2DMips(L.width, L.height, u32(L.format),
                                           mip_levels.data(),
                                           u32(mip_levels.size()), mapping);
}

// BD ships static reflection cubes (cube_*) as a 2D DXT atlas of six square
// faces left to right, but the water/glass fetch is a hardware cube (RefEnv,
// fetch slot 5), which would otherwise read the null placeholder. Slice the
// untiled mip-0 atlas into the TextureCube companion that slot publishes.
void AttachCubeAtlasCompanion(const MirrorLayout &L, GuestTexture *tex,
                              const std::vector<u8> &staging, u32 guest_va) {
  const u32 face_blocks_w =
      rex::align(L.height, L.texels_per_edge) / L.texels_per_edge;
  const u32 face_visible_row = face_blocks_w * L.bytes_per_block;
  const u32 face_row_bytes = rex::align(face_visible_row, 256u);
  const u32 face_size = face_row_bytes * L.block_h;
  const u32 face_row_width_texels =
      (face_row_bytes / L.bytes_per_block) * L.texels_per_edge;

  std::vector<u8> face_bufs[6];
  const u8 *face_ptrs[6];
  for (u32 f = 0; f < 6u; ++f) {
    face_bufs[f].assign(face_size, 0u);
    for (u32 by = 0; by < L.block_h; ++by) {
      const u8 *sb = staging.data() + size_t(by) * L.staging_row_bytes +
                     size_t(f) * face_visible_row;
      std::memcpy(face_bufs[f].data() + size_t(by) * face_row_bytes, sb,
                  face_visible_row);
    }
    face_ptrs[f] = face_bufs[f].data();
  }

  GuestTexture *cube = BuildBCMirrorTextureCube(
      L.height, L.height, u32(L.format), face_ptrs, face_size, face_row_bytes,
      face_row_width_texels);
  if (cube) {
    tex->companionCube.reset(cube);
  } else {
    RR6_WARN("native texture va=0x{:08X}: cube_* atlas companion build failed "
            "({}x{} fmt=0x{:X}), water/glass cube fetch reads null",
            guest_va, L.height, L.height, u32(L.format));
  }
}

GuestTexture *Build2DMirror(const MirrorLayout &L, const u8 *src,
                            const xe::xe_gpu_texture_fetch_t &fetch,
                            u32 guest_va, u32 name_va, bool dxt) {
  // The fetch constant's swizzle picks each output component (3 bits per
  // output: 0-3 a source component, 4 zero, 5 one), the way Xenia applies it.
  // An A8R8G8B8 texture fetched as k_8_8_8_8 carries the swizzle that restores
  // its channel order, and Xenos replicates a single-channel format into every
  // component before swizzling (k_8, DXT5A), and a two-channel one (DXN) as RGGG, where
  // host BC5 would return 0 and 1 for blue and alpha.
  plume::RenderComponentMapping mapping{};
  const plume::RenderComponentMapping *mapping_ptr = nullptr;
  {
    const bool single = L.format == xe::TextureFormat::k_8 ||
                        L.format == xe::TextureFormat::k_DXT5A;
    const bool dual = L.format == xe::TextureFormat::k_DXN;
    auto pick = [&](u32 shift) {
      const u32 sel = (u32(fetch.swizzle) >> shift) & 0x7u;
      switch (sel) {
      case 0:
        return plume::RenderSwizzle::R;
      case 1:
        return single ? plume::RenderSwizzle::R : plume::RenderSwizzle::G;
      case 2:
        return single ? plume::RenderSwizzle::R
                      : (dual ? plume::RenderSwizzle::G : plume::RenderSwizzle::B);
      case 3:
        return single ? plume::RenderSwizzle::R
                      : (dual ? plume::RenderSwizzle::G : plume::RenderSwizzle::A);
      case 4:
        return plume::RenderSwizzle::ZERO;
      default:
        return plume::RenderSwizzle::ONE;
      }
    };
    mapping = plume::RenderComponentMapping(pick(0), pick(3), pick(6), pick(9));
    mapping_ptr = &mapping;
  }
  std::vector<u8> staging(FaceStagingSize(L), 0u);
  GuestTexture *tex = nullptr;
  if (fetch.tiled) {
    UntileFace(L, src, staging.data(), L.pack_off_x, L.pack_off_y);
    tex = BuildMippedMirror(L, fetch, staging, mapping_ptr);
  } else {
    // Linear textures carry a single level here.
    CopyLinearFace(L, fetch, src, staging.data());
  }
  if (!tex) {
    tex = BuildBCMirrorTexture(L.width, L.height, u32(L.format),
                                      staging.data(), staging.size(),
                                      L.staging_row_bytes, L.row_width_texels,
                                      mapping_ptr);
  }
  if (!tex) {
    RR6_WARN("native texture va=0x{:08X}: 2D mirror build failed "
            "({}x{} fmt=0x{:X})",
            guest_va, L.width, L.height, u32(L.format));
    return nullptr;
  }

  const bool is_cube_atlas =
      std::strncmp(rr6::mem::str(name_va), "cube_", 5) == 0 && dxt &&
      L.height > 0u && L.width == L.height * 6u;
  if (is_cube_atlas) {
    AttachCubeAtlasCompanion(L, tex, staging, guest_va);
  }
  return tex;
}

} // namespace

GuestTexture *GetOrCreateNativeMirror(u32 guest_va, u32 name_va) {
  if (!guest_va)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_mirror_mutex);

  auto it = g_native_mirrors.find(guest_va);
  if (it != g_native_mirrors.end()) {
    // From the bdAllocRenderBuffer hook a hit means the engine re-malloc'd a
    // freed texture struct at this VA: the previous occupant was released via
    // the preload table path (hcgTextureListRelease) that we do not hook,
    // leaving this mirror stale. From NativeTextureReplace it is the rewritten
    // payload wanting a fresh mirror. Either way, evict and rebuild.
    g_pending_native_destroy[Video::RetireSlot("native mirror")].push_back(
        std::move(it->second));
    g_native_mirrors.erase(it);
  }

  xe::xe_gpu_texture_fetch_t fetch;
  if (!ReadFetch(guest_va, fetch)) {
    RR6_WARN("native texture: guest VA 0x{:08X} does not translate", guest_va);
    return nullptr;
  }

  const bool dxt = (fetch.format == xe::TextureFormat::k_DXT1 ||
                    fetch.format == xe::TextureFormat::k_DXT2_3 ||
                    fetch.format == xe::TextureFormat::k_DXT4_5 ||
                    fetch.format == xe::TextureFormat::k_DXN ||
                    fetch.format == xe::TextureFormat::k_DXT5A);
  // Uncompressed k_8_8_8_8 (color ramps / UI atlases BD stores raw, e.g. the
  // cloud sky LUT dg03_cbr01). 2D only: cube/volume assume DXT blocks.
  const bool rgba8 = (fetch.format == xe::TextureFormat::k_8_8_8_8 ||
                      fetch.format == xe::TextureFormat::k_8) &&
                     fetch.dimension <= xe::DataDimension::k2DOrStacked;
  const bool supported_dim = fetch.dimension != xe::DataDimension::k1D;
  // DXT ships endian kNone/k8in16, and k_8_8_8_8 ships k8in32 (full word swap).
  const bool endian_ok =
      fetch.endianness <= (dxt ? xe::Endian::k8in16 : xe::Endian::k8in32);

  // Non-tiled is only taken for 2D (rr6's CopyLinearFace): the volume and
  // cube deswizzlers handle tiled data alone.
  const bool linear_ok =
      fetch.tiled || fetch.dimension == xe::DataDimension::k2DOrStacked;
  if (fetch.type != xe::FetchConstantType::kTexture || !supported_dim ||
      !(dxt || rgba8) || !endian_ok || !linear_ok) {
    RR6_WARN("native texture rejected name='{}' va=0x{:08X} fetch1=0x{:08X} type={} dim={} "
            "fmt=0x{:X} endian={} tiled={} {}x{}x{}",
            TextureName(name_va), guest_va, reinterpret_cast<const u32 *>(&fetch)[1], u32(fetch.type),
            u32(fetch.dimension), u32(fetch.format), u32(fetch.endianness),
            u32(fetch.tiled), FetchWidth(fetch), FetchHeight(fetch),
            FetchDepth(fetch));
    return nullptr;
  }

  // The XPhysicalAllocEx VA the engine wrote through, so the heap-aware
  // accessor matches how the data was written.
  const auto *src = rr6::mem::at<const u8>(fetch.base_address << 12);
  if (!src) {
    RR6_WARN("native texture va=0x{:08X}: base address 0x{:08X} does not "
            "translate",
            guest_va, fetch.base_address << 12);
    return nullptr;
  }

  const MirrorLayout layout = MakeLayout(fetch);
  perf::Scope timer(perf::Counter::TextureMirror);
  GuestTexture *tex = nullptr;
  switch (fetch.dimension) {
  case xe::DataDimension::k3D:
    tex = BuildVolumeMirror(layout, src, guest_va);
    break;
  case xe::DataDimension::kCube:
    tex = BuildCubeMirror(layout, src, guest_va);
    break;
  default:
    tex = Build2DMirror(layout, src, fetch, guest_va, name_va, dxt);
    break;
  }
  if (!tex)
    return nullptr;

  auto stored = std::unique_ptr<GuestTexture>(tex);
  GuestTexture *raw = stored.get();
  g_native_mirrors.emplace(guest_va, std::move(stored));
  // A rebuild through NativeTextureReplace passes no name and keeps the entry
  // the allocation registered, sequence included.
  if (name_va) {
    const char *name = rr6::mem::str(name_va);
    if (*name)
      g_native_names[guest_va] = {NormalizeTextureName(name),
                                  ++g_native_name_seq};
  }
  return raw;
}

TextureContent TextureContent::Scan(u32 guest_va) {
  const TextureContent whole;
  xe::xe_gpu_texture_fetch_t fetch;
  if (!ReadFetch(guest_va, fetch) ||
      fetch.type != xe::FetchConstantType::kTexture ||
      fetch.dimension != xe::DataDimension::k2DOrStacked ||
      !rg::FormatInfo::Get(fetch.format))
    return whole;

  const MirrorLayout L = MakeLayout(fetch);
  if (!L.width || !L.height || !L.bytes_per_block || !L.texels_per_edge)
    return whole;

  const u32 base = fetch.base_address << 12;
  const u32 blocks =
      fetch.tiled ? L.aligned_w * L.aligned_h : L.block_w * L.block_h;
  const auto *src = rr6::mem::try_at<const u8>(base);
  // Both ends, since try_at validates an address and not a range.
  if (!src || !rr6::mem::try_at<const u8>(base + blocks * L.bytes_per_block - 1))
    return whole;

  const auto block_at = [&](u32 bx, u32 by) -> const u8 * {
    if (!fetch.tiled)
      return src + (size_t(by) * L.block_w + bx) * L.bytes_per_block;
    const i32 offset =
        tu::GetTiledOffset2D(i32(bx), i32(by), L.aligned_w, L.bpb_log2);
    return offset < 0 ? nullptr : src + size_t(offset);
  };

  const u8 *field = block_at(0, 0);
  if (!field)
    return whole;

  u32 min_x = L.block_w;
  u32 min_y = L.block_h;
  u32 max_x = 0;
  u32 max_y = 0;
  for (u32 by = 0; by < L.block_h; ++by) {
    for (u32 bx = 0; bx < L.block_w; ++bx) {
      const u8 *block = block_at(bx, by);
      if (!block || std::memcmp(block, field, L.bytes_per_block) == 0)
        continue;
      min_x = std::min(min_x, bx);
      max_x = std::max(max_x, bx);
      min_y = std::min(min_y, by);
      max_y = std::max(max_y, by);
    }
  }
  if (min_x > max_x || min_y > max_y)
    return whole;

  TextureContent out;
  out.u0 = float(min_x * L.texels_per_edge) / float(L.width);
  out.v0 = float(min_y * L.texels_per_edge) / float(L.height);
  out.u1 =
      std::min(1.0f, float((max_x + 1) * L.texels_per_edge) / float(L.width));
  out.v1 =
      std::min(1.0f, float((max_y + 1) * L.texels_per_edge) / float(L.height));
  return out;
}

GuestTexture *ResolveGuestTexture(u32 guest_va) {
  if (!guest_va)
    return nullptr;

  if (auto *host = HostResourceHeap::FromGuest<GuestTexture>(guest_va)) {
    return host;
  }

  // Pure lookup, since mirrors are built eagerly at alloc time. Building on
  // demand would mis-decode arbitrary bytes as a fetch constant and overread
  // small allocations.
  {
    std::lock_guard<std::mutex> lock(g_mirror_mutex);
    auto it = g_native_mirrors.find(guest_va);
    if (it != g_native_mirrors.end())
      return it->second.get();
  }
  return nullptr;
}

void EvictNativeTexture(u32 guest_va) {
  if (!guest_va)
    return;
  std::lock_guard<std::mutex> lock(g_mirror_mutex);
  auto it = g_native_mirrors.find(guest_va);
  if (it == g_native_mirrors.end())
    return;
  // Defer teardown to the post-fence drain, and remove from the registry now so
  // a same-VA re-alloc builds a fresh mirror.
  g_pending_native_destroy[Video::RetireSlot("native mirror")].push_back(
      std::move(it->second));
  g_native_mirrors.erase(it);
  g_native_names.erase(guest_va);
}

std::vector<NativeTextureRef> NativeTexturesByName(std::string_view name) {
  const std::string wanted = NormalizeTextureName(name);
  std::vector<NativeTextureRef> out;
  std::lock_guard<std::mutex> lock(g_mirror_mutex);
  for (const auto &[va, entry] : g_native_names) {
    if (entry.name == wanted)
      out.push_back({va, entry.seq});
  }
  return out;
}

bool NativeTextureReplace(u32 guest_va, const u8 *blob, size_t size) {
  if (!blob || size < 2048u)
    return false;
  const auto *d3dtex = rr6::mem::at<const D3DTexture>(guest_va);
  if (!d3dtex)
    return false;

  u32 fc[6];
  for (int i = 0; i < 6; ++i)
    fc[i] = u32(d3dtex->Format.dword[i]);
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fc, sizeof(fetch));

  u32 blobFc[6];
  for (int i = 0; i < 6; ++i) {
    const u8 *w = blob + 4u + 28u + u32(i) * 4u;
    blobFc[i] = (u32(w[0]) << 24) | (u32(w[1]) << 16) | (u32(w[2]) << 8) |
                u32(w[3]);
  }
  xe::xe_gpu_texture_fetch_t blobFetch;
  std::memcpy(&blobFetch, blobFc, sizeof(blobFetch));

  // Same geometry or nothing: the physical allocation was sized for the live
  // texture, and the mirror build below trusts the live fetch constant.
  if (blobFetch.format != fetch.format || blobFc[2] != fc[2] ||
      blobFetch.pitch != fetch.pitch || !fetch.tiled ||
      fetch.dimension != xe::DataDimension::k2DOrStacked || fetch.mip_address) {
    RR6_WARN("native texture replace refused va=0x{:08X}: geometry mismatch",
            guest_va);
    return false;
  }

  const MirrorLayout L = MakeLayout(fetch);
  const size_t payload =
      size_t(L.aligned_w) * size_t(L.aligned_h) * size_t(L.bytes_per_block);
  const u32 blobPayload = (u32(blob[0]) << 24) | (u32(blob[1]) << 16) |
                          (u32(blob[2]) << 8) | u32(blob[3]);
  if (blobPayload != payload || size < 2048u + payload) {
    RR6_WARN("native texture replace refused va=0x{:08X}: payload {} vs {}",
            guest_va, blobPayload, payload);
    return false;
  }

  auto *dst = rr6::mem::at<u8>(fetch.base_address << 12);
  if (!dst)
    return false;
  std::memcpy(dst, blob + 2048, payload);
  return GetOrCreateNativeMirror(guest_va) != nullptr;
}

void DrainEvictedNativeTextures(u32 slot) {
  std::vector<std::unique_ptr<GuestTexture>> dead;
  {
    std::lock_guard<std::mutex> lock(g_mirror_mutex);
    dead.swap(g_pending_native_destroy[slot]);
  }
  // Lock released before NotifyTextureDestroyed, which takes Video's mutex.
  for (auto &tex : dead) {
    Video::NotifyTextureDestroyed(tex.get());
    // Only fence(slot) was awaited, and the other in-flight slot may still
    // reference the image/view, so park them instead of freeing here.
    ParkTextureGPUObjects(tex.get());
  }
}

// Per-VA state of the unhooked mirrors (ResolveOrMirrorGuestTexture): the signature each mirror was
// built from, the VAs whose format was rejected, and the per-frame texel hash
// of linear textures.
std::mutex s_unhooked_mutex;
std::unordered_map<u32, u64> s_signatures;
std::unordered_set<u32> s_rejected;
std::unordered_map<u32, std::pair<u64, u64>> s_texel_hashes;
// Base and mip addresses per mirrored VA, unhooked mirrors and resolve targets
// alike, so freeing their memory can find them. RR6's fetch constants carry the
// data's virtual address (0xE.../0xF... physical heap views), not a physical one.
std::unordered_map<u32, std::pair<u32, u32>> s_texel_bases;

// Resolve destinations: guest textures the GPU writes through D3DDevice_Resolve.
// They get a render-target-capable host texture sized from their fetch
// constant, and are never rebuilt from guest memory (which the host GPU never
// writes back).
std::mutex g_resolve_mutex;
std::unordered_map<u32, std::unique_ptr<GuestTexture>> g_resolve_targets;

GuestTexture *FindResolveTarget(u32 guest_va) {
  std::lock_guard lock(g_resolve_mutex);
  auto it = g_resolve_targets.find(guest_va);
  return it != g_resolve_targets.end() ? it->second.get() : nullptr;
}

void ForEachResolveTarget(
    const std::function<void(u32 guest_va, GuestTexture *tex)> &fn) {
  std::lock_guard lock(g_resolve_mutex);
  for (auto &[va, tex] : g_resolve_targets)
    fn(va, tex.get());
}

GuestTexture *GetOrCreateResolveTarget(u32 guest_va,
                                       plume::RenderFormat format) {
  if (!guest_va || format == plume::RenderFormat::UNKNOWN)
    return nullptr;
  xe::xe_gpu_texture_fetch_t fetch;
  if (!ReadFetch(guest_va, fetch) ||
      fetch.type != xe::FetchConstantType::kTexture)
    return nullptr;
  // Resolves copy host surfaces, which are the guest size times their scale,
  // so the destination is scaled the same way. Shaders sample it by
  // normalized coordinates and never see the difference.
  const u32 scale = Output::SurfaceScale(FetchWidth(fetch), FetchHeight(fetch));
  const u32 width = FetchWidth(fetch) * scale;
  const u32 height = FetchHeight(fetch) * scale;
  if (!width || !height)
    return nullptr;

  // A cube destination takes its six faces from six resolves (RR6's dynamic
  // environment map), each landing in the face D3DDevice_Resolve names.
  const bool cube = fetch.dimension == xe::DataDimension::kCube;
  const auto view_dim = cube ? plume::RenderTextureViewDimension::TEXTURE_CUBE
                             : plume::RenderTextureViewDimension::TEXTURE_2D;

  std::lock_guard lock(g_resolve_mutex);
  auto &slot = g_resolve_targets[guest_va];
  if (slot && slot->width == width && slot->height == height &&
      slot->format == format && slot->viewDimension == view_dim)
    return slot.get();
  if (slot) {
    Video::NotifyTextureDestroyed(slot.get());
    g_pending_native_destroy[Video::RetireSlot("resolve target")].push_back(
        std::move(slot));
  }

  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;
  auto tex = std::make_unique<GuestTexture>(ResourceType::Texture);
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = cube ? 6 : 1;
  desc.format = format;
  desc.flags = IsRenderTargetCapable(format)
                   ? plume::RenderTextureFlag::RENDER_TARGET
                   : plume::RenderTextureFlag::NONE;
  if (cube)
    desc.flags |= plume::RenderTextureFlag::CUBE;
  desc.committed = false;
  tex->viewDimension = view_dim;
  tex->textureHolder = CreateHostTexture(device, desc, "resolve-target");
  tex->texture = tex->textureHolder.get();
  if (!tex->texture)
    return nullptr;
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = format;
  view_desc.dimension = view_dim;
  view_desc.mipLevels = 1;
  tex->textureView = tex->texture->createTextureView(view_desc);
  tex->width = width;
  tex->height = height;
  tex->depth = 1;
  tex->mipLevels = 1;
  tex->format = format;
  tex->guestFormat = u32(fetch.format);
  tex->hostScale = scale;
  Video::BindTextureSRV(tex.get());
  slot = std::move(tex);
  {
    std::lock_guard base_lock(s_unhooked_mutex);
    s_texel_bases[guest_va] = {u32(fetch.base_address) << 12,
                               u32(fetch.mip_address) << 12};
  }
  return slot.get();
}


bool EvictGuestTexture(u32 guest_va) {
  if (!guest_va)
    return false;
  bool found = false;
  {
    std::lock_guard lock(s_unhooked_mutex);
    found |= s_signatures.erase(guest_va) != 0;
    s_rejected.erase(guest_va);
    s_texel_hashes.erase(guest_va);
    s_texel_bases.erase(guest_va);
  }
  {
    std::lock_guard lock(g_resolve_mutex);
    auto it = g_resolve_targets.find(guest_va);
    if (it != g_resolve_targets.end()) {
      found = true;
      Video::NotifyTextureDestroyed(it->second.get());
      std::lock_guard mirror_lock(g_mirror_mutex);
      g_pending_native_destroy[Video::RetireSlot("evicted resolve target")]
          .push_back(std::move(it->second));
      g_resolve_targets.erase(it);
    }
  }
  EvictNativeTexture(guest_va);
  return found;
}

u32 EvictGuestTexturesInRange(u32 begin, u32 end) {
  const auto in = [&](u32 a) { return a >= begin && a < end; };
  std::vector<u32> dead;
  {
    std::lock_guard lock(s_unhooked_mutex);
    for (const auto &[va, bases] : s_texel_bases) {
      if (in(bases.first) || (bases.second && in(bases.second)))
        dead.push_back(va);
    }
  }
  for (u32 va : dead)
    EvictGuestTexture(va);
  return u32(dead.size());
}

// RR6's D3D creates its textures unhooked, so they are mirrored the first
// time a draw binds them, and rebuilt when the texture's fetch constant (base
// address, format, size) changes under the same struct, i.e. the struct was
// freed and reused. Only the texel-identifying words are compared: the sampler
// fields live in the device copy, not here.
GuestTexture *ResolveOrMirrorGuestTexture(u32 guest_va) {
  if (!guest_va)
    return nullptr;
  if (GuestTexture *resolved = FindResolveTarget(guest_va))
    return resolved;
  u32 fc[6];
  if (!ReadFetchWords(guest_va, fc))
    return nullptr;
  // dword1: format/endian/tiled/base, dword2: size, dword5: mip address.
  u64 signature =
      (u64(fc[1]) << 32 | fc[2]) ^ (u64(fc[5]) * 0x9E3779B97F4A7C15ull);
  // Linear textures are the ones the CPU rewrites (RR6's boot Pac-Man screen
  // is a 224x288 k_8_8_8_8 redrawn every frame), so their texels join the
  // signature, rehashed at most once per frame.
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fc, sizeof(fetch));
  if (!fetch.tiled && fetch.dimension == xe::DataDimension::k2DOrStacked) {
    const u64 frame = GuestMirrorFrame();
    std::lock_guard hash_lock(s_unhooked_mutex);
    auto &[hashed_frame, hash] = s_texel_hashes[guest_va];
    if (hashed_frame != frame || !hash) {
      const MirrorLayout L = MakeLayout(fetch);
      const u32 pitch_texels =
          fetch.pitch ? u32(fetch.pitch) << 5 : rex::align(L.width, 32u);
      const size_t bytes = size_t(pitch_texels / std::max(1u, L.texels_per_edge)) *
                           L.bytes_per_block * L.block_h;
      const u32 base = fetch.base_address << 12;
      const auto *src = rr6::mem::try_at<const u8>(base);
      if (src && bytes && rr6::mem::try_at<const u8>(base + u32(bytes) - 1)) {
        perf::Scope timer(perf::Counter::TexelHash, bytes);
        hash = XXH3_64bits(src, bytes);
      }
      hashed_frame = frame;
    }
    signature ^= hash;
  }
  {
    std::lock_guard lock(s_unhooked_mutex);
    auto sig = s_signatures.find(guest_va);
    if (sig != s_signatures.end() && sig->second == signature) {
      if (s_rejected.count(guest_va))
        return nullptr;
      // Physical frees evict on whichever guest thread frees.
      std::lock_guard mirror_lock(g_mirror_mutex);
      auto it = g_native_mirrors.find(guest_va);
      if (it != g_native_mirrors.end())
        return it->second.get();
    }
    s_signatures[guest_va] = signature;
    s_rejected.erase(guest_va);
    s_texel_bases[guest_va] = {u32(fetch.base_address) << 12,
                               u32(fetch.mip_address) << 12};
  }
  GuestTexture *tex = GetOrCreateNativeMirror(guest_va);
  if (!tex) {
    std::lock_guard lock(s_unhooked_mutex);
    s_rejected.insert(guest_va);
  }
  return tex;
}

} // namespace rr6::gpu

// Midasm hook at global scope, externed by the generated recompiler dispatch,
// so name and signature must match the hook config. Fires in
// hcgTextureListRelease's refcount-zero block when the name-refcounted
// preload table releases a native texture's last reference. hcgTextureCacheAdd
// registered flt_82DBB148[k] with bdAllocRenderBuffer's return (== the
// g_native_mirrors key) and unk_82DBB170[k] with the XPhysical allocation. That
// free path calls XPhysicalFree directly, bypassing LoadTexture__vf03, so the
// host mirror must be evicted here.
//   r25 = flt_82DBB148[k] = mirror key   r26 = unk_82DBB170[k] = XPhysical
//   marker
void bdPreloadTableFreeTextureHook(PPCRegister &r25, PPCRegister &r26) {
  if (r26.u32)
    rr6::gpu::EvictNativeTexture(r25.u32);
}
