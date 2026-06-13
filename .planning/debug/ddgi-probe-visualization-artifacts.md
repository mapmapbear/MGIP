---
status: resolved
trigger: "DDGI probe visualization appears to have no depth testing and shows strange stripes."
created: 2026-06-13
updated: 2026-06-13
---

# Debug Session: DDGI Probe Visualization Artifacts

## Symptoms

- Expected: Probe debug spheres should occlude correctly against scene depth and display readable probe state.
- Actual: Probe spheres looked like they ignored depth and showed distracting stripe patterns.

## Evidence

- 2026-06-13: `DDGIDebugPass` enabled scene-depth testing, but drew procedural spheres two-sided with depth writes disabled.
- 2026-06-13: Two-sided sphere rendering lets back-facing fragments pass against the scene depth and overdraw front-facing fragments because the sphere does not write depth for itself.
- 2026-06-13: `ddgi_probe_visualization.slang` mapped the low-resolution per-probe octahedral irradiance atlas directly onto the sphere normal, magnifying 8x8 directional texels into visible bands.

## Resolution

- root_cause: Visualization artifacts came from debug draw presentation, not DDGI probe computation. Spheres were double-sided without self-depth, and the shader displayed raw low-resolution directional atlas texels.
- fix: Enabled back-face culling for probe spheres and changed the fragment visualization to show mostly averaged probe irradiance with a small directional tint.
- verification: `cmake --build H:\GitHub\VKDemo\out\build\x64-Debug --target Demo --config Debug` passed; RHI boundary guard passed and `ddgi_probe_visualization.slang` regenerated successfully.
- files_changed: render/passes/DDGIDebugPass.cpp, shaders/ddgi_probe_visualization.slang
