// rr6 - ReXGlue Recompiled Project
//
// reblue precompiles a pipeline cache recorded from Blue Dragon sessions and
// captures the misses for the next one. RR6 has no recorded cache yet, so this
// only reports first-encounter compiles on the render thread, which is the
// list a future rr6 cache has to cover.

#include "gpu/pipeline/pso_recorder.h"

#include <mutex>
#include <unordered_set>

#include "core/logging.h"

namespace rr6::gpu {

void RecordPipelineState(const PipelineState &state, u32 renderPassId,
                         bool builtOnRenderThread) {
  (void)state;
  (void)renderPassId;
  if (!builtOnRenderThread)
    return;
  static std::atomic<u32> s_misses{0};
  const u32 n = s_misses.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n % 256) == 0)
    RR6_DEV_INFO("PSO: {} pipeline(s) compiled on the render thread", n);
}

void ReplayBootCache() {}
void FlushPSOCapture() {}
void OnShaderCreated(u64) {}
void OnVertexDeclarationCreated(u64) {}

} // namespace rr6::gpu
