---
quick_id: 260613-p7e
status: complete
date: 2026-06-13
---

# Summary

Fixed `sdf_baker` glTF loading for textured scenes by registering a no-op tinygltf image loader. The baker now accepts image references while discarding pixel data, so geometry-only SDF baking does not require stb image decoding.

## Verification

- Passed: `cmd.exe /d /s /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build H:\GitHub\VKDemo\out\build\x64-Debug --target sdf_baker --config Debug'`
- Passed: `.\sdf_baker.exe resources/Sponza/Sponza.gltf Resources/Sponza/Sponza_SDF_test.bin --min-res 4 --max-res 4 --samples 1`

## Result

The reported Sponza glTF path loads successfully:

- Vertices: 192496
- Triangles: 262267
