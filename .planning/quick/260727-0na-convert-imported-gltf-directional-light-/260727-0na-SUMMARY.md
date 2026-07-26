---
status: complete
quick_id: 260727-0na
completed: 2026-07-27
description: Convert imported glTF directional light lux to Blender-compatible unitless intensity
---

# Summary: Convert glTF Directional Light Intensity

## Outcome

Imported glTF directional-light intensity now converts from lux to the renderer's
Blender-compatible convention, so Sponza's `34150` value resolves to `50`.

## Changes

- Added a directional-only `683.0` compatibility conversion.
- Applied it to both TinyGLTF light-definition parsing paths.
- Bumped SceneCache and SceneAsset versions to invalidate pre-conversion values.
- Added import, point-light isolation, and cache round-trip regression coverage.

## Verification

- The new regression test failed before implementation at the expected assertion.
- `.\build_debug_with_vsdevcmd.cmd` passed.
- `ctest --test-dir out\build\x64-Debug -C Debug --output-on-failure` passed 3/3 tests.
- `git diff --check` passed with only line-ending warnings.
