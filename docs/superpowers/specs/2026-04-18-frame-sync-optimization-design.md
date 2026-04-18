---
name: frame-sync-optimization
description: Move timeline wait to beginFrame for CPU/GPU overlap, enforce MAILBOX/IMMEDIATE present mode
type: project
---

# Frame Synchronization Optimization Design

## Problem Statement

Current frame synchronization blocks CPU on every frame via `waitForFrameCompletion()` called in `Renderer::endFrame()` after submit but before present. This prevents CPU/GPU overlap and creates VSync lock in FIFO mode.

## Current Architecture

### Frame Flow (Blocking)
```
beginFrame: reset pool, begin cmd
recordFrame: record passes
endFrame: 
  submit → advanceToNextFrame → waitForFrameCompletion (BLOCKING) → Present
```

### Key Components
- `VulkanFrameContext`: Timeline semaphore for frame completion signaling
- `VulkanSwapchain`: Binary semaphores for acquire/present, MAILBOX/IMMEDIATE support
- `Renderer::endFrame()`: Contains blocking wait at line 2429

### Timeline Semaphore Flow
- Each submit signals `timelineValue = m_frameCounter++`
- `waitForFrameCompletion()` waits on `m_frames[m_currentFrameIndex].lastSignalValue`
- Frame slots: 3+ frames (tied to `m_requestedImageCount`)

---

## Proposed Architecture

### Frame Flow (Non-Blocking)
```
beginFrame:
  advanceToNextFrame → waitForFrameCompletion (wait for oldest slot) → reset pool, begin cmd
  
recordFrame: record passes

endFrame:
  submit → Present (no blocking)
```

### Why: This enables CPU/GPU overlap
- GPU executes frame N
- CPU simultaneously records frame N+1
- Wait only happens when we try to reuse oldest frame slot (N-3)

---

## Implementation Changes

### 1. Renderer.cpp

**Renderer::beginFrame()** - Add wait at start:
```cpp
void Renderer::beginFrame(rhi::CommandList& cmd, const RenderParams& params) {
  // NEW: Advance to next frame slot and wait for it to be GPU-complete
  m_perFrame.frameContext->advanceToNextFrame();
  m_perFrame.frameContext->waitForFrameCompletion();
  
  // Existing: begin command buffer recording
  m_perFrame.frameContext->beginFrame();
  ...
}
```

**Renderer::endFrame()** - Remove blocking calls:
```cpp
void Renderer::endFrame(rhi::CommandList& cmd) {
  submitFrame(*m_perFrame.frameContext, cmd);
  // REMOVED: advanceToNextFrame() - moved to beginFrame
  // REMOVED: waitForFrameCompletion() - moved to beginFrame
  presentFrame(*m_swapchainDependent.swapchain);
  m_perFrame.frameCounter++;
}
```

### 2. VulkanFrameContext.cpp

No changes needed. `waitForFrameCompletion()` already waits on the correct frame slot's timeline value.

### 3. VulkanSwapchain.cpp

No changes needed. Present mode selection already prefers MAILBOX/IMMEDIATE with FIFO fallback.

---

## Present Mode Policy

**Policy**: Prefer MAILBOX/IMMEDIATE, fallback to FIFO if unsupported.

Current implementation (`selectSwapPresentMode`) is correct:
1. If vSync=false: try MAILBOX → try IMMEDIATE → fallback FIFO
2. If vSync=true: use FIFO

No changes required.

---

## Ring Buffer Resource Management

### Current Implementation (Correct)
- `m_frames.resize(frameCount)` - frame slots matching swapchain image count
- `m_frameResources.resize(m_requestedImageCount)` - per-slot acquire semaphores
- `DeferredDestructionQueue` - timeline-based resource retirement

### Timeline-Based Retirement Flow
1. `enqueueRetirement(resource, timelineValue)` - schedule destruction
2. `processRetirements(currentTimelineValue)` - called after `waitForFrameCompletion`
3. Resources freed only when GPU confirms completion via timeline semaphore

---

## Testing Strategy

1. **Visual Test**: Run application, verify no tearing or stuttering
2. **Performance Test**: Measure frame time variance - should see reduced CPU wait time
3. **Edge Cases**: 
   - Swapchain resize (rebuild) - verify frame counters reset correctly
   - MAILBOX unsupported - verify FIFO fallback still works

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Frame slot reused before GPU complete | Timeline wait guarantees completion |
| MAILBOX not supported | FIFO fallback (but with blocking) |
| Swapchain rebuild race | `vkQueueWaitIdle` in rebuild (existing) |

---

## Success Criteria

1. CPU no longer blocks on `waitForFrameCompletion()` before present
2. Frame recording overlaps with previous frame's GPU execution
3. MAILBOX/IMMEDIATE mode used when available (vSync=false)
4. No visual artifacts (tearing, stuttering)