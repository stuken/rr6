// rr6 - ReXGlue Recompiled Project
//
// xenos plugin build only: rr6_dump_frames="N,..." records the destination
// texture of every D3DDevice_Resolve the guest issues during present frame N,
// then three presents later (the plugin executes resolves on its own thread)
// untiles each one from guest memory and writes
// rr6_frame_N_rt_<VA>_<W>x<H>.bmp, the same names the native renderer's
// frame dumps use, so the two builds' intermediate surfaces can be compared
// directly. Only 32bpp 2D formats are handled; the texel values written
// (alpha included, in the BMP's fourth byte) are what a shader would sample
// through the fetch swizzle. The plugin keeps resolve output on the GPU
// unless told otherwise, so run with
//   --readback_resolve=full --d3d12_readback_resolve=true

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_STRING(rr6_dump_frames, "", kCvarGroup,
                      "Comma-separated present frame numbers whose resolve "
                      "targets are written as BMPs in the user data folder");

REXCVAR_DEFINE_INT32(rr6_trace_frame, 0, kCvarGroup,
                     "Log the D3D calls of this present frame and the next two "
                     "(render targets, draws with their textures, resolves)");

REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_Clear);

namespace {

namespace tu = rex::graphics::texture_util;

// XDK 2135 D3DBaseTexture: the fetch constant sits at +16.
constexpr u32 kTextureFetchOffset = 16;
constexpr u32 kDumpDelay = 3;

std::mutex g_mutex;
std::vector<u32> g_wanted;
bool g_parsed = false;
u32 g_frame = 0; // presents completed
struct Pending {
  u32 frame;
  std::vector<u32> textures;
};
std::vector<Pending> g_pending;

bool Tracing() {
  const i32 wanted = REXCVAR_GET(rr6_trace_frame);
  // Three presents: the plugin build's frames do not always hold a whole
  // scene render between two presents.
  return wanted > 0 && g_frame + 1 >= static_cast<u32>(wanted) &&
         g_frame + 1 < static_cast<u32>(wanted) + 3;
}

// D3DDevice: render targets at +0x30F8, textures at +0x31A0.
void TraceCall(const char *what, PPCContext &ctx, const std::string &extra) {
  const u32 dev = ctx.r3.u32;
  const auto *rt = rr6::mem::at<const be_u32>(dev + 0x30F8);
  const auto *tex = rr6::mem::at<const be_u32>(dev + 0x31A0);
  std::string t;
  for (int i = 0; tex && i < 4; ++i)
    if (u32(tex[i]))
      t += std::format(" t{}=0x{:08X}", i, u32(tex[i]));
  RR6_INFO("[xtrace] {}{} rt=0x{:08X}{} lr=0x{:08X}", what, extra,
           rt ? u32(rt[0]) : 0, t, u32(ctx.lr));
}

void ParseWanted() {
  if (g_parsed)
    return;
  g_parsed = true;
  const std::string text = REXCVAR_GET(rr6_dump_frames);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(',', pos);
    if (end == std::string::npos)
      end = text.size();
    if (end > pos)
      g_wanted.push_back(static_cast<u32>(std::stoul(text.substr(pos, end - pos))));
    pos = end + 1;
  }
}

bool WriteBmp(const std::filesystem::path &path, u32 width, u32 height,
              const std::vector<u8> &bgra) {
  FILE *f = std::fopen(path.string().c_str(), "wb");
  if (!f)
    return false;
  const u32 image = width * height * 4;
  u8 header[54] = {'B', 'M'};
  auto put32 = [&](int off, u32 v) { std::memcpy(header + off, &v, 4); };
  auto put16 = [&](int off, u16 v) { std::memcpy(header + off, &v, 2); };
  put32(2, 54 + image);
  put32(10, 54);
  put32(14, 40);
  put32(18, width);
  put32(22, static_cast<u32>(-static_cast<i32>(height))); // top-down
  put16(26, 1);
  put16(28, 32);
  put32(34, image);
  std::fwrite(header, 1, sizeof(header), f);
  std::fwrite(bgra.data(), 1, image, f);
  std::fclose(f);
  return true;
}

void DumpTexture(u32 frame, u32 va, const std::filesystem::path &dir) {
  const auto *words = rr6::mem::at<const be_u32>(va + kTextureFetchOffset);
  auto *memory = REX_KERNEL_MEMORY();
  if (!words || !memory)
    return;
  u32 fc[6];
  for (int i = 0; i < 6; ++i)
    fc[i] = u32(words[i]);
  const u32 format = fc[1] & 0x3F;
  const u32 endian = (fc[1] >> 6) & 3;
  const u32 base = fc[1] & 0xFFFFF000;
  const bool tiled = (fc[0] >> 31) & 1;
  const u32 pitch = ((fc[0] >> 22) & 0x1FF) * 32;
  const u32 width = (fc[2] & 0x1FFF) + 1;
  const u32 height = ((fc[2] >> 13) & 0x1FFF) + 1;
  const u32 swizzle = (fc[3] >> 1) & 0xFFF;
  // k_8_8_8_8, k_2_10_10_10, k_8_8_8_8_A: 32bpp unorm formats.
  if (format != 6 && format != 7 && format != 10) {
    RR6_INFO("rr6_dump_frames: skip 0x{:08X} format {}", va, format);
    return;
  }
  // With readback on, the resolved texels land one 32x32 tile (4 KiB at
  // 32bpp) past the fetch constant's base: every RR6 resolve compared against
  // the native renderer's dump was shifted by exactly one tile otherwise.
  const u8 *src = memory->TranslatePhysical(base) + (tiled ? 4096u : 0u);
  const u32 aligned_h = (height + 31) & ~31u;
  std::vector<u8> bgra(size_t(width) * height * 4);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const u32 off = tiled ? u32(tu::GetTiledOffset2D(i32(x), i32(y), pitch, 2))
                            : (y * pitch + x) * 4;
      if (off + 4 > pitch * aligned_h * 4)
        continue;
      u8 b[4];
      std::memcpy(b, src + off, 4);
      u32 v;
      switch (endian) {
      case 2: // 8in32
        v = u32(b[0]) << 24 | u32(b[1]) << 16 | u32(b[2]) << 8 | b[3];
        break;
      case 1: // 8in16
        v = u32(b[1]) << 24 | u32(b[0]) << 16 | u32(b[3]) << 8 | b[2];
        break;
      case 3: // 16in32
        v = u32(b[2]) << 24 | u32(b[3]) << 16 | u32(b[0]) << 8 | b[1];
        break;
      default:
        v = u32(b[3]) << 24 | u32(b[2]) << 16 | u32(b[1]) << 8 | b[0];
        break;
      }
      u8 comp[6] = {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24), 0, 255};
      if (format == 7) { // 2_10_10_10: keep the top 8 bits of each channel
        comp[0] = u8((v >> 2) & 0xFF);
        comp[1] = u8((v >> 12) & 0xFF);
        comp[2] = u8((v >> 22) & 0xFF);
        comp[3] = u8(((v >> 30) & 3) * 85);
      }
      auto pick = [&](u32 sel) { return comp[std::min<u32>(sel, 5)]; };
      u8 *out = bgra.data() + (size_t(y) * width + x) * 4;
      out[2] = pick(swizzle & 7);
      out[1] = pick((swizzle >> 3) & 7);
      out[0] = pick((swizzle >> 6) & 7);
      out[3] = pick((swizzle >> 9) & 7);
    }
  }
  const auto path = dir / std::format("rr6_frame_{}_rt_{:08X}_{}x{}.bmp", frame,
                                      va, width, height);
  if (WriteBmp(path, width, height, bgra))
    RR6_INFO("rr6_dump_frames: wrote {} fmt={} endian={} tiled={} swz={:03X}",
             path.string(), format, endian, u32(tiled), swizzle);
}

} // namespace

REX_HOOK_RAW(D3DDevice_SetRenderTarget) {
  if (Tracing())
    RR6_INFO("[xtrace] SetRenderTarget {} 0x{:08X} lr=0x{:08X} frame={}",
             ctx.r4.u32, ctx.r5.u32, u32(ctx.lr), g_frame + 1);
  __imp__D3DDevice_SetRenderTarget(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawVertices) {
  if (Tracing())
    TraceCall("Draw", ctx, std::format(" prim={} n={}", ctx.r4.u32, ctx.r6.u32));
  __imp__D3DDevice_DrawVertices(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  if (Tracing())
    TraceCall("DrawIndexed", ctx,
              std::format(" prim={} n={}", ctx.r4.u32, ctx.r7.u32));
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
}

REX_HOOK_RAW(D3DDevice_BeginVertices) {
  if (Tracing())
    TraceCall("BeginVertices", ctx,
              std::format(" prim={} n={}", ctx.r4.u32, ctx.r5.u32));
  __imp__D3DDevice_BeginVertices(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Clear) {
  if (Tracing())
    TraceCall("Clear", ctx, std::format(" flags=0x{:X}", ctx.r6.u32));
  __imp__D3DDevice_Clear(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Resolve) {
  if (Tracing())
    TraceCall("Resolve", ctx,
              std::format(" flags=0x{:X} dest=0x{:08X}", ctx.r4.u32, ctx.r6.u32));
  {
    std::lock_guard lock(g_mutex);
    ParseWanted();
    const u32 frame = g_frame + 1;
    const u32 dest = ctx.r6.u32;
    if (dest && std::find(g_wanted.begin(), g_wanted.end(), frame) != g_wanted.end()) {
      if (g_pending.empty() || g_pending.back().frame != frame)
        g_pending.push_back({frame, {}});
      auto &list = g_pending.back().textures;
      if (std::find(list.begin(), list.end(), dest) == list.end())
        list.push_back(dest);
    }
  }
  __imp__D3DDevice_Resolve(ctx, base);
}

// Called by input_script.cpp's Present wrapper once per present.
void Rr6XenosDumpOnPresent() {
  std::lock_guard lock(g_mutex);
  ParseWanted();
  ++g_frame;
  if (g_pending.empty() || g_pending.front().frame + kDumpDelay != g_frame)
    return;
  std::filesystem::path dir = rex::Runtime::instance()
                                  ? rex::Runtime::instance()->user_data_root()
                                  : std::filesystem::path(".");
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  for (u32 va : g_pending.front().textures)
    DumpTexture(g_pending.front().frame, va, dir);
  g_pending.erase(g_pending.begin());
}
