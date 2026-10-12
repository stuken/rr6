# XenosRecomp shader integration plan for rr6

Reference implementation: `D:\reblue` (v1.3.1). SDK: rexglue 0.10.0 (both projects).

---

## Status (2026-10-10)

| Phase | State |
|---|---|
| 0. Extraction | **Done.** `scripts/extract_shaders.py` gives 176 unique shaders (103 VS, 73 PS) from 244 containers. No runtime census is needed: `D3DDevice_Create{Vertex,Pixel}Shader` each have exactly one caller (the boot loader `0x82222900`, which walks the 122-pair table at `0x82373508`), and no data pointer references them outside `.pdata`. |
| 1. XenosRecomp fork | **Done.** `thirdparty/XenosRecomp`, branch `rr6`, uncommitted. All 176 shaders produce DXIL and SPIR-V with 0 failures. |
| 2. Build integration | **Done.** `rr6_shader_cache` (OBJECT library) builds from `default.xex`. Nothing links it yet. The `src/CMakeLists.txt` and `rr6_common` restructure is deferred to Phase 4, where the sources first appear. |
| 3. D3D identification | **Done for the hook set.** `config/d3d.toml` names 75 functions and is included from `rr6_manifest.toml`. Scratch codegen applies all 75 with no warnings. The device layout is in that file's header. `scripts/re/funcdb.py` queries the codegen output. Render-state names are open (see below). |
| 4. Native renderer | **Milestone 4 done.** With `-DRR6_NATIVE_GPU=ON` the game plays from boot through the menus into a race, and the race scene, car select and course intro match the xenos plugin (verified against real hardware). The car's dynamic environment reflections are still missing. `OFF` still builds and runs the xenos plugin. |
| 5 | Not started |

### Phase 4, milestone 1 (what exists)

**Build**

- `cmake/native_gpu.cmake`
- `cmake/shaders.cmake` (host HLSL, DXIL only)
- `tools/prelink_shader_cache.cpp` → `rr6_prelink`
- plume as a local clone in `thirdparty/plume`
- miniz and zstd in `thirdparty/`

**Renderer**

- Most of reblue's `src/gpu` was ported with `bd::`→`rr6::`, `BD_*`→`RR6_*` and `bd_*` cvars → `rr6_*`.
- Removed: the BD hooks, the sun occlusion, the bloom clamp, and the PSO predictor, precache and recorded cache. `pso_recorder.cpp` is a stub.
- Minimal rr6 versions of `core/{logging,settings,encoding}` and `platform/platform.h`.

**Guest layout**

- `src/gpu/d3d.h` has rr6's `D3DDevice` (`0x5080`) with only the fields `config/d3d.toml` cites.
- The resource headers are still BD's and unverified for XDK 2135 (see the file header).

**Hooks** (`src/gpu/hooks/device.cpp`)

- **`Direct3D_CreateDevice`:**
  - allocates the device and publishes it in `VdGlobalDevice`;
  - runs the guest's own `D3D__InitializeStateTables`;
  - creates and binds the default back buffer and depth surfaces through the guest's `CreateSurface`/`SetRenderTarget`/`SetDepthStencilSurface`, because RR6 reads RT0 back immediately.
- **`D3DDevice_Clear`** and **`D3DDevice_Present`** call into `Video`.
- **`D3D__MakeSpace`** and **`D3D__AllocCommandMemory`** turn the guest's PM4 ring and command memory into discard buffers. Every unhooked guest D3D function still runs and writes its packets there harmlessly. The ring's hard end at `+0x4` and the allocator callback at `+0x3534` were the two hangs found on the way.

**Diagnostics**

- `platform/guest_abort_log.cpp` logs the guest registers on `REX_FATAL`/abort and the host pc (as `rr6.exe+offset`) on unrecovered faults. Resolve the pc with `llvm-symbolizer --obj=rr6.exe`.
- `rr6_d3d12_debug=true` turns on the D3D12 debug layer and logs its messages.
- Present runs at about 16 fps in this session, which is over Remote Desktop. 90% of the main thread's time is the swapchain fence wait, so measure locally before treating it as a renderer cost.

### Phase 4, milestone 2 (what was added)

**Approach: wrap, don't replace.** RR6's own D3D code still creates and owns every guest object (shaders, declarations, surfaces, textures, buffers), so its unhooked internals stay consistent. The host keeps mirrors keyed by guest address:

- **Shaders** (`hooks/shader.cpp`): `Create*Shader` builds a `GuestShader` from the XenosRecomp cache (122/122 VS and PS hits, all host shaders built at boot); `Set*Shader` binds it.
- **Render targets, depth and declarations** (`hooks/state.cpp`):
  - the game's default back buffer maps onto `Video`'s host back buffer;
  - other surfaces get `SurfaceRegistry` targets sized through the guest's own `D3DSurface_GetDesc`;
  - declarations go through reblue's `CreateVertexDeclaration`.
- **Draws** (`hooks/draw.cpp`): `DrawVertices` and `DrawIndexedVertices` are replaced. `BeginVertices`/`EndVertices` are wrapped (and, since milestone 4, `DrawVerticesUP`). Each draw binds host mirrors of the device's streams, index buffer and textures.
- **Buffers** (`gpu/guest_buffers.cpp`):
  - XDK 2135 vertex buffers carry a Xenos vertex fetch constant at `+12`/`+16`;
  - index buffers carry the raw base and size there, with INDEX32 in Common bit 31;
  - mirrors are rehashed once per frame and re-uploaded only when the guest bytes change.
- **Textures** (`native_texture_mirror.cpp`):
  - the fetch constant is at RR6's `+16`;
  - mirrors are built on first bind and rebuilt when the fetch constant changes;
  - linear (untiled) 2D 8888 is supported;
  - linear textures are rehashed per frame because the CPU rewrites them.
  - The game writes `device.textures[]` itself rather than calling `SetTexture`.
- **Render state** (`draw.cpp`, `d3d.h`): decoded straight from the Xenos register shadows the setters write.
  - RB_DEPTHCONTROL `+0x2D74`, RB_BLENDCONTROL0 `+0x2D78`, RB_COLORCONTROL `+0x2D7C`, PA_SU_SC_MODE_CNTL `+0x2D88`;
  - RB_COLOR_MASK `+0x2D1C`, RB_STENCILREFMASK `+0x2D40`, RB_ALPHA_REF `+0x2D44`;
  - the ALPHABLENDENABLE flag at `+0x2CBC` bit 31.
  - The setters insert raw D3D values, so reblue's converters apply unchanged. The render-state enum order matches BD's (index 24 = ALPHATESTENABLE).
- **Back buffer:** sized to the guest's 1280x720, and Present scales it to the window. Rendering at output resolution needs viewport scaling RR6 does not have yet.

**Diagnostics:**

- `rr6_dump_frames=N,M` writes those present frames to `<user>/rr6_frame_N.bmp`, plus every resolve target.
- Under `rr6_devmode`, draw statistics are logged at frames 60 and 600.
- The crash logger also scans the host stack for `rr6.exe` return addresses.

**Fixes outside rr6:** `thirdparty/plume`'s `D3D12CommandList::copyTextureRegion` dereferenced a null destination texture on texture-to-buffer copies. It is fixed locally, uncommitted, and a candidate for upstream.

### Phase 4, milestone 3 (what was added)

**Rendering fixes**

- **Constants:** VS and PS constants upload on every draw. RR6 has no dirty signal, and stale constants caused the washed-out, overlapping menu layers.
- **Textures:**
  - `k_8` (the movie's Y/U/V planes), DXN (as BC5) and linear 2D textures are supported.
  - The fetch constant's swizzle is applied to every format, which fixed the red/blue swap.
  - Single-channel formats replicate R, and DXN replicates G into B and A (RGGG), as Xenos and xenia do.
- **Resolve:**
  - the clear flags (`0x100` color, `0x200` depth/stencil) are honoured;
  - every resolve target can be dumped (`rr6_dump_frames`).
- **EDRAM aliasing:** a workaround that copied the last-drawn surface's top-left region on a resolve with no intervening draw. It was a misdiagnosis of the missing `DrawVerticesUP` stretch (milestone 4) and has been removed.
- **Present:** BD's scanout ramp (x^0.5 plus a Rec709 reshape) is now identity. RR6 leaves its gamma ramp linear, and xenia shows the front buffer as is.

**Test tooling**

- **`rr6_input_script`:**
  - scripted pad input, timed by present frame or by seconds;
  - the xenos build counts frames by wrapping the guest's `D3DDevice_Present`, so both builds run the same script and can be compared frame for frame.
- **`rr6_null_audio`:** a silent audio backend (`platform/silent_audio.h`).
  - RR6 treats a failed `XAudioRegisterRenderDriverClient` as fatal: its sound manager at `0x824E2454` stays null and the guest faults at boot.
  - SDL fails that way with no output device, as over a disconnected Remote Desktop session, and so does the SDK's `NopAudioSystem`.
- **`rr6_trace_frame`:** logs, for frame N:
  - every draw, with texture formats, swizzles and signs;
  - PS constants for full-screen passes;
  - render-target binds with guest and host formats;
  - clears;
  - resolves, with the source surface's pitch and EDRAM base words and the guest call chain.
- **`rr6_dump_frames`:** takes a list of frames.

### Phase 4, milestone 4 (what was fixed)

Xenos was confirmed correct against hardware. Each fix below was found by comparing intermediate surfaces between the two builds (see the tooling at the end).

- **Race brightness (XenosRecomp):** Xenos co-issues an ALU instruction's vector and scalar halves, and both read their sources before either writes. The recompiler emitted the vector write first, so a scalar op reading the vector destination saw the new value. In the environment fog shader that turned `rsqrt(phase)` into `rsqrt(distance * FogCoeffExp.z)`, and the in-scatter term washed every wall, tree and building blue-white. The recompiler now snapshots the register before the vector write (`s<N>` temps). 65 of the 176 shaders were affected.
- **Car select backdrop:** the preview is composited with alpha test GREATER, ref 0 (`RB_COLORCONTROL` = `0x8700000C`), with blending off. Native only implemented `clip(a - ref)`, which keeps alpha 0. The shared constants now carry `ALPHA_FUNC` (`c20.w`), and the recompiler emits `ALPHA_TEST(oC0.w)`, which implements all eight compares (`shader_common.h`).
- **Viewport:** native ignored the guest viewport and reset it to the surface size on `SetRenderTarget` and `SetDepthStencilSurface`, writing that back into the guest device. Draws now take the viewport from the device (`+0x3208`, which agrees with the register shadow at `+0x2D48`), and the hooks no longer write it.
- **Depth size:** Xenos depth shares the colour surface's EDRAM pitch, so RR6 pairs a 1280x720 depth surface with its 1440x800 race target. D3D12 clips rendering to the smaller attachment, so the host depth texture now grows to cover its colour target.
- **`DrawVerticesUP`:** XDK 2135's version calls `BeginVertices` but closes the packet inline, never calling `EndVertices`, so native dropped every one. That included the triangle-fan stretch of the 1440x800 scene into the 1280x720 back buffer that feeds the post chain. It is now wrapped. Triangle fans, which D3D12 lacks, draw through a fan index buffer (`Video::FanExpansionIBView`). Together with the viewport and depth fixes, this removed the race crop and the black band on the course intro.
- **Car shadow flicker (depth bias):** the 13 car shadow quads (`DrawVerticesUP`, quad list, a 384x512 DXT5 texture per car) are coplanar with the road and rely on `D3DRS_SLOPESCALEDEPTHBIAS`, which native ignored, so they z-fought. The device keeps `PA_SU_POLY_OFFSET_{FRONT,BACK}_{SCALE,OFFSET}` at `+0x2E90`, with the enables in `PA_SU_SC_MODE_CNTL` bits 11 and 12. Draws convert them as xenia does: the offset times 2^24, rounded away from zero, and the scale over 16. Where the device supports dynamic depth bias, the bias is set on the command list after every guest PSO bind. Otherwise it is baked into the PSO, with the slope rounded up to a multiple of 1/4 because the shadow bias changes with the camera every frame. The test GPU (AMD 8060S) takes the baked path.
- **Half-pixel offset (Pac-Man seams):** RR6 leaves `PA_SU_VTX_CNTL.PIX_CENTER` (device `+0x2E00` bit 0, written by `D3DRS_HALFPIXELOFFSET`) at 0, which means Direct3D 9 pixel centres. Native passed a zero `g_HalfPixelOffset`, so the intro's 224x288 Pac-Man texture, drawn 2x with the guest's -0.5 pixel pre-offset, sampled exactly on texel boundaries and doubled or dropped columns. The shared constants now carry (1/viewport width, -1/viewport height) while `PIX_CENTER` is 0, as xenia applies it, and every recompiled VS adds it to `oPos`.
- **Cube resolve targets:** a resolve destination whose fetch constant is a cube is created as a cube, and each resolve lands in the face it names.
- **User clip planes (car reflections):** the dynamic environment pass draws each 256x256 half of its 256x512 target twice with different cameras, split along the diagonal by clip plane 0 (`(1,-1,0,-0.002)` and its negation). Native ignored clip planes, so the second camera overwrote the first and the env map, and with it the cars' reflections, came out as a dark jumble. The shared constants now carry `PA_CL_CLIP_CNTL` bits 0-5 (device `+0x2D84`, `D3DRS_CLIPPLANEENABLE`) and the six planes from `+0x3230` (`c21.z`, `c22..c27`). Every recompiled VS writes them as `SV_ClipDistance0/1` at exit (`WRITE_USER_CLIP_DISTANCES`), after its interpolators so the pixel shader's input registers are unchanged.
- **`k_DXT5A` textures (square smoke and flames):** the nitrous flame and exhaust sprites use 1024x1024 `k_DXT5A` (format 59, a single DXT5 alpha block per 4x4). Native rejected the format, so those draws sampled no texture and came out as solid squares. It now uploads as BC4, with the channel replicated to every component as Xenos does.
- **Indexed quad lists (nitrous blur):** the nitrous radial blur (`ps_820615E0`, a feedback blur of the previous frame) is an indexed quad list of 1024 indices over a shared vertex grid. Native drew every quad list through the consecutive-vertex expansion buffer, ignoring the guest indices, so the mesh collapsed to a few pixels (`no_blur.rdc`, event 4607). Indexed quad lists now expand the guest's own indices into a triangle list in the upload heap (`DrawIndexedQuadList`). Verified in game.
- **Scissor:** draws take the guest window scissor (`PA_SC_WINDOW_SCISSOR_TL/BR` at `+0x2D04`/`+0x2D08`). `D3DDevice_SetScissorRect` stores the viewport there, clipped to the `D3DRS_SCISSORTESTENABLE` rect when that is on. Native had used the viewport alone.

**Comparison tooling**

- **xenos `rr6_dump_frames`** (`platform/xenos_resolve_dump.cpp`): the plugin build dumps every resolve target of frame N from guest memory, with the native build's file names. Run it with `--readback_resolve=full --d3d12_readback_resolve=true`. Without those, xenia keeps resolves on the GPU.
- **xenos `rr6_trace_frame`:** logs render-target binds, draws (with textures), clears and resolves over three presents.
- **Native trace additions:** fetch exp-adjust and dimension, all 16 texture slots, colour control and alpha ref, viewport and register viewport, UP vertex data, and the VS/PS constants of the first scene draws.
- **Dumps keep alpha**, in the BMP's fourth byte.
- **Readback offset:** with `--readback_resolve=full`, xenia's resolved texels sit one 32x32 tile (4 KiB at 32bpp) past the fetch base, so `rr6_dump_frames` reads tiled resolves from there. Earlier xenos dumps, including the race scene references, were shifted 32 pixels right.

### Phase 4, next milestones

1. **Coverage:** checked against xenos: boot, Pac-Man, title, main menu, World Xplorer, car select, course intro, the Harborline 765 and Lakeshore Drive races (Single Race), the intro movie, and the attract demo (Rave City Riverfront at night, Highland Cliffs). All match apart from timing. The attract demo's course is random per run, so it can't be paired frame for frame. Open: whether the car headlight flares are fixed by the same change.
- **Sun visibility (partial resolves):** RR6's D3D has no occlusion queries. The sun instead draws a probe disc into the scene with a depth test, resolves the 64x64 region around it (`D3DDevice_Resolve` with a source rect), and the sun's glow draw samples that 64x64 texture to scale itself (RenderDoc capture `sun_visible.rdc`, events 2829-2864). Native resolved the whole 1440x800 surface shrunk into 64x64, so the glow read unrelated pixels and showed through the cliff. Resolves now honour the source rect and destination point with a region copy (`CopySurfaceRegionLocked`) when formats match and nothing is scaled; otherwise they fall back to a whole-surface resolve with a warning. The probe itself also never drew: the clear quad, the probe quad and the restore disc are drawn with `D3DRS_VIEWPORTENABLE` off, so PA_CL_VTE_CNTL (device `+0x2D8C`, 0x400 rather than 0x43F) leaves their positions in render target pixels. Native always applied the viewport, so they landed far off screen. Every recompiled VS now ends with `oPos.xy = oPos.xy * g_NdcScale + (g_NdcOffset + g_HalfPixelOffset) * oPos.w`. The scale and offset (`c28`) are the identity normally, and map window coordinates onto the host viewport while x/y scale and offset are off. Verified in game on Highland Cliffs (`sun_visible2.rdc` was the capture before this fix).
2. **Resources:**
   - verify the XDK 2135 VB/IB/texture/surface headers (VB: 20 bytes, refcount in Common's low byte; texture: 40; surface: 64);
   - then port reblue's `hooks/resource.cpp` against them: create, lock and unlock, `AddRef`/`Release`/`Destroy`, `GetDesc`/`GetLevelDesc`, `GetSurfaceLevel`, and the cube and volume variants.
3. **`Resolve`:** depth resolves are not modeled yet, nor partial resolves that need a format conversion or scaling.
4. **Resolution scaling:** done, host-side as xenia does it. The guest still sees 1280x720.
   - `rr6_resolution_scale` (integer, 1-4) multiplies everything the host renders into:
     - every render surface (`HostSurface`);
     - the back buffer (`GuestBackBufferDims`);
     - every resolve target (`GetOrCreateResolveTarget`), cubes included.
   - The default, 0, takes the render height over 720, rounded up: 3x at 2160p and 2x at 1080p, which supersamples. The scale is latched on first use, so a window resize keeps it and Present scales as before.
   - Viewports and scissors stay in guest pixels everywhere and are scaled only in `FlushViewport`. `SetDefaultViewport` divides host sizes back down.
   - Resolve source rects and destination points are scaled at the top of `ResolveRtToTexture`. Since both sides of every copy carry the same factor, the region copies, the cube faces and the box-filter downsample keep their exact ratios. That is why only integer scales are allowed.
   - Shaders need no other changes:
     - none of RR6's 176 reads its pixel position (`svPosRegister`);
     - every `tfetch` offset is zero;
     - blur steps come from game constants in UV space, so they cover the same screen distance at any scale;
     - the half-pixel and window-coordinate (`VTE`) constants are computed from the guest viewport, so they stay in guest pixels.
   - Checked at 2x against 1x: boot, title, menus, World Xplorer, car select, course intro and the Harborline race all match when downscaled (mean difference under 1 outside race timing). The Pac-Man intro has no seams, and every resolve target comes out at twice its size. 3x (auto at 2160p) reaches the race at the same pace as 1x.
   - **Edge snap (white border at 3x and above):** the full-screen stretch into the 1280x720 post chain is pre-offset by about a quarter guest pixel (NDC −1.0004/+1.0007). After the half-pixel shift its left and top edges sit at guest 0.244. The guest tests coverage at guest pixel centres, so on hardware the outer column is covered, and at 2x the first host pixel centre (0.25) still is. At 3x (0.167) and above it isn't, so the outer host pixels kept the white clear colour, and later passes smeared it to 2 px. Every recompiled VS now ends with `SNAP_TO_VIEWPORT_EDGES(oPos)`, which moves a vertex less than half a guest pixel inside a viewport edge onto the edge (`g_EdgeSnap`, `c29.xy`, zero at 1x). Checked at 3x and 4x: race and menu edges match 1x and xenos, the Pac-Man intro has no seams, and 1x is pixel-identical.
   - **Settings were never adopted:** `rr6::gpu::Settings::Init()` had no caller, so every renderer setting sat at its compiled default. `Rr6App::OnPostInitLogging` now calls it, as reblue does.
   - **Settings trimmed to what rr6 uses:** `rr6_anisotropy` (16 by default, for every all-LINEAR sampler), `rr6_supersampling`, `rr6_vsync`, `rr6_geometry_gpu_upload` and `rr6_dred`, plus `rr6_resolution_scale` and `rr6_fps_limit`. Removed:
     - the Blue Dragon settings, whose consumers were BD hooks rr6 doesn't have: `rr6_msaa`, `rr6_post_quality`, `rr6_reflection_quality`, `rr6_ntsc_filter`, `rr6_dof_strength`, `rr6_shadow_dimension`, `rr6_shadow_distance` and `rr6_fov_offset`, along with the quality presets built on them;
     - `rr6_aspect_ratio`, whose modes relied on BD's reprojection. rr6 now always fits the guest's 16:9 into the window with bars;
     - `rr6_scene_color_r11g11b10`: it has no alpha, and the sun probe writes the scene's alpha;
     - `rr6_pso_precache`: the precache is a stub;
     - the dead design-canvas helpers in `Output`.
     - Old config files that still name removed settings load fine; unknown keys are ignored.
   - **Supersampling:** `rr6_supersampling=2` doubles the resolution scale (up to 8x; 1440 × 8 stays inside D3D12's 16384 limit). That is 6x at 2160p, and it still reaches the race at the same pace as 1x on the 8060S. Present's blit (`gamma_correction_ps`) now area-filters when it shrinks by more than 2:1, averaging up to 4×4 bilinear taps across each output pixel's footprint. At 2:1 and below it is still one tap.
   - **MSAA:** skipped and removed. RR6 requests none (`msaa=0` on the scene target), and resolution scaling covers anti-aliasing.
   - Frame dumps are written at the scaled size, so comparisons against xenos dumps need `--rr6_resolution_scale=1`.
   - Verified in game at scale: the sun visibility probe on Highland Cliffs and the nitrous blur.
5. **Lifetime:** done (`hooks/resource.cpp`).
   - `D3DResource_Destroy` (called by `D3DResource_Release` and the device's unbinding at refcount zero) drops the mirror by `Common` type (bits 16-18):
     - vertex and index buffers: `EvictGuestBuffer`, keyed by the data base at `+12`;
     - textures: `EvictGuestTexture`, which drops the texel mirror, any resolve target and the cached signature. Without this, a freed render texture's resolve target would shadow whatever texture was created next at its address;
     - surfaces: `ForgetGuestSurface`, which unmaps the guest surface and returns the host surface to `SurfaceRegistry` (refcount 0) for reuse or retirement.
   - Course and menu textures never reach `Destroy`: their headers live inside loaded data, and the memory is freed wholesale. So `XPhysicalAlloc` (`sub_82248480`) and `XPhysicalFree` (`sub_82248508`, the only callers of `MmAllocatePhysicalMemoryEx` and `MmFreePhysicalMemory`) are hooked as well. Freeing an allocation drops every texture and buffer mirror whose base or mip address lies inside it. RR6's fetch constants carry the data's virtual address (`0xE…`/`0xF…`), so ranges are compared as virtual addresses.
   - Lookups now take the mirror mutex, because physical frees run on several guest threads.
   - Boot → Harborline race → pause → Menu → World Xplorer → second race: 1038 texture mirrors dropped by frees and 84 by `Destroy`. Both races render the same as with eviction disabled.
   - A temporary check found no stale mirrors even with eviction off: RR6 reloads a course byte-identically at the same addresses. A different course in the same heap is the case this protects against; it frees host memory either way.
   - `rr6_log_destroy=true` logs every destroy and every free that drops mirrors.

### Phase 3 findings

- **Shape of the XDK 2135 API**
  - `D3DDevice_Present` (`0x8225A008`) is the per-frame entry. It saves state, resolves, then calls the internal `D3DDevice_Swap` (`0x82259A28`, which calls `VdSwap`) and restores. Blue Dragon has no `Present`, so its `Swap` hook maps to RR6's `Present`.
  - Exports the game never calls: `Reset`, queries, fences, `BlockUntilIdle`, tiling, ZPass, `DrawIndexedVerticesUP`, gamma ramp. Those reblue hooks and stubs have no RR6 counterpart.
  - Shader literals: `SetPixelShader` and `SetVertexShader` copy each literal to device `+0x480 + address`. That is why the recompiler's literal register formula holds.
- **Render and sampler state**
  - They go through the device dispatch tables: 568 inline `lwz/mtctr/bctrl` sites in game code, covering 55 render-state indices and 9 sampler-state indices.
  - As in reblue, Phase 4 should keep the guest setters, which write the device register shadows, and read those shadows at draw time.
  - Naming the 97 render-state entries needs each setter's target register. That is open, and is Phase 4 work.
- **Offsets compared with Blue Dragon**
  - Dispatch tables and fetch/float constants sit a uniform `0x28`/`0x80` further in (`+0x60`, `+0x480`, `+0x780`, `+0x1780`).
  - The device is `0x5080` bytes.
  - Everything past the constants differs. Use `config/d3d.toml`, not `gpu/d3d.h`.

| reblue hook | RR6 |
|---|---|
| `Direct3D_CreateDevice` | `0x8225A260` |
| `D3DDevice_Clear` | `0x82253D50` (`ClearF` `0x82253648`); Stencil is in r9 as in BD |
| `D3DDevice_Swap` | `D3DDevice_Present` `0x8225A008` |
| `Draw{Vertices,VerticesUP,IndexedVertices}`, `Begin/EndVertices` | `0x82254128`, `0x822540A0`, `0x82254368`, `0x82253E00`/`0x82254080` |
| `D3DDevice_Resolve` | `0x82258980` |
| `Create{Surface,Texture,VertexBuffer,IndexBuffer}` | `0x82252CB8`, `0x82252B98`, `0x82254C70`, `0x82254DA0` |
| VB/IB `Lock`/`Unlock`, `D3DResource_*`, `LockRect`, `GetDesc` | all named in `config/d3d.toml`; RR6 adds cube and volume variants |
| `Create{VertexShader,PixelShader,VertexDeclaration}` | `0x8224EF40`, `0x8224EE50`, `0x8224ECA0` |
| `Set{Viewport,RenderTarget,DepthStencilSurface,ScissorRect}` | `0x82257488`, `0x82257C78`, `0x82257FE0`, `0x822573C0` |
| `Set{VertexShader,PixelShader,VertexDeclaration,Texture,StreamSource,Indices}` | `0x8224EC38`, `0x8224E8C8`, `0x8224F018`, `0x822530A8`, `0x822576F8`, `0x82257798` |
| Ring buffer stubs | `SetRingBufferParameters` `0x8225B8A0`, `D3D__MakeSpace` `0x8225B5E0` |
| `bdSetRenderState` (BD wrapper) | none; device dispatch slots at `+0x60` |

### Container revision 0x0E, as implemented (`XenosRecomp/shader.h`)

| Item | Layout |
|---|---|
| Header | `flags, virtualSize, physicalSize, definitionTableOffset, constantTableOffset, shaderOffset` |
| Constant table | `constantTableOffset` points at `{u32 size; CTAB}`, the same as 0x11 |
| Shader struct | Starts at `shaderOffset`, immediately after the CTAB |
| Shader struct, common words | `[0]` program control, `[4]` `interpolatorInfo` |
| PS words | `[6]` interpolator mask, `[7]` outputs, `[8..]` interpolators |
| VS words | `[6]` `field18` (= 1 leading entry), `[7]` element count, `[8]` trailing count, `[10..]` `field18` + elements + interpolators + trailing |
| Element and interpolator encoding | The same bitfields as 0x11 |
| Definition table | 6-dword header (`[4]` = byte size, `[5]` = uninitialised padding), then `{u16 address; u16 floatCount; float values[]}` runs and two null terminators |
| Literal register | `((address & 0xFFF) >> 4) + 208`. The compiler packs literals downward from c255. This was checked against every literal read in the microcode. |
| Microcode | Always at `virtualSize + 0`, `physicalSize` bytes long |
| svPos | None. No RR6 pixel shader reads pixel position. |
| Int4 definitions | Unseen in RR6; the recompiler emits `#error` if one appears |

### rr6 shader ABI choices (`shader_common.h`, `RR6_RECOMP`)

- The build defines both `REBLUE_RECOMP` and `RR6_RECOMP`. That gives reblue's
  shared-constant layout, swap masks and structured control flow, which the
  ported renderer expects.
- The Blue Dragon `ShadowTexture` PCF rewrite is gated off. RR6 has 4 shaders
  with a `ShadowTexture` sampler, so it would otherwise fire on them.
- Vertex input locations: Position0, Normal0, Tangent0/1, Binormal0,
  TexCoord0–2 and Color0 (locations 0–8). That is every semantic RR6 declares.
- Observed spec constants: 60 VS use `R11G11B10_NORMAL`, and all 73 PS use
  `ALPHA_TEST`.

---

## 1. How reblue integrates XenosRecomp

XenosRecomp in reblue is not a standalone feature. It feeds reblue's own
plume-based renderer, which replaces the SDK's Xenia-style GPU emulation
outright. The chain runs as follows.

### 1.1 Recompiler (build-time tool)

- `thirdparty/XenosRecomp` is a submodule of the `zolaware/reblue-XenosRecomp`
  fork. Game-specific behaviour sits behind `#ifdef REBLUE_RECOMP`, alongside
  the upstream `UNLEASHED_RECOMP` paths (about 45 sites in
  `shader_recompiler.cpp` and 6 in `shader_common.h`).
- `thirdparty/CMakeLists.txt` builds it as a host executable target:
  `add_subdirectory(XenosRecomp EXCLUDE_FROM_ALL)` with
  `target_compile_definitions(XenosRecomp PRIVATE REBLUE_RECOMP)`.
- `XenosRecomp/shader_common.h` defines the shader ABI. It is shared by three
  consumers:
  1. the recompiler, which embeds it into every generated HLSL file;
  2. reblue's host HLSL (`src/gpu/shaders/hlsl/*.hlsl`, built with `-D REBLUE_RECOMP`);
  3. reblue's C++, through the `REBLUE_SHADER_COMMON_DIR` include path. This
     carries the `SPEC_CONSTANT_*` bits, the shared-constant offsets and
     `REBLUE_VERTEX_INPUT_LOCATIONS`.

### 1.2 Build integration

| File | Role |
|---|---|
| `cmake/shader_cache.cmake` | `reblue_shader_cache()`: globs `assets/**/*.{vso,pso,xex}`, runs `XenosRecomp <dir> <out.cpp> shader_common.h` as an `add_custom_command`, and creates the `reblue_shader_cache_gen` target. It also defines `reblue_shader_hlsl_dump`, which dumps the intermediate HLSL. |
| `cmake/shaders.cmake` | `reblue_host_shader(stem profile)`: runs dxc on the hand-written HLSL and writes `-Fh` headers (`g_<stem>_dxil` / `g_<stem>_spirv`). `reblue_stage_dxc()` copies `dxcompiler.dll` and `dxil.dll` next to the exe. |
| `cmake/generated.cmake` | Wires both into the `reblue_generated` OBJECT library. Under D3D12 it also builds `reblue_prelink` (`tools/prelink_shader_cache.cpp`), which DXC-links every spec-constant mask subset into `linked_shader_cache.cpp`. |
| `vcpkg.json` | `directx-dxc`, `directx-headers`, `xxhash`, plus the plume submodule. |

The generated `shader_cache.cpp` contains:

- `g_shaderCacheEntries[]`: `{xxh3 hash, dxil off/size, spirv off/size,
  specConstantsMask, GuestShader*}`, sorted by hash;
- a zstd DXIL blob;
- a zstd, smol-v-encoded SPIR-V blob.

The entry struct is declared by hand in `src/gpu/shaders/shader_cache.h`.

### 1.3 Runtime

- `ReblueApp::OnPreSetup` sets `config.graphics = nullptr`, which stops the SDK
  from creating a GPU. `OnPreLaunchModule` then brings up the plume device
  (D3D12 or Vulkan).
- About 55 `REX_HOOK`s replace the guest's statically linked XDK D3D library:
  `Direct3D_CreateDevice`, `D3DDevice_Create{Vertex,Pixel}Shader`,
  `CreateVertexDeclaration`, `Draw*`, `Set*`, `Resolve`, `Swap`, the resource
  lock/unlock calls, and others (see `src/gpu/hooks/*.cpp`). Names come from
  `config/functions.toml`.
- `hooks/shader.cpp` sends `D3DDevice_CreateVertexShader(function)` to
  `gpu/shaders/guest_shaders.cpp::CreateShader`. That hashes
  `XXH3(container, virtualSize+physicalSize)`, which is the same key
  XenosRecomp used, finds the entry with `lower_bound`, and stores it on a
  `GuestShader`.
- `GetOrLinkShader(gs, specConstants)` decompresses the cache once, lazily.
  - **D3D12:** shaders without spec constants are used as-is. Shaders with spec
    constants come from the prelinked table, falling back to a runtime DXC link
    (`shader_linker.cpp` / `dxc_link.cpp`).
  - **Vulkan:** smol-v decode. Spec constant id 0 is supplied per pipeline.
- Draw-time state is read from the guest `D3DDevice` struct (`gpu/d3d.h`).
  Field offsets are XDK-version-specific: the float constants at `+0x700` and
  `+0x1700`, the fetch constants at `+0x400`, and so on. That state is uploaded
  into the VS, PS and shared constant buffers whose layout `shader_common.h`
  defines.

---

## 2. Where rr6 stands

| | reblue (Blue Dragon) | rr6 (Ridge Racer 6) |
|---|---|---|
| GPU | Native plume renderer, `config.graphics = nullptr` | SDK `xenos` plugin (`rexglue_setup_target(rr6 GPU_PLUGINS xenos)`, `config.gpu_plugin = "xenos"`) |
| Shader cache today | XenosRecomp, embedded | Xenia runtime translation, persisted to `cache/shaders/shareable` (`store_shaders` cvar, on by default) |
| D3D symbols | About 1,650 named functions in `config/functions.toml` | None. `rr6_config.toml` lists 21 unnamed addresses. |
| XDK | Later 2.0 XDK | **2.0.2135** (launch-era): D3D9, D3DX9, XGRAPHC statically linked |
| Shader location | Loose `.vso`/`.pso` on disc | **Embedded in `default.xex`**. The XEX is encrypted (AES, retail key) with basic compression. |
| Container revision | `0x102A11xx` | **`0x102A0E00` (PS) / `0x102A0E01` (VS)** |

What I found in rr6 (the scratch scripts are in this session's scratchpad):

- The decrypted XEX image (5.9 MB) holds **122 VS and 122 PS container
  references, 176 of them unique**, each 176–2276 bytes. All 244 parse
  consistently.
- The `vs_3_0`/`ps_3_0` strings in the image are CTAB creator stamps
  ("Microsoft (R) Xbox 360 Shader Compiler 2.0.2135.0"). The shaders are
  precompiled, and there is no sign of runtime HLSL compilation. (reblue needed
  `hcg*CreateByHlsl` hooks for that; rr6 probably does not.)
- `RRM.DAT`, `RRM2.DAT` and `RRM3.DAT` contain no plain shader containers.
  `RRM.DAT` opens with an ASCII hash-like header and is probably a
  compressed or encrypted archive. Whether it hides any further shaders is
  **unconfirmed**; Phase 0 settles it.
- **Rev-0x0E container layout** (it differs from the 0x11 layout XenosRecomp
  expects):
  - The header is 6 dwords: `flags, virtualSize, physicalSize, fieldC,
    constantTableOffset, definitionTableOffset`. The 0x11 header has 9; the
    0x0E one has no `shaderOffset`, `field1C` or `field20`.
  - `constantTableOffset` points at `{u32 size; CTAB}`, a standard D3D CTAB
    with header 0x1C and version `FFFE0300`/`FFFF0300`. That matches 0x11.
  - VS: `flags & 1 == 1`, the same convention `shader_recompiler.cpp:1286` uses.
  - VS `fieldC` (0x18) points at a 0x34-byte block right after the header,
    which looks like literal or definition data. The location of the shader
    microcode and the interpolator and vertex-element tables still needs
    reverse engineering.
  - XenosRecomp's scanner (`main.cpp`, `(flags & 0xFFFFFF00) == 0x102A1100 &&
    field1C == 0 && field20 == 0`) **rejects every rr6 shader as written**.

---

## 3. The key decision

**XenosRecomp output cannot be plugged into the SDK `xenos` plugin.** The
generated HLSL assumes reblue/UnleashedRecomp's binding model: root constant
buffers or push-constant buffer addresses, bindless texture indices in a
shared constant buffer, native input-layout vertex fetch, and DXIL-library
spec constants. Xenia's command processor binds shaders produced by its own
DXBC/SPIR-V translator, which uses a completely different ABI. Getting
reblue-style shader integration therefore means getting reblue-style
rendering too.

| Option | What it is | Cost | Result |
|---|---|---|---|
| **A. Native renderer (the reblue model)** | Null graphics, hook XDK D3D, port reblue's `src/gpu`, embed a XenosRecomp cache | Large. reblue's `src/gpu` is about 14k LOC, roughly half of it portable. Plus D3D RE for XDK 2135. | No runtime shader translation, no EDRAM emulation, full control (resolution, AA, aspect, mods) |
| **B. Keep `xenos`, pre-warm it** | Ship a pre-recorded `cache/shaders/shareable/*` from a full playthrough, or add an SDK hook that seeds shader storage from the 176 extracted ucode blobs | Small | Removes first-run compile stutter only. XenosRecomp is not involved. |

**Recommendation:** go with A, and order the phases so that 0–2 are cheap,
independent of the renderer, and prove the shaders recompile before anyone
commits to the renderer port in Phase 4. Keep the `xenos` build working
throughout (see the build option in 4.1). That keeps the game playable and
gives a reference renderer for side-by-side comparison.

---

## 4. Plan

### Phase 0: Shader extraction and census

**Goal:** a reproducible, gitignored `assets/shaders/` containing every shader
the game can create.

1. Add `scripts/extract_shaders.py`:
   1. Decrypt and decompress `assets/default.xex`: retail AES key, then the
      basic-compression block list from optional header `0x3FF`.
   2. Scan for `0x102A0E0x`, validating the CTAB header and version as the
      census script did.
   3. Write `vs_<xxh3>.vso` and `ps_<xxh3>.pso`.

   Alternative: reuse the SDK's own XEX loader (`src/system/xex_module.cpp`,
   `lzx.cpp`) from a small C++ tool so no crypto code lives in Python.
2. Add a temporary census hook once `D3DDevice_CreateVertexShader` and
   `D3DDevice_CreatePixelShader` are located (see Phase 3; the blob xrefs find
   them fast). It logs the XXH3 of each container at runtime and dumps any hash
   missing from the static set to `assets/shaders/runtime/`. Play every mode
   and track type once. This rules the DAT archives in or out as a shader
   source.
3. Keep extracted shaders out of the repo, in line with the "no game assets"
   policy. The build consumes `assets/shaders/`.

**Exit:** the static count (176 unique) is reconciled with the runtime
census, and the hash list is checked in as `tools/shader_cache/shader_hashes.csv`
(reblue keeps the same file).

### Phase 1: XenosRecomp fork for rr6

1. Fork `reblue-XenosRecomp` rather than upstream, to get structured control
   flow, the HLSL dump, failure reporting and the cube/PCF fixes. Add it as
   `thirdparty/XenosRecomp` in rr6, along with its `dxc-bin`, `fmt`, `smol-v`,
   `xxHash` and `zstd` submodules.
2. Add an `RR6_RECOMP` define. Leave `REBLUE_RECOMP` off: those paths are Blue
   Dragon's shared-constant layout and vertex decode.
3. **Support container rev 0x0E**, the largest unknown:
   1. Use a versioned header parse in `shader.h`/`shader_recompiler.cpp`:
      `if ((flags >> 8 & 0xFF) == 0x0E)`, then the 6-dword header.
   2. Find the microcode offset, the definition table layout, and the
      interpolator and vertex-element tables for 0x0E. Compare against the
      0x11 layout and Xenia's ucode parsing. Disassemble a few small PS
      shaders (176-byte containers) with the SDK's `translator_disasm.cpp` to
      confirm microcode boundaries.
   3. Relax the scanner predicate in `main.cpp` for 0x0E.
4. Write an rr6 `shader_common.h` section (`#ifdef RR6_RECOMP`):
   - shared-constant layout: booleans, swap masks, half-pixel offset, alpha
     threshold, bindless indices;
   - vertex input locations for the semantics RR6 actually declares, taken
     from the VS vertex-element tables;
   - spec constant bits. Start with `R11G11B10_NORMAL` and `ALPHA_TEST`, and
     add more only when a shader needs one.
5. Run a standalone build and iterate until
   `XenosRecomp assets/shaders out.cpp shader_common.h dump/` reports
   **0 failures** for both DXIL and SPIR-V.

**Exit:** all shaders produce DXIL and SPIR-V, and the HLSL dump has been
reviewed for obviously wrong constant or fetch mapping.

### Phase 2: Build integration (mirror reblue's cmake)

1. Copy `cmake/shader_cache.cmake` and `cmake/shaders.cmake` from reblue,
   renaming `reblue_*` to `rr6_*` and `REBLUE_*` to `RR6_*`. Point
   `INPUT_DIR` at `assets/shaders`, not `assets`, because the XEX is encrypted
   and XenosRecomp's `.xex` glob would find nothing.
2. Add `thirdparty/CMakeLists.txt` with
   `add_subdirectory(XenosRecomp EXCLUDE_FROM_ALL)` and
   `target_compile_definitions(XenosRecomp PRIVATE RR6_RECOMP)`. The xxHash
   fallback can be copied too.
3. Add `vcpkg.json` dependencies: `directx-dxc`, `directx-headers`, `xxhash`,
   `zstd`. Add `find_package(directx-dxc CONFIG REQUIRED)` on WIN32.
4. Add an `rr6_generated` OBJECT library for `${GEN}/shader_cache.cpp`, with
   `add_dependencies(rr6_generated rr6_shader_cache_gen)`. Also add
   `src/gpu/shaders/shader_cache.h` with the exact emitter struct, and change
   the emitter's `#include` under `RR6_RECOMP`.
5. Convert rr6's single-source `CMakeLists.txt` to the reblue layout:
   `src/CMakeLists.txt`, a `rr6_sources()` helper and an `rr6_common` OBJECT
   lib. OBJECT, not STATIC, so `REX_HOOK` symbols survive.

**Exit:** `cmake --build` regenerates `shader_cache.cpp` whenever the
recompiler or `shader_common.h` changes. `rr6_shader_hlsl_dump` works. The exe
links the (still unused) cache.

### Phase 3: Identify XDK 2135 D3D in rr6

The XenosRecomp cache only pays off once the D3D hooks below exist.

1. Find the anchors:
   - **Shader blobs:** VA = `0x82000000` + image offset (the first VS is
     `0x82035A50`). Xrefs to those blobs reach the call sites of
     `D3DDevice_CreateVertexShader` and `D3DDevice_CreatePixelShader`.
   - **Kernel imports:** `VdSwap` leads to `D3DDevice_Swap`.
     `VdInitializeEngines` and `VdInitializeRingBuffer` lead to
     `Direct3D_CreateDevice` and `SetRingBufferParameters`.
     `VdSetGraphicsInterruptCallback` and `VdRetrainEDRAM` mark
     device-creation neighbours.
   - **Shape matching:** compare against reblue's named functions. The XDK
     versions differ, but `Draw*`, `Resolve`, `Set*Shader` and the resource
     lock functions keep recognisable shapes.
2. Export the names with the SDK's `scripts/ida/export_named_funcs.py` into
   `config/functions.toml`, and add it to `rr6_manifest.toml` `includes`.
3. Re-derive the **`D3DDevice` struct layout for 2135** to replace reblue's
   `gpu/d3d.h`: dirty masks, the fetch-constant table, VS/PS float constants,
   bool constants and render-state shadows. Do not assume reblue's offsets
   (`+0x700`, `+0x1700`, alloc size `0x5000`). Read them out of the
   `Direct3D_CreateDevice` allocation and the `SetVertexShaderConstantF`
   inlines.
4. Check which `Set*` calls are inlined in RR6's code. Inlined ones are read
   from the struct at draw time, as reblue does; called ones get hooks.

**Exit:** an rr6 equivalent of reblue's hook list (section 1.3) with every
address named. Compare against `src/gpu/hooks/*.cpp`.

### Phase 4: Native renderer bring-up

1. Add the build option, so the plugin build stays available:
   ```cmake
   option(RR6_NATIVE_GPU "Native plume renderer (OFF = SDK xenos plugin)" OFF)
   ```
   - When ON: compile `src/gpu/**` and its hooks, skip `GPU_PLUGINS xenos`,
     and set `config.graphics = nullptr`.
   - When OFF: build today's setup.

   Hooks are link-time, so this has to be a build switch, not a cvar.
   Optionally build both exes the way reblue builds `reblue` and `reblue_vk`.
2. Add the `plume` submodule (`zolaware/plume`) and its thirdparty settings
   from reblue's `thirdparty/CMakeLists.txt`.
3. Port reblue's `src/gpu` in layers, keeping the BSD-3 headers and
   attribution:
   1. **Portable with light edits:**
      - `device.*`, `frame_ring`, `host_heap`, `host_resource_heap`
      - `bindless*`, `deferred_destroy`, `graveyard`, `device_lost`, `dred`
      - `format`, `byte_swap`, `sampler_cache`, `texture_upload`
      - `surface_registry`, `resolve`, `present`, `output`
      - `pipeline/pipeline_cache`, `pipeline_state`
      - `shaders/{guest_shaders,shader_linker,dxc_link}`
      - `imgui_overlay_drawer`
   2. **Rewrite against the rr6 ABI:**
      - `d3d.h` (the 2135 layout)
      - `constant_buffers.*` (the rr6 `shader_common.h` layout)
      - `vertex_declaration.*` (RR6 semantics and swap masks)
      - `draw*.cpp`
   3. **Leave out** Blue Dragon-specific code:
      - `hooks/tweaks`, `debug_overlay_basis`
      - the `occlusion` sun flare
      - the bloom clamp substitution in `guest_shaders.cpp`
      - the `hcg*` blit shaders
      - `pso_predictor`/`pso_recorder`, until needed
   4. Carry over `src/gpu/shaders/hlsl` host shaders as needed: copy, gamma,
      the MSAA resolves, and the imgui VS/PS.
4. Bring it up in this order:
   1. `CreateDevice`, `Clear` and `Swap` (a coloured screen);
   2. shader creation, with a hash hit rate of 100% logged;
   3. one 2D draw (menus or the logo);
   4. textures;
   5. render targets and `Resolve`;
   6. 3D track geometry;
   7. post-effects.

   Compare every step against the `xenos` build.

### Phase 5: Hardening and performance

- Spec constants: start with a runtime DXC link, then add an
  `rr6_prelink` copy of `tools/prelink_shader_cache.cpp` once the masks
  stabilise.
- Vulkan backend (spec constant id 0 per pipeline), then Linux and macOS
  following reblue's CMake.
- PSO precache at boot. With 176 shaders the PSO space is small, so precaching
  every observed pipeline is realistic.
- Port reblue's `Settings` for resolution, MSAA and aspect.

---

## 5. Risks and open questions

1. **Container rev 0x0E**, the biggest XenosRecomp change: the microcode
   offset and the definition and interpolator tables are not yet mapped. Mitigation:
   the shaders are small, and Xenia's ucode disassembler confirms boundaries.
2. **XDK 2135 D3D differs from Blue Dragon's XDK.** The struct offsets,
   inlining and possibly function signatures all differ. The `D3DDevice_Clear`
   r8-slot quirk is an example of an ABI detail that may or may not recur.
3. **Shaders hidden in the DAT archives.** Static analysis found none.
   Phase 0's runtime census decides.
4. **Size of the renderer port.** It is the real cost. Phases 0–2 are worth
   doing first because they de-risk the shader half and leave the `xenos`
   build untouched.
5. **XenosRecomp's unimplemented features** (memexport, point size, dynamic
   register indexing, mini vertex fetch): check the Phase 1 HLSL dump and the
   failure list for any rr6 shaders that hit them.
