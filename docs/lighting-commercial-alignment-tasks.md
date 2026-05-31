# Lighting Commercial Alignment Task Matrix

Scope note: keep the current metallic-roughness BRDF as the active material model. New material lobes such as clearcoat, sheen, transmission, anisotropy, specular color, and IOR are tracked here but marked out of scope because the current product direction is "do not expand BRDF materials."

Status legend:

- Done: implemented in the current code changes.
- Partial: implemented enough to improve the result, but still not reference-engine complete.
- Pending: should be implemented later.
- Out of scope: explicitly not part of the current renderer target.

## IBL Generation Quality

| Task | Status | Notes |
| --- | --- | --- |
| Load real HDR environment input | Done | Runtime loads `resources/Env/urban_street_04_4k.ktx2` as BC6H/BC6U block data. |
| Generate irradiance cubemap | Done | Startup compute convolution writes a cube-compatible irradiance map. |
| Generate GGX prefiltered cubemap | Done | Startup compute path writes roughness mip chain. |
| Generate BRDF/DFG LUT | Done | Startup compute path writes split-sum DFG LUT. |
| GGX source mip selection by PDF and source texel solid angle | Done | Prefilter shader now estimates source mip from GGX PDF and HDR source solid angle. |
| Strict irradiance source solid-angle weighting | Partial | Current convolution samples the hemisphere; it is good enough for runtime, but not a fully reference-validated offline convolution. |
| Validate IBL outputs against a reference renderer | Pending | Needs image captures against Filament/Unreal with identical HDR, model, camera, and exposure. |
| Runtime debug views for irradiance/prefilter/DFG | Done | Settings panel exposes irradiance faces, selectable prefilter mip faces, and DFG LUT through ImGui descriptors. |

## Exposure, Color, And Display Transform

| Task | Status | Notes |
| --- | --- | --- |
| Shared fixed exposure for LightPass and SkyPass | Done | `LightParams::iblControls.x` drives both passes. |
| Separate IBL and sky intensity controls | Done | `iblControls.y` and `iblControls.z`. |
| Runtime exposure/IBL/sky/specular occlusion controls | Done | Settings panel exposes fixed exposure, IBL intensity, sky intensity, and specular occlusion strength. |
| Scene-linear lighting workflow | Partial | Shading is scene-linear before ACES; raw glTF baseColor/emissive textures now sample through sRGB image formats, while KTX2 sidecars still rely on their encoded VkFormat. |
| Auto exposure | Pending | Needs luminance histogram or average luminance pass plus temporal adaptation. |
| White point control | Pending | Needed for more predictable ACES/display matching. |
| Color grading pipeline | Pending | Needs LUT or grading controls after tone mapping policy is settled. |
| Reference display calibration | Pending | Needs known camera/exposure values and screenshot comparison. |

## Existing Metallic-Roughness BRDF

| Task | Status | Notes |
| --- | --- | --- |
| GGX direct lighting | Done | Existing direct PBR path remains active. |
| Split-sum specular IBL | Done | Prefiltered cubemap + DFG LUT are sampled in LightPass. |
| Lambert diffuse IBL | Done | Irradiance map contributes diffuse IBL. |
| Specular occlusion from AO | Done | Uses common AO/NdotV/roughness approximation. |
| Burley diffuse | Out of scope | This changes the diffuse BRDF model; excluded by current "no BRDF expansion" direction. |
| Diffuse/specular energy compensation beyond DFG | Out of scope | Track only if the current BRDF model is reopened. |
| Clearcoat | Out of scope | Explicit material lobe expansion. |
| Sheen | Out of scope | Explicit material lobe expansion. |
| Transmission | Out of scope | Explicit material lobe expansion. |
| Anisotropy | Out of scope | Explicit material lobe expansion. |
| Specular color / IOR material controls | Out of scope | Explicit material parameter expansion. |

## AO, Shadows, And Indirect Lighting

| Task | Status | Notes |
| --- | --- | --- |
| Diffuse AO applied to IBL | Done | Existing AO attenuates diffuse IBL. |
| Specular occlusion applied to IBL | Done | Implemented in LightPass. |
| Bent normal AO | Pending | Requires bent normal data in GBuffer or a separate AO pass output. |
| Multi-bounce AO approximation | Pending | Can be added without new material lobes, but needs a chosen approximation and tuning. |
| CSM direct shadowing | Done | Existing cascaded shadow path remains active. |
| GI | Pending | No probe GI, DDGI, SSGI, or baked GI system exists yet. |
| Reflection probe blending | Pending | Needs multiple probes, influence volumes, and blend policy. |
| Local reflection capture | Pending | Needs capture/update path and per-object/probe selection. |
| SSR | Pending | Requires screen-space ray tracing and resolve. |
| SSGI | Pending | Requires screen-space GI pass and temporal/spatial filtering. |

## Sky And Probe Consistency

| Task | Status | Notes |
| --- | --- | --- |
| GPU-driven SkyPass | Done | Sky is a render graph pass in the GPU-driven path. |
| SkyPass uses generated cubemap/probe path | Done | Sky samples generated `prefilteredMap` mip 0. |
| Shared exposure/intensity between sky and scene | Done | Both use `LightParams::iblControls`. |
| Full sky cubemap separate from prefilter probe | Pending | Current sky uses prefilter mip 0; a dedicated sky cubemap can preserve higher visual resolution. |
| Probe orientation validation | Pending | Needs reference captures for all cube faces and sky horizon. |

## Units And Calibration

| Task | Status | Notes |
| --- | --- | --- |
| Directional light physical units | Pending | Current values are artistic RGB intensity, not lux/EV-calibrated. |
| Point light physical units | Pending | Current falloff/intensity is not calibrated to lumens/candela. |
| HDR environment intensity calibration | Partial | IBL intensity exists, but no physical reference scale is established. |
| Camera exposure model | Partial | Fixed exposure exists; EV100/aperture/shutter/ISO model does not. |
| Material color-space audit | Partial | Raw glTF baseColor/emissive upload is corrected to sRGB sampling when the image is not also used as a data map; compressed/KTX2 asset formats and output/display calibration still need reference validation. |
| Golden reference scene | Pending | Needed before saying output matches a commercial engine. |

## Current Execution Queue

1. Compile and fix integration errors from the current IBL/Sky/lighting changes.
2. Add a golden reference scene and capture comparison workflow.
3. Decide whether to implement auto exposure next or physical-unit calibration first.
4. Audit compressed/KTX2 texture formats and output/display calibration against the scene-linear lighting path.
