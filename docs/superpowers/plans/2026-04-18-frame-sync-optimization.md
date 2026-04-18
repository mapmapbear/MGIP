# Frame Synchronization Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move timeline wait from endFrame to prepareFrameResources, enabling CPU/GPU overlap where CPU records frame N+1 while GPU executes frame N.

**Architecture:** Frame synchronization currently blocks CPU at end of each frame. By moving `advanceToNextFrame()` and `waitForFrameCompletion()` to the start of `prepareFrameResources()`, the wait happens before recording the next frame, allowing GPU to continue executing the previous frame while CPU records commands.

**Tech Stack:** Vulkan RHI, Timeline Semaphores, C++17, CMake

---

## Files Modified

| File | Change |
|------|--------|
| `render/Renderer.cpp:1180-1186` | Add wait before `beginFrame()` |
| `render/Renderer.cpp:2426-2429` | Remove wait after submit |

---

### Task 1: Move Frame Wait to prepareFrameResources

**Files:**
- Modify: `render/Renderer.cpp:1180-1186`

- [ ] **Step 1: Add advanceToNextFrame and waitForFrameCompletion at start of prepareFrameResources**

Current code at line 1184-1185:
```cpp
  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");
  m_perFrame.frameContext->beginFrame();
```

Replace with:
```cpp
  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");

  // Advance to next frame slot and wait for it to be GPU-complete
  // This enables CPU/GPU overlap: while GPU executes frame N, CPU records frame N+1
  m_perFrame.frameContext->advanceToNextFrame();
  m_perFrame.frameContext->waitForFrameCompletion();

  m_perFrame.frameContext->beginFrame();
```

- [ ] **Step 2: Verify the edit is syntactically correct**

Read the modified section to confirm correct formatting.

---

### Task 2: Remove Blocking Wait from endFrame

**Files:**
- Modify: `render/Renderer.cpp:2426-2429`

- [ ] **Step 1: Remove advanceToNextFrame and waitForFrameCompletion from endFrame**

Current code at lines 2419-2433:
```cpp
void Renderer::endFrame(rhi::CommandList& cmd)
{
  (void)cmd;
  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");

  submitFrame(*m_perFrame.frameContext, cmd);

  // Advance to next frame slot and wait for it to be ready
  // This allows CPU to overlap recording of next frame with GPU execution of current frame
  m_perFrame.frameContext->advanceToNextFrame();
  m_perFrame.frameContext->waitForFrameCompletion();

  presentFrame(*m_swapchainDependent.swapchain);
  m_perFrame.frameCounter++;
}
```

Replace with:
```cpp
void Renderer::endFrame(rhi::CommandList& cmd)
{
  (void)cmd;
  ASSERT(m_perFrame.frameContext != nullptr, "Per-frame FrameContext must be initialized");

  submitFrame(*m_perFrame.frameContext, cmd);

  // Frame advancement and wait moved to prepareFrameResources for CPU/GPU overlap
  // GPU executes frame N while CPU records frame N+1

  presentFrame(*m_swapchainDependent.swapchain);
  m_perFrame.frameCounter++;
}
```

- [ ] **Step 2: Verify the edit is syntactically correct**

Read the modified section to confirm correct formatting.

---

### Task 3: Build and Verify

- [ ] **Step 1: Build the project**

Run: `cmake --build out/build/x64-Debug --target MinimalLatestApp`
Expected: Build succeeds with no errors

- [ ] **Step 2: Run the application briefly**

Run: `./out/build/x64-Debug/app/MinimalLatestApp.exe`
Expected: Application runs without crashes, window displays correctly

- [ ] **Step 3: Commit the changes**

```bash
git add render/Renderer.cpp
git commit -m "$(cat <<'EOF'
perf(frame): move timeline wait to beginFrame for CPU/GPU overlap

- Move advanceToNextFrame() and waitForFrameCompletion() from
  endFrame() to prepareFrameResources()
- CPU now records frame N+1 while GPU executes frame N
- Eliminates blocking wait before present

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review Checklist

| Check | Status |
|-------|--------|
| Spec coverage: wait moved to beginFrame | Covered in Task 1 |
| Spec coverage: wait removed from endFrame | Covered in Task 2 |
| Placeholder scan: no TBD/TODO | Verified |
| Type consistency: FrameContext methods unchanged | Verified |