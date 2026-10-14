/**
 * @file    gpu/resolve.cpp
 * @brief   EDRAM resolve emulation: surface-to-texture copy, the lazy resolve
 *          alias, and the fullscreen composite chain seeding.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "core/profiling.h"

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/format.h"
#include "gpu/output.h"

namespace rr6::gpu {

namespace {

inline const plume::RenderTexture *const kDepthFbKey =
    reinterpret_cast<const plume::RenderTexture *>(uintptr_t{1});

GuestTexture *ResolveSourceForFlagsLocked(VideoState &s, u32 flags) {
  if (flags & kResolveDepthStencil)
    return s.depth_stencil;
  if (s.render_target)
    return s.render_target;
  return s.back_buffer_surface;
}

bool CanAliasResolveLocked(const GuestTexture *src, const GuestTexture *dst) {
  return src && dst && src != dst && src->texture && dst->texture &&
         src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
         src->width == dst->width && src->height == dst->height &&
         src->format == dst->format && dst->resolveScale == 1.0f &&
         dst->viewDimension !=
             plume::RenderTextureViewDimension::TEXTURE_CUBE &&
         dst->mipLevels <= 1 && src->descriptorIndex != kInvalidDescriptorIndex;
}

} // namespace

void DetachSourceSurfaceLocked(VideoState &s, GuestTexture *texture) {
  if (!texture || !texture->sourceSurface)
    return;
  GuestTexture *source = texture->sourceSurface;
  source->destinationTextures.erase(texture);
  texture->sourceSurface = nullptr;
}

bool CopySurfaceToTextureLocked(VideoState &s, GuestTexture *src,
                                GuestTexture *dst, const char *reason) {
  if (!dst || !dst->texture || !src || !src->texture || src == dst) {
    return false;
  }
  if (!s.ready)
    return false;
  if (dst->sampleCount != plume::RenderSampleCount::COUNT_1) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      RR6_WARN("{}: MSAA destination {}x{} rejected (resolve targets must be "
              "single-sample)",
              reason, dst->width, dst->height);
    }
    return false;
  }

  BeginCommandList(s);
  if (!s.command_list_open)
    return false;

  s.draw_framebuffer_bound = false;

  const bool dims_match =
      src->width == dst->width && src->height == dst->height;
  const bool format_match = src->format == dst->format;
  const float resolve_scale = dst->resolveScale;
  const bool needs_scale = (resolve_scale != 1.0f);
  const bool dst_is_cube =
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;

  if (dims_match && format_match && !needs_scale &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1) {
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(dst->texture,
                                    plume::RenderTextureLayout::COPY_DEST),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, pre,
                             std::size(pre));
    src->layout = plume::RenderTextureLayout::COPY_SOURCE;
    dst->layout = plume::RenderTextureLayout::COPY_DEST;
    if (dst_is_cube) {
      s.command_list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(
              dst->texture, dst->resolveLevel, dst->resolveFace),
          plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0));
    } else {
      s.command_list->copyTexture(dst->texture, src->texture);
    }
  } else if (dst_is_cube) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      RR6_WARN("{}: cube resolve dst needs convert/scale (src={}x{} fmt={} "
              "dst fmt={} scale={}), no per-face RTV, skipping",
              reason, src->width, src->height, static_cast<int>(src->format),
              static_cast<int>(dst->format), resolve_scale);
    }
    return false;
  } else {
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      BindTextureSRVLocked(s, src);
    }
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        RR6_ERROR("{}: source RT 0x{:x} not in bindless heap, can't "
                 "shader-resolve",
                 reason, reinterpret_cast<uintptr_t>(src));
      }
      return false;
    }

    const bool depth_dst = IsDepthFormat(dst->format);
    if (!depth_dst && !IsRenderTargetCapable(dst->format)) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        RR6_ERROR("{}: dst format {} is not RT-capable, skipping shader "
                 "resolve (src={}x{} fmt={} dst={}x{} fmt={})",
                 reason, static_cast<int>(dst->format), src->width, src->height,
                 static_cast<int>(src->format), dst->width, dst->height,
                 static_cast<int>(dst->format));
      }
      return false;
    }

    const plume::RenderTexture *fb_key = depth_dst ? kDepthFbKey : nullptr;

    auto it = dst->framebuffers.find(fb_key);
    plume::RenderFramebuffer *dst_fb = nullptr;
    if (it != dst->framebuffers.end()) {
      dst_fb = it->second.get();
    } else {
      std::unique_ptr<plume::RenderFramebuffer> fb;
      if (depth_dst) {
        plume::RenderFramebufferDesc desc;
        desc.depthAttachment = dst->texture;
        desc.colorAttachmentsCount = 0;
        fb = s.device->createFramebuffer(desc);
      } else {
        const plume::RenderTexture *attachments[1] = {dst->texture};
        plume::RenderFramebufferDesc desc(attachments, 1);
        fb = s.device->createFramebuffer(desc);
      }
      if (!fb) {
        static std::atomic<u32> s_warn{0};
        if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
          RR6_ERROR("{}: createFramebuffer failed (depth_dst={}), skipping "
                   "shader resolve (src={}x{} dst={}x{})",
                   reason, depth_dst ? 1 : 0, src->width, src->height,
                   dst->width, dst->height);
        }
        return false;
      }
      dst_fb = fb.get();
      dst->framebufferAttached = true;
      dst->framebuffers.emplace(fb_key, std::move(fb));
      s.framebuffer_owners.insert(dst);
    }

    const plume::RenderTextureLayout dst_write_layout =
        depth_dst ? plume::RenderTextureLayout::DEPTH_WRITE
                  : plume::RenderTextureLayout::COLOR_WRITE;
    const bool discard_dst =
        (dst->layout == plume::RenderTextureLayout::UNKNOWN);
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(dst->texture, dst_write_layout),
    };
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, pre,
                             std::size(pre));
    src->layout = plume::RenderTextureLayout::SHADER_READ;
    dst->layout = dst_write_layout;
    if (discard_dst)
      s.command_list->discardTexture(dst->texture);

    s.command_list->setFramebuffer(dst_fb);
    s.command_list->setViewports(
        plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                              static_cast<float>(dst->height)));
    s.command_list->setScissors(plume::RenderRect(
        0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
    plume::RenderPipeline *pipeline = nullptr;
    if (src->sampleCount != plume::RenderSampleCount::COUNT_1) {
      pipeline = GetOrCreateResolveMSAAPipeline(s, dst->format,
                                                src->sampleCount, depth_dst);
    } else if (depth_dst) {
      pipeline = GetOrCreateCopyDepthPipeline(s, dst->format);
    } else {
      pipeline = GetOrCreateResolvePipeline(s, dst->format);
    }
    if (!pipeline) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        RR6_ERROR("{}: createGraphicsPipeline failed for dst format {}", reason,
                 static_cast<int>(dst->format));
      }
      return false;
    }
    s.command_list->setPipeline(pipeline);
    const u32 descriptor_index = src->descriptorIndex;

    u32 box_ratio = 0u;
    if (!depth_dst && src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->width != 0u && dst->height != 0u && src->width > dst->width &&
        src->height > dst->height && (src->width % dst->width) == 0u &&
        (src->height % dst->height) == 0u &&
        (src->width / dst->width) == (src->height / dst->height)) {
      box_ratio = src->width / dst->width;
    }

    struct CopyPushConstants {
      u32 resourceDescriptorIndex;
      u32 resourceDescriptorIndex2;
      float param0;
      float param1;
    } push{descriptor_index, 0u, depth_dst ? 1.0f : resolve_scale,
           static_cast<float>(box_ratio)};
    s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                             kCopyPushConstantByteOffset,
                                             sizeof(push));
    s.command_list->drawInstanced(3, 1, 0, 0);

    s.command_list->setFramebuffer(nullptr);
    s.current_pso = nullptr;
    s.dirtyStates.pipelineState = true;
    s.draw_framebuffer_bound = false;
  }

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

namespace {

bool ClearColorTargetLocked(VideoState &s, GuestTexture *dst,
                            plume::RenderColor color) {
  if (!dst || !dst->texture || !s.ready)
    return false;
  if (IsDepthFormat(dst->format) ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;

  plume::RenderFramebuffer *dst_fb = GetFramebuffer(s, dst, nullptr);
  if (!dst_fb)
    return false;

  plume::RenderTextureBarrier pre(dst->texture,
                                  plume::RenderTextureLayout::COLOR_WRITE);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &pre, 1);
  dst->layout = plume::RenderTextureLayout::COLOR_WRITE;
  s.command_list->setFramebuffer(dst_fb);
  s.command_list->setViewports(
      plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                            static_cast<float>(dst->height)));
  s.command_list->setScissors(plume::RenderRect(
      0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
  s.command_list->clearColor(0, color);
  s.command_list->setFramebuffer(nullptr);
  s.draw_framebuffer_bound = false;

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

} // namespace

void Video::ClearTexture(GuestTexture *texture, u32 color_argb) {
  if (!texture || !texture->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ClearColorTargetLocked(s, texture, ArgbToRenderColor(color_argb));
}

bool MaterializeOutboundLocked(VideoState &s, GuestTexture *source,
                               bool aliasable_only) {
  if (!source || source->destinationTextures.empty())
    return false;
  bool recorded = false;
  std::vector<GuestTexture *> destinations(source->destinationTextures.begin(),
                                           source->destinationTextures.end());
  for (GuestTexture *dst : destinations) {
    if (!dst || dst == source || dst->sourceSurface != source)
      continue;
    if (dst->pendingDestroy) {
      DetachSourceSurfaceLocked(s, dst);
      continue;
    }
    const bool one_to_one =
        source->width == dst->width && source->height == dst->height &&
        source->format == dst->format &&
        source->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->resolveScale == 1.0f &&
        dst->viewDimension != plume::RenderTextureViewDimension::TEXTURE_CUBE;
    if (aliasable_only && !one_to_one)
      continue;
    if (CopySurfaceToTextureLocked(s, source, dst, "Materialize")) {
      recorded = true;
      DetachSourceSurfaceLocked(s, dst);
    }
  }
  return recorded;
}

void MaterializeInboundLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->sourceSurface || dst->sourceSurface == dst)
    return;
  if (CopySurfaceToTextureLocked(s, dst->sourceSurface, dst, "Materialize")) {
    DetachSourceSurfaceLocked(s, dst);
  }
}

bool FullscreenChainClassLocked(const VideoState &s, const GuestTexture *t) {
  const GuestTexture *bb = s.back_buffer_surface;
  if (!t || !bb || t == bb || IsDepthFormat(t->format) || t->reflection)
    return false;
  constexpr u32 kDesignWidth = 1280u;
  const u32 min_full_width =
      bb->width > kDesignWidth ? kDesignWidth : bb->width;
  if (t->width < min_full_width)
    return false;
  const u64 a = u64(t->width) * bb->height;
  const u64 b = u64(t->height) * bb->width;
  const u64 diff = a > b ? a - b : b - a;
  return diff * 100ull <= b * 3ull;
}

void Video::TrackResolveSource(u32 flags, GuestTexture *dst, u32 level,
                               u32 face) {
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  dst->resolveLevel = level;
  dst->resolveFace = face;
  const int exp_bias = static_cast<i32>(flags) >> kResolveExponentBiasShift;
  dst->resolveScale = static_cast<float>(std::ldexp(1.0, exp_bias));
  GuestTexture *src = ResolveSourceForFlagsLocked(s, flags);
  if (!src || !src->texture || src == dst) {
    DetachSourceSurfaceLocked(s, dst);
    return;
  }
  if (dst->sourceSurface && dst->sourceSurface != src)
    dst->sourceSurface->destinationTextures.erase(dst);
  dst->sourceSurface = src;
  src->destinationTextures.insert(dst);
  BindTextureSRVLocked(s, src);
}

namespace {

void NoteTileContentLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->texture || !FullscreenChainClassLocked(s, dst))
    return;
  s.fullscreen_chain_head = dst;
}

} // namespace

namespace {

// A resolve of part of the surface: a straight region copy, which needs the
// same format and size class on both sides (no scaling or MSAA).
bool CopySurfaceRegionLocked(VideoState &s, GuestTexture *src, GuestTexture *dst,
                             const plume::RenderRect *src_rect, u32 dst_x,
                             u32 dst_y) {
  if (src->format != dst->format || dst->resolveScale != 1.0f ||
      src->hostScale != dst->hostScale ||
      src->sampleCount != plume::RenderSampleCount::COUNT_1 ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  const bool dst_is_cube =
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;
  const u32 level = dst_is_cube ? dst->resolveLevel : 0u;
  const u32 dst_w = std::max(dst->width >> level, 1u);
  const u32 dst_h = std::max(dst->height >> level, 1u);
  i32 left = src_rect ? src_rect->left : 0;
  i32 top = src_rect ? src_rect->top : 0;
  i32 right = src_rect ? src_rect->right : i32(src->width);
  i32 bottom = src_rect ? src_rect->bottom : i32(src->height);
  // Clip to the source surface, then to what fits at the destination point.
  left = std::max(left, 0);
  top = std::max(top, 0);
  right = std::min(right, i32(src->width));
  bottom = std::min(bottom, i32(src->height));
  right = std::min(right, left + i32(dst_w) - i32(dst_x));
  bottom = std::min(bottom, top + i32(dst_h) - i32(dst_y));
  if (right <= left || bottom <= top || dst_x >= dst_w || dst_y >= dst_h)
    return true; // nothing lands inside the destination

  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;

  plume::RenderTextureBarrier pre[2] = {
      plume::RenderTextureBarrier(src->texture,
                                  plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(dst->texture,
                                  plume::RenderTextureLayout::COPY_DEST),
  };
  s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, std::size(pre));
  src->layout = plume::RenderTextureLayout::COPY_SOURCE;
  dst->layout = plume::RenderTextureLayout::COPY_DEST;
  const plume::RenderBox box(left, top, right, bottom);
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(
          dst->texture, level, dst_is_cube ? dst->resolveFace : 0u),
      plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0), dst_x,
      dst_y, 0, &box);
  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

} // namespace

void Video::ResolveRtToTexture(GuestTexture *dst,
                               const plume::RenderRect *src_rect, u32 dst_x,
                               u32 dst_y) {
  RR6_CPU_ZONE("ResolveRtToTexture");
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  GuestTexture *src = dst->sourceSurface;
  if (!src || src == dst || !src->texture)
    return;

  // The guest's rect and point are in guest pixels, and each host surface is
  // scaled by its own host scale.
  const i32 src_scale = i32(src->hostScale);
  plume::RenderRect scaled_rect;
  if (src_rect) {
    scaled_rect = {src_rect->left * src_scale, src_rect->top * src_scale,
                   src_rect->right * src_scale, src_rect->bottom * src_scale};
    src_rect = &scaled_rect;
  }
  dst_x *= dst->hostScale;
  dst_y *= dst->hostScale;

  // A sub-rectangle (or an offset destination) that is not simply the whole
  // surface onto the whole texture.
  const bool partial =
      (src_rect && (src_rect->left != 0 || src_rect->top != 0 ||
                    u32(src_rect->right) != src->width ||
                    u32(src_rect->bottom) != src->height)) ||
      dst_x != 0 || dst_y != 0;
  if (partial) {
    if (CopySurfaceRegionLocked(s, src, dst, src_rect, dst_x, dst_y)) {
      s.last_resolved_dst = dst;
      DetachSourceSurfaceLocked(s, dst);
      return;
    }
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      RR6_WARN("Resolve: partial resolve {}x{} fmt={} -> {}x{} fmt={} scale={} "
               "needs a conversion, resolving the whole surface",
               src->width, src->height, static_cast<int>(src->format),
               dst->width, dst->height, static_cast<int>(dst->format),
               dst->resolveScale);
    }
  }

  if (CanAliasResolveLocked(src, dst)) {
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    s.draw_framebuffer_bound = false;
    return;
  }

  if (CopySurfaceToTextureLocked(s, src, dst, "Resolve")) {
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    DetachSourceSurfaceLocked(s, dst);
  }
}

u32 TargetScaleLocked(VideoState &s) {
  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);
  if (rt)
    return rt->hostScale;
  if (ds)
    return ds->hostScale;
  return Output::ResolutionScale();
}

GuestTexture *SampledTextureLocked(const VideoState &s, GuestTexture *tex) {
  GuestTexture *src = tex ? tex->sourceSurface : nullptr;
  if (src && src->texture &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      src != s.render_target && src != s.depth_stencil &&
      src->descriptorIndex != kInvalidDescriptorIndex)
    return src;
  return tex;
}

namespace {

// A render-target texture of `like`'s format at w x h, for the guest scale
// copy. Never resized: a texture's size is fixed for its lifetime.
std::unique_ptr<GuestTexture> CreateGuestScaleCopy(VideoState &s,
                                                   const GuestTexture *like,
                                                   u32 w, u32 h,
                                                   u32 host_scale) {
  auto copy = std::make_unique<GuestTexture>(ResourceType::Texture);
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = w;
  desc.height = h;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = like->format;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.committed = false;
  copy->textureHolder =
      CreateHostTexture(s.device.get(), desc, "guest-scale-copy");
  copy->texture = copy->textureHolder.get();
  if (!copy->texture)
    return nullptr;
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = like->format;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = 1;
  copy->textureView = copy->texture->createTextureView(view_desc);
  copy->width = w;
  copy->height = h;
  copy->depth = 1;
  copy->format = like->format;
  copy->guestFormat = like->guestFormat;
  copy->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  copy->hostScale = host_scale;
  BindTextureSRVLocked(s, copy.get());
  if (copy->descriptorIndex == kInvalidDescriptorIndex)
    return nullptr;
  return copy;
}

} // namespace

// A pass into a lower-scale target than the textures it samples must see them
// at its own resolution, as the guest does. RR6's glow (Output::SurfaceScale)
// bright-passes the 1280x720 frame into its 320x180 buffer with one bilinear
// tap; reading the scaled frame directly, that tap would cover a fraction of
// its 4x4 guest texels and alias. So the frame is box-filtered to 1280x720
// first, and the tap reads that.
void BuildGuestScaleCopiesLocked(VideoState &s) {
  const u32 target_scale = TargetScaleLocked(s);
  for (u32 i = 0; i < 16; ++i) {
    s.scale_copies[i] = nullptr;
    GuestTexture *tex = s.textures[i];
    if (!tex || !tex->texture || tex->hostScale <= target_scale ||
        tex->viewDimension != plume::RenderTextureViewDimension::TEXTURE_2D ||
        tex->mipLevels > 1 || !IsRenderTargetCapable(tex->format) ||
        tex->width % tex->hostScale || tex->height % tex->hostScale)
      continue;
    // A texture bound to several slots is copied once.
    for (u32 j = 0; j < i && !s.scale_copies[i]; ++j) {
      if (s.textures[j] == tex)
        s.scale_copies[i] = s.scale_copies[j];
    }
    if (s.scale_copies[i])
      continue;
    GuestTexture *src = SampledTextureLocked(s, tex);
    if (!src->texture || src->sampleCount != plume::RenderSampleCount::COUNT_1)
      continue;
    const u32 w = tex->width / tex->hostScale * target_scale;
    const u32 h = tex->height / tex->hostScale * target_scale;
    if (!tex->guestScaleCopy) {
      tex->guestScaleCopy = CreateGuestScaleCopy(s, tex, w, h, target_scale);
      if (!tex->guestScaleCopy) {
        RR6_WARN_FIRST(4, "guest scale copy {}x{} fmt={} could not be created",
                       w, h, static_cast<int>(tex->format));
        continue;
      }
    }
    GuestTexture *copy = tex->guestScaleCopy.get();
    if (copy->width != w || copy->height != h)
      continue;
    if (CopySurfaceToTextureLocked(s, src, copy, "GuestScaleCopy"))
      s.scale_copies[i] = copy;
  }
}

} // namespace rr6::gpu
