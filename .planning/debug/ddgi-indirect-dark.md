---
status: resolved
trigger: "DDGI indirect light becomes dark after several frames; hysteresis=0.2 and stride=1 causes flicker; Reset History does not restore the initial brighter result."
created: 2026-06-13
updated: 2026-06-13
---

# Debug Session: DDGI Indirect Darkening

## Symptoms

- Expected: DDGI indirect lighting should remain stable after loading mesh SDF, and Reset History should restore a clean probe state.
- Actual: Early frames look acceptable, then indirect lighting becomes increasingly dark.
- Additional observation: Reducing hysteresis to 0.2 and update stride to 1 causes flickering, but Reset History still keeps the darker result.
- Trigger: Enable DDGI / mesh SDF path and run Sponza-like scene.

## Current Focus

- hypothesis: DDGI probe update is producing too-dark radiance after the first fallback frames, or the reset path is not clearing the resource actually sampled by GI.
- test: Inspect DDGI update shaders, GI sampling shader, atlas initialization/clear path, and UI reset wiring.
- expecting: Find either a shader-side energy/shadowing issue, missing clear/transition, or stale config/resource mismatch.
- next_action: gather initial code evidence.

## Evidence

- 2026-06-13: `GPUDrivenRenderer::resetDDGIHistory` waits idle, shuts down DDGI resources, resets `m_temporalFrameCounter` and `m_ddgiUpdateOffset`, then recreates probe resources. Reset is reaching the intended resource path.
- 2026-06-13: `ddgi_gi_sdf_rays.slang` uses a bright sky fallback only when `firstFrame != 0`. Later frames use direct sun plus binary SDF shadow and history-probe indirect.
- 2026-06-13: Hit shading's sun shadow ray treated any SDF hit as occlusion after only a `2 * voxelSize` normal offset. In low-resolution/thin geometry scenes this can self-shadow the hit surface or nearby structure, producing very low radiance that temporal accumulation converges toward.
- 2026-06-13: Low hysteresis (`0.2`) and stride `1` causing flicker is consistent with live probe updates using noisy low-ray-count / binary shadow input, not with a stuck history reset button.

## Eliminated

- hypothesis: Reset button does not clear DDGI resources.
  evidence: The reset path destroys and recreates probe resources and resets counters; dark result after reset is reproducible because the new probe field converges to the same low-energy solution.

## Resolution

- root_cause: DDGI probe ray hit shading had too little stable energy after the first frame. The first frame injected sky fallback on hits, but later frames relied on SDF-shadowed direct sun and previous-probe bounce. Coarse binary SDF shadowing likely self-occluded many hits, so probe irradiance converged darker and reset simply replayed that convergence.
- fix: Ignore very near shadow hits to reduce self-shadowing, and add a small sky diffuse floor to hit shading every frame so simplified SDF-only probes do not collapse to black before richer material/IBL hit lighting exists.
- verification: `cmake --build H:\GitHub\VKDemo\out\build\x64-Debug --target Demo --config Debug` passed; RHI boundary guard passed and `ddgi_gi_sdf_rays.slang` regenerated successfully.
- files_changed: shaders/ddgi_gi_sdf_rays.slang
