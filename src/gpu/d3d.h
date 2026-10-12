/**
 * @file    gpu/d3d.h
 * @brief   Guest layouts of the XDK 2.0.2135 D3D9 objects RR6's code reads and
 *          writes.
 *
 * Ported from reblue (Copyright (c) 2026 Tom Clay, BSD 3-Clause; see
 * thirdparty notices). The D3DDevice block and the resource headers are rr6's
 * own, recovered from RR6's D3D code (config/d3d.toml): the creators
 * (D3DDevice_CreateVertexBuffer/IndexBuffer/Texture/Surface and
 * D3DTexture_GetSurfaceLevel), D3DResource_AddRef/Release/Destroy/GetType,
 * D3DDevice_SetStreamSource and the lock and desc accessors. XDK 2135's
 * headers are smaller than Blue Dragon's: the reference count lives in Common,
 * and there is no ReadFence or Identifier.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

namespace rr6::gpu {

// D3DResource::Common, as XDK 2135 packs it:
//   bits 0-7    reference count (D3DResource_AddRef/Release)
//   bits 16-18  header kind (D3DCommonType), what D3DResource_Destroy and
//               D3DResource_GetType switch on
//   bits 19-23  device bind count: binding adds 0x80000, unbinding subtracts
//               it, and the resource is destroyed once both counts are zero
//   bit 24      set on every header D3D creates
//   bit 25      the header does not own its memory (a texture level's surface
//               from GetSurfaceLevel), so Destroy frees only the header
//   bit 30      created with usage 0x4, its data in cached memory
//   bit 31      INDEX32 on an index buffer. On a surface, EDRAM that
//               CreateSurface allocated and Destroy returns.
constexpr u32 kCommonRefCountMask = 0x000000FFu;
constexpr u32 kCommonTypeShift = 16;
constexpr u32 kCommonTypeMask = 0x00070000u;
constexpr u32 kCommonBindCountMask = 0x00F80000u;
constexpr u32 kCommonD3DCreated = 0x01000000u;
constexpr u32 kCommonNoOwnedMemory = 0x02000000u;
constexpr u32 kCommonCachedMemory = 0x40000000u;
constexpr u32 kCommonIndex32 = 0x80000000u;
constexpr u32 kCommonEdramAllocated = 0x80000000u;

// Common bits 16-18.
enum class D3DCommonType : u32 {
  kVertexBuffer = 1,
  kIndexBuffer = 2,
  kTexture = 3, // 2D, cube, volume or array: the fetch constant says which
  kSurface = 4,
};

inline D3DCommonType CommonType(u32 common) {
  return D3DCommonType((common & kCommonTypeMask) >> kCommonTypeShift);
}

// D3DRESOURCETYPE, what D3DResource_GetType returns. For textures and surfaces
// it reads the dimension out of the fetch constant.
enum class D3DResourceType : u32 {
  kSurface = 1,
  kVolume = 2,
  kTexture = 3,
  kVolumeTexture = 4,
  kCubeTexture = 5,
  kVertexBuffer = 6,
  kIndexBuffer = 7,
  kArrayTexture = 8,
};

// 12-byte header shared by every D3D9 resource.
struct D3DResource {
  be_u32 Common; // +0x00  the kCommon* bits above
  // +0x04  GPU fence of the last use: unbinding stores the device's current
  // fence, and Destroy and Lock block on it while it is non-zero.
  be_u32 Fence;
  be_u32 BaseFlush; // +0x08  0xFFFF0000 at create
};
static_assert(sizeof(D3DResource) == 12);

// The pViewport argument to D3DDevice_SetViewport (0x82257488), and the
// layout of the device's cached viewport at D3DDevice+0x3208.
struct D3DViewport9 {
  be_u32 X;
  be_u32 Y;
  be_u32 Width;
  be_u32 Height;
  be_f32 MinZ;
  be_f32 MaxZ;
};
static_assert(sizeof(D3DViewport9) == 24);

// Guest RECT (LTRB), the device's scissor rect at D3DDevice+0x3220.
struct D3DRect {
  be_i32 left;
  be_i32 top;
  be_i32 right;
  be_i32 bottom;
};
static_assert(sizeof(D3DRect) == 16);

// GPUTEXTURE_FETCH_CONSTANT: the 6-dword Xenos texture descriptor that
// textures and surfaces carry at +0x10. native_texture_mirror decodes it.
struct GpuTextureFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(GpuTextureFetchConstant) == 24);

// D3DTexture / D3DCubeTexture / D3DVolumeTexture / D3DArrayTexture, 40 bytes.
// D3DResource_Destroy frees the base (dword 1) and mip (dword 5) allocations
// the fetch constant names.
struct D3DTexture {
  D3DResource resource;           // +0x00
  be_u32 MipFlush;                // +0x0C  0xFFFF0000 at create
  GpuTextureFetchConstant Format; // +0x10
};
static_assert(sizeof(D3DTexture) == 40);
static_assert(offsetof(D3DTexture, Format) == 0x10);

// D3DSurface, 64 bytes. It carries a texture fetch constant of its own at the
// same offset as a texture's, so D3DSurface_GetDesc runs the texture level
// desc on it (or, with kCommonNoOwnedMemory, on Parent at the level in
// Format dword 4 bits 2-5).
struct D3DSurface {
  D3DResource resource;           // +0x00
  be_u32 Unused0C;                // +0x0C  never written by the creators
  GpuTextureFetchConstant Format; // +0x10  dword 2 holds width-1, height-1
  // +0x28  bytes: EDRAM tiles * 5120 for a created surface, the level's
  // size for a level surface.
  be_u32 Size;
  be_u32 Parent;                  // +0x2C  D3DTexture* of a level surface
  // +0x30  RB_SURFACE_INFO: EDRAM pitch in bits 0-13 (rounded up to the
  // 80-pixel tile), MSAA samples in bits 16-17 (the MultiSampleType GetDesc
  // reports), HiZ pitch from bit 18.
  be_u32 SurfaceInfo;
  // +0x34  RB_COLOR_INFO / RB_DEPTH_INFO: EDRAM base tile in bits 0-11,
  // format from bit 16.
  be_u32 EdramInfo;
  be_u32 HiZInfo; // +0x38  bit 0: owns the HiZ area
  be_u32 Unused3C; // +0x3C
};
static_assert(sizeof(D3DSurface) == 64);
static_assert(offsetof(D3DSurface, Format) == 0x10);
static_assert(offsetof(D3DSurface, Parent) == 0x2C);
static_assert(offsetof(D3DSurface, SurfaceInfo) == 0x30);

// Written by D3DSurface_GetDesc and D3DTexture_GetLevelDesc (0x82251DC8).
struct D3DSurfaceDesc {
  be_u32 Format;             // +0x00  guest D3DFORMAT
  be_u32 Type;               // +0x04  D3DRESOURCETYPE
  be_u32 Usage;              // +0x08  always 0
  be_u32 Pool;               // +0x0C  always 0
  be_u32 MultiSampleType;    // +0x10  SurfaceInfo bits 16-17, else 0
  be_u32 MultiSampleQuality; // +0x14  always 0
  be_u32 Width;              // +0x18  surface / mip-level width
  be_u32 Height;             // +0x1C  surface / mip-level height
};
static_assert(sizeof(D3DSurfaceDesc) == 32);

// D3DVertexBuffer, 20 bytes. Fetch0/Fetch1 are the first two words of the
// buffer's Xenos vertex fetch constant: the data's VA | 3 (the vertex fetch
// type), then its size in bytes (bits 2-25) | 2 (the endian swap).
// D3DVertexBuffer_Lock and D3DResource_Destroy mask them the same way.
struct D3DVertexBuffer {
  D3DResource resource; // +0x00
  be_u32 Fetch0;        // +0x0C
  be_u32 Fetch1;        // +0x10
};
static_assert(sizeof(D3DVertexBuffer) == 20);
static_assert(offsetof(D3DVertexBuffer, Fetch0) == 0x0C);

// D3DIndexBuffer, 20 bytes: the data's VA and size, both raw. Common's
// kCommonIndex32 marks 32-bit indices.
struct D3DIndexBuffer {
  D3DResource resource; // +0x00
  be_u32 Address;       // +0x0C
  be_u32 Size;          // +0x10
};
static_assert(sizeof(D3DIndexBuffer) == 20);
static_assert(offsetof(D3DIndexBuffer, Address) == 0x0C);

// Shaders and vertex declarations are not D3DResources in XDK 2135: a shader
// is a 52-byte header followed by a copy of its container (the physical
// microcode at +12), and a declaration keeps its reference count at +4. The
// renderer tracks both by the address the creator returns and reads neither
// header.

// The microcode blob CreateVertexShader/CreatePixelShader are handed, layout
// mirrors thirdparty/XenosRecomp/XenosRecomp/shader.h.
struct ShaderContainer {
  be_u32 flags;
  be_u32 virtualSize;
  be_u32 physicalSize;
  be_u32 fieldC;
  be_u32 constantTableOffset;
  be_u32 definitionTableOffset;
  be_u32 shaderOffset;
  be_u32 field1C;
  be_u32 field20;
};
static_assert(sizeof(ShaderContainer) == 0x24);

// Direct3D_CreateDevice (0x8225A260) allocates this through D3D_AllocDevice.
constexpr u32 kD3DDeviceAllocSize = 0x5080;

// dword[0] bits [1:0] of a fetch constant tag what it describes.
constexpr u32 kFetchTypeMask = 0x3;
constexpr u32 kFetchTypeTexture = 2;
constexpr u32 kFetchTypeVertex = 3;

// 24-byte Xenos GPU fetch descriptor. D3DDevice holds 32 in fetchConstants[].
struct DeviceFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(DeviceFetchConstant) == 24);

// Render and sampler state dispatch, {Get, Set, Default} per entry. The guest
// init (0x822612B0) copies Set/Get into the device and calls Set(Default).
constexpr u32 kRenderStateTableGuestAddr = 0x823763E0;
constexpr u32 kRenderStateTableEntries = 97;
constexpr u32 kSamplerStateTableGuestAddr = 0x82376870;
constexpr u32 kSamplerStateTableEntries = 20;
constexpr u32 kSamplerCount = 26;

// Shared guest memory. Only fields with a citation in config/d3d.toml are
// named. Everything else is padding until a function proves its meaning.
struct D3DDevice {
  be_u32 m_pRing;          // +0x000  D3D__MakeSpace compares against the limit
  be_u32 m_pRingEnd;       // +0x004  hard end; 0x8225C540 sizes its chunks to it
  be_u32 m_pRingLimit;     // +0x008
  be_u32 m_ReferenceCount; // +0x00C  D3DDevice_Release
  be_u64 m_Mask[5];        // +0x010  dirty masks the Set* functions OR into
  u8 pad_038[0x060 - 0x038];
  be_u32 m_SetRenderStateCall[kRenderStateTableEntries];   // +0x060
  be_u32 m_SetSamplerStateCall[kSamplerStateTableEntries]; // +0x1E4
  be_u32 m_GetRenderStateCall[kRenderStateTableEntries];   // +0x234
  be_u32 m_GetSamplerStateCall[kSamplerStateTableEntries]; // +0x3B8
  u8 pad_408[0x480 - 0x408];
  DeviceFetchConstant fetchConstants[32]; // +0x480  D3DDevice_SetTexture
  be_f32 vsFloatConstants[256][4];        // +0x780  SetVertexShaderConstantF
  be_f32 psFloatConstants[256][4];        // +0x1780 SetPixelShaderConstantF
  u8 pad_2780[0x2C90 - 0x2780];
  be_u32 vertexDeclaration; // +0x2C90  D3DDevice_SetVertexDeclaration
  u8 pad_2C94[0x2CB8 - 0x2C94];
  // Blend as set through D3DRS (SRCBLEND..BLENDOPALPHA), applied to the four
  // RB_BLENDCONTROLs only while ALPHABLENDENABLE (blendFlags bit 31) is on.
  // Bit 30 is SEPARATEALPHABLENDENABLE.
  be_u32 blendStaged; // +0x2CB8
  be_u32 blendFlags;  // +0x2CBC
  u8 pad_2CC0[0x2D04 - 0x2CC0];
  // PA_SC_WINDOW_SCISSOR_TL/BR (0x2081/0x2082): x in bits 0-14, y in 16-30,
  // BR exclusive. D3DDevice_SetScissorRect (0x822573C0) stores the viewport
  // rectangle, clipped to the D3DRS_SCISSORTESTENABLE rect when that is on.
  be_u32 paScWindowScissorTL; // +0x2D04
  be_u32 paScWindowScissorBR; // +0x2D08
  u8 pad_2D0C[0x2D14 - 0x2D0C];
  be_u32 indexOffset; // +0x2D14  draws compare and update it
  u8 pad_2D18[0x2D1C - 0x2D18];
  // Xenos register shadows the render-state setters write. The 0x21xx group
  // starts at +0x2D0C and the 0x22xx group at +0x2D74, one dword per register.
  be_u32 rbColorMask;            // +0x2D1C  RB_COLOR_MASK (0x2104)
  u8 pad_2D20[0x2D3C - 0x2D20];
  be_u32 rbStencilRefMaskBF;     // +0x2D3C  RB_STENCILREFMASK_BF (0x210C)
  be_u32 rbStencilRefMask;       // +0x2D40  RB_STENCILREFMASK (0x210D)
  be_f32 rbAlphaRef;             // +0x2D44  RB_ALPHA_REF (0x210E), 0..1
  be_f32 viewportScaleOffset[6]; // +0x2D48  D3DDevice_SetViewport
  u8 pad_2D60[0x2D74 - 0x2D60];
  be_u32 rbDepthControl;      // +0x2D74  RB_DEPTHCONTROL (0x2200)
  be_u32 rbBlendControl0;     // +0x2D78  RB_BLENDCONTROL0 (0x2201)
  be_u32 rbColorControl;      // +0x2D7C  RB_COLORCONTROL (0x2202)
  u8 pad_2D80[0x2D84 - 0x2D80];
  // PA_CL_CLIP_CNTL (0x2204): bits 0-5 enable user clip planes 0-5
  // (D3DRS_CLIPPLANEENABLE, 0x82255C90).
  be_u32 paClClipCntl;        // +0x2D84
  be_u32 paSuScModeCntl;      // +0x2D88  PA_SU_SC_MODE_CNTL (0x2205)
  // PA_CL_VTE_CNTL (0x2206): D3DRS_VIEWPORTENABLE (0x82256480) writes 0x43F,
  // or 0x400 with the viewport off, when bits 0-3 (x/y scale and offset)
  // clear mean the VS already outputs window coordinates.
  be_u32 paClVteCntl;         // +0x2D8C
  u8 pad_2D90[0x2D98 - 0x2D90];
  be_u32 rbBlendControl1[3];  // +0x2D98  RB_BLENDCONTROL1..3 (0x2209..)
  u8 pad_2DA4[0x2E00 - 0x2DA4];
  // Bit 0 is PA_SU_VTX_CNTL.PIX_CENTER, the only bit D3DRS_HALFPIXELOFFSET
  // (0x82256850) writes: 0 (the default) puts pixel centres at integer
  // vertex positions, as in Direct3D 9.
  be_u32 paSuVtxCntl;           // +0x2E00
  u8 pad_2E04[0x2E90 - 0x2E04];
  // PA_SU_POLY_OFFSET_{FRONT,BACK}_{SCALE,OFFSET}. D3DRS_SLOPESCALEDEPTHBIAS
  // (0x82255CD8) stores the scale x16, in 1/16 subpixel units, and
  // D3DRS_DEPTHBIAS (0x82255D98) the offset as is; both set
  // PA_SU_SC_MODE_CNTL bits 11/12 (front/back enable) when either is non-zero.
  be_f32 polyOffsetFrontScale;  // +0x2E90
  be_f32 polyOffsetFrontOffset; // +0x2E94
  be_f32 polyOffsetBackScale;   // +0x2E98
  be_f32 polyOffsetBackOffset;  // +0x2E9C
  u8 pad_2EA0[0x30F4 - 0x2EA0];
  be_u32 indexBuffer;           // +0x30F4  D3DDevice_SetIndices
  be_u32 renderTargets[4];      // +0x30F8  D3DDevice_SetRenderTarget
  be_u32 depthStencil;          // +0x3108  D3DDevice_SetDepthStencilSurface
  be_u32 streams[16][2];        // +0x310C  {offset, vertex buffer}
  u8 pad_318C[0x3190 - 0x318C];
  u8 streamStrides[16];         // +0x3190  stride / 4
  be_u32 textures[kSamplerCount]; // +0x31A0  D3DDevice_SetTexture
  D3DViewport9 viewport;      // +0x3208  D3DDevice_SetViewport
  D3DRect scissorRect;        // +0x3220  D3DDevice_SetScissorRect
  be_f32 clipPlanes[6][4];    // +0x3230  D3DDevice_SetClipPlane
  be_u32 pixelShader;         // +0x3290  D3DDevice_SetPixelShader
  u8 pad_3294[0x4FE8 - 0x3294];
  be_u32 vertexShader;        // +0x4FE8  D3DDevice_SetVertexShader
  u8 pad_4FEC[0x5060 - 0x4FEC];
  be_u32 behaviorFlags;       // +0x5060  Direct3D_CreateDevice
  u8 pad_5064[0x5080 - 0x5064];
};
static_assert(sizeof(D3DDevice) == kD3DDeviceAllocSize);
static_assert(offsetof(D3DDevice, m_Mask) == 0x010);
static_assert(offsetof(D3DDevice, m_SetRenderStateCall) == 0x060);
static_assert(offsetof(D3DDevice, m_SetSamplerStateCall) == 0x1E4);
static_assert(offsetof(D3DDevice, m_GetRenderStateCall) == 0x234);
static_assert(offsetof(D3DDevice, m_GetSamplerStateCall) == 0x3B8);
static_assert(offsetof(D3DDevice, fetchConstants) == 0x480);
static_assert(offsetof(D3DDevice, vsFloatConstants) == 0x780);
static_assert(offsetof(D3DDevice, psFloatConstants) == 0x1780);
static_assert(offsetof(D3DDevice, vertexDeclaration) == 0x2C90);
static_assert(offsetof(D3DDevice, blendFlags) == 0x2CBC);
static_assert(offsetof(D3DDevice, paScWindowScissorTL) == 0x2D04);
static_assert(offsetof(D3DDevice, indexOffset) == 0x2D14);
static_assert(offsetof(D3DDevice, rbColorMask) == 0x2D1C);
static_assert(offsetof(D3DDevice, rbStencilRefMask) == 0x2D40);
static_assert(offsetof(D3DDevice, rbAlphaRef) == 0x2D44);
static_assert(offsetof(D3DDevice, rbDepthControl) == 0x2D74);
static_assert(offsetof(D3DDevice, rbColorControl) == 0x2D7C);
static_assert(offsetof(D3DDevice, paClClipCntl) == 0x2D84);
static_assert(offsetof(D3DDevice, paSuScModeCntl) == 0x2D88);
static_assert(offsetof(D3DDevice, paClVteCntl) == 0x2D8C);
static_assert(offsetof(D3DDevice, rbBlendControl1) == 0x2D98);
static_assert(offsetof(D3DDevice, viewportScaleOffset) == 0x2D48);
static_assert(offsetof(D3DDevice, paSuVtxCntl) == 0x2E00);
static_assert(offsetof(D3DDevice, polyOffsetFrontScale) == 0x2E90);
static_assert(offsetof(D3DDevice, indexBuffer) == 0x30F4);
static_assert(offsetof(D3DDevice, renderTargets) == 0x30F8);
static_assert(offsetof(D3DDevice, depthStencil) == 0x3108);
static_assert(offsetof(D3DDevice, streams) == 0x310C);
static_assert(offsetof(D3DDevice, streamStrides) == 0x3190);
static_assert(offsetof(D3DDevice, textures) == 0x31A0);
static_assert(offsetof(D3DDevice, viewport) == 0x3208);
static_assert(offsetof(D3DDevice, scissorRect) == 0x3220);
static_assert(offsetof(D3DDevice, clipPlanes) == 0x3230);
static_assert(offsetof(D3DDevice, pixelShader) == 0x3290);
static_assert(offsetof(D3DDevice, vertexShader) == 0x4FE8);
static_assert(offsetof(D3DDevice, behaviorFlags) == 0x5060);


constexpr u32 kClearTarget = 0x1u;
constexpr u32 kClearZBuffer = 0x10u;
constexpr u32 kClearStencil = 0x20u;
constexpr u32 kClearAll = kClearTarget | kClearZBuffer | kClearStencil;

constexpr u32 kResolveDepthStencil = 0x4u;
constexpr u32 kResolveExponentBiasShift = 26;

} // namespace rr6::gpu
