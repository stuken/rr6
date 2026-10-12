// rr6 - ReXGlue Recompiled Project

#pragma once

namespace rr6::gpu {

struct VideoState;
struct GuestTexture;

// Called once per presented frame with the frame's source render target, after
// its command list was submitted. Dumps it when rr6_dump_frames names this
// frame. Caller holds s.mutex.
void MaybeDumpFrame(VideoState &s, GuestTexture *rt);

} // namespace rr6::gpu
