/**
 * @file    gpu/d3d.h
 * @brief   Guest layouts of the XDK 2.0.2135 D3D9 objects RR6's code reads and
 *          writes.
 *
 * Ported from reblue (Copyright (c) 2026 Tom Clay, BSD 3-Clause; see
 * thirdparty notices). The D3DDevice block is rr6's own, recovered from RR6's
 * D3D code (config/d3d.toml). The resource headers below are still reblue's
 * Blue Dragon layouts and are NOT verified for XDK 2135: RR6's vertex buffer
 * header is 20 bytes with the reference count in Common's low byte, its
 * textures 40 bytes and its surfaces 64. They are only safe once the resource
 * hooks own every accessor, so revisit them with the resource milestone.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

namespace rr6::gpu {

// D3DRESOURCETYPE values from the guest D3D9 SDK. D3DResource::Common carries
// the type in its low 4 bits, so these double as the Common base type bits.
enum class D3DResourceType : u32 {
  kSurface = 1,
  kTexture = 3,
  kVolumeTexture = 4,
  kCubeTexture = 5,
  kVertexBuffer = 6,
  kIndexBuffer = 7,
};

// 24-byte header shared by every D3D9 resource (D3DResource ord 933).
struct D3DResource {
  be_u32 Common;         // +0x00  type/flag bits, read by D3DResource_GetType
  be_u32 ReferenceCount; // +0x04  engine increments/decrements directly
  be_u32 Fence;          // +0x08
  be_u32 ReadFence;      // +0x0C
  be_u32 Identifier;     // +0x10
  be_u32 BaseFlush;      // +0x14
};
static_assert(sizeof(D3DResource) == 24);

// The pViewport argument to D3DDevice_SetViewport (BD 0x82473610), and the
// layout of the device's cached viewport at D3DDevice+0x3058.
struct D3DViewport9 {
  be_u32 X;
  be_u32 Y;
  be_u32 Width;
  be_u32 Height;
  be_f32 MinZ;
  be_f32 MaxZ;
};
static_assert(sizeof(D3DViewport9) == 24);

// Guest RECT (LTRB): the pRect argument to D3DDevice_SetScissorRect
// (BD 0x82473548).
struct D3DRect {
  be_i32 left;
  be_i32 top;
  be_i32 right;
  be_i32 bottom;
};
static_assert(sizeof(D3DRect) == 16);

// GPUTEXTURE_FETCH_CONSTANT (ord 853): 6-dword Xenos texture descriptor at
// D3DTexture+0x1C. reblue-created textures leave it zero since their accessors
// are hooked. Engine-created bdAllocRenderBuffer textures carry a real one that
// native_texture_mirror decodes.
struct GpuTextureFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(GpuTextureFetchConstant) == 24);

// D3DTexture / D3DBaseTexture / D3DCubeTexture / D3DVolumeTexture, 52 bytes.
struct D3DTexture {
  D3DResource resource;           // +0x00
  be_u32 MipFlush;                // +0x18
  GpuTextureFetchConstant Format; // +0x1C
};
static_assert(sizeof(D3DTexture) == 52);
static_assert(offsetof(D3DTexture, Format) == 0x1C);

// D3DSurface, 48 bytes. D3DSurface_GetDesc (0x8246D668, non-texture path)
// unpacks Width/Height from the dword at +0x24:
//   Width  = (SizeBits >> 18) + 1            (width-1 in bits [31:18])
//   Height = ((SizeBits >> 3) & 0x7FFF) + 1  (height-1 in bits [17:3])
// Bits [2:0] are pad.
struct D3DSurface {
  D3DResource resource; // +0x00
  be_u32 SurfaceInfo;   // +0x18  GPU_SURFACEINFO union word
  be_u32 DepthInfo;     // +0x1C  GPU_DEPTHINFO union word
  be_u32 HiControl;     // +0x20
  be_u32 SizeBits;      // +0x24  packed Height/Width
  be_u32 Format;        // +0x28  D3DFORMAT
  be_u32 Size;          // +0x2C
};
static_assert(sizeof(D3DSurface) == 48);

// Verified against the code at 0x8246D668. Written by D3DSurface_GetDesc
// and D3DTexture_GetLevelDesc.
struct D3DSurfaceDesc {
  be_u32 Format;             // +0x00  guest D3DFORMAT
  be_u32 Type;               // +0x04  D3DRESOURCETYPE
  be_u32 Usage;              // +0x08  always written 0 by reblue
  be_u32 Pool;               // +0x0C  always written 0 by reblue
  be_u32 MultiSampleType;    // +0x10  0, since reblue surfaces are COUNT_1
  be_u32 MultiSampleQuality; // +0x14  0
  be_u32 Width;              // +0x18  surface / mip-level width
  be_u32 Height;             // +0x1C  surface / mip-level height
};
static_assert(sizeof(D3DSurfaceDesc) == 32);

// Written by D3DVolumeTexture_GetLevelDesc. Adds Depth and drops the
// MultiSample fields, so NOT the D3DSurfaceDesc layout.
struct D3DVolumeDesc {
  be_u32 Format; // +0x00  guest D3DFORMAT
  be_u32 Type;   // +0x04  4 == D3DRTYPE_VOLUMETEXTURE
  be_u32 Usage;  // +0x08  0
  be_u32 Pool;   // +0x0C  0
  be_u32 Width;  // +0x10  mip-level width
  be_u32 Height; // +0x14  mip-level height
  be_u32 Depth;  // +0x18  mip-level depth
};
static_assert(sizeof(D3DVolumeDesc) == 28);

// D3DLOCKED_RECT. Points the engine at the texture's guest scratch mirror.
struct D3DLockedRect {
  be_u32 Pitch; // +0x00  row pitch in bytes
  be_u32 pBits; // +0x04  guest VA of the locked pixel scratch
};
static_assert(sizeof(D3DLockedRect) == 8);

// D3DVertexBuffer / D3DIndexBuffer, 32 bytes. The +0x18/+0x1C pair is the Xenos
// fetch constant, encoded two ways depending on who wrote it:
//   Pre-XGOffset (bdPhysical*Create replication, raw dwords):
//     VB writes size@FetchLo/base@FetchHi, IB writes (size|3)@FetchLo/
//     ((base & 0x3FFFFFC)|0x10000002)@FetchHi.
//   Post-XGOffsetResourceAddress (asset-loaded structs, Bootstrap path):
//     FetchLo = base_va | tag_bits, FetchHi = (size & 0x3FFFFFC) | 0x10000002.
// reblue's own mirror and bootstrap paths use the post-XGOffset encoding.
struct D3DVertexBuffer {
  D3DResource resource;
  be_u32 FetchLo; // +0x18
  be_u32 FetchHi; // +0x1C
};
static_assert(sizeof(D3DVertexBuffer) == 32);
static_assert(offsetof(D3DVertexBuffer, FetchLo) == 0x18);
static_assert(offsetof(D3DVertexBuffer, FetchHi) == 0x1C);

struct D3DIndexBuffer {
  D3DResource resource;
  be_u32 FetchLo; // +0x18
  be_u32 FetchHi; // +0x1C
};
static_assert(sizeof(D3DIndexBuffer) == 32);
static_assert(offsetof(D3DIndexBuffer, FetchLo) == 0x18);
static_assert(offsetof(D3DIndexBuffer, FetchHi) == 0x1C);

// D3DVertexShader / D3DPixelShader / D3DVertexDeclaration, 24 bytes each.
struct D3DVertexShader {
  D3DResource resource;
};
struct D3DPixelShader {
  D3DResource resource;
};
struct D3DVertexDeclaration {
  D3DResource resource;
};
static_assert(sizeof(D3DVertexShader) == 24);
static_assert(sizeof(D3DPixelShader) == 24);
static_assert(sizeof(D3DVertexDeclaration) == 24);

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


// The Common low-4 bits select the base type. Texture dimensionality lives in
// the fetch constant. Bit 30 (0x40000000) on a surface marks it texture-backed,
// which routes D3DSurface_GetDesc through D3DTexture_GetLevelDesc.
constexpr u32 kCommonFlagTextureBackedSurface = 0x40000000u;

constexpr u32 kClearTarget = 0x1u;
constexpr u32 kClearZBuffer = 0x10u;
constexpr u32 kClearStencil = 0x20u;
constexpr u32 kClearAll = kClearTarget | kClearZBuffer | kClearStencil;

constexpr u32 kResolveDepthStencil = 0x4u;
constexpr u32 kResolveExponentBiasShift = 26;

inline void InitResourceHeader(D3DResource &r, D3DResourceType type) {
  r.Common = u32(type);
  r.ReferenceCount = 1u;
  r.Fence = 0u;
  r.ReadFence = 0u;
  r.Identifier = 0u;
  r.BaseFlush = 0u;
}

} // namespace rr6::gpu
