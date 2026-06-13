---
quick_id: 260613-rti
status: complete
date: 2026-06-13
---

# Summary

Added a minimal colored-bounce path for DDGI Mesh SDF.

## Changes

- Extended Mesh SDF assets to v2 with an RGBA8 albedo payload after the R16F distance field.
- `sdf_baker` writes nearest-triangle material color using glTF primitive `baseColorFactor`; OBJ defaults to white.
- `SDFLoader` remains compatible with v1 assets and uploads v2 albedo as a 3D texture.
- `GlobalSDFPass` composes a global albedo volume alongside the global SDF.
- `DDGIRayTracePass` binds the global albedo volume, and `ddgi_gi_sdf_rays.slang` uses hit albedo instead of scalar constant albedo.

## Verification

- Passed: `cmake --build H:\GitHub\VKDemo\out\build\x64-Debug --target sdf_baker --config Debug`
- Passed: `cmake --build H:\GitHub\VKDemo\out\build\x64-Debug --target Demo --config Debug`
- Passed: `out\build\x64-Debug\sdf_baker.exe resources\Sponza\Sponza.gltf out\build\x64-Debug\Resources\Sponza\Sponza_sdf.gltf`

## Notes

- This is still not a full Surface Cache: only material baseColor factors are baked, not baseColor textures.
- Existing v1 SDF assets load as white albedo, so they will not show colored bounce until rebaked.
