---
quick_id: 260613-pg1
status: complete
date: 2026-06-13
---

# Summary

Added UI and runtime wiring to load a mesh SDF asset from a glTF-derived path and enable DDGI.

## Changes

- Added `RendererFacade` / `GPUDrivenRenderer` APIs for loading a DDGI mesh SDF.
- Added on-demand DDGI resource initialization so DDGI can be enabled after renderer startup.
- Added Model Loader UI controls for DDGI enable, mesh SDF path, path derivation, and loading.
- Path derivation prefers `sponza.gltf -> sponza_sdf.gltf` and falls back to `_SDF.bin` / `_sdf.bin` when the preferred file does not exist.

## Verification

- Passed: `cmd.exe /d /s /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build H:\GitHub\VKDemo\out\build\x64-Debug --target Demo --config Debug'`
- Build included the RHI boundary guard: backend include, Vulkan token, and native getter checks passed.

## Notes

The SDF loader validates file contents, not the extension, so a binary SDF payload can be named `sponza_sdf.gltf` if desired.
