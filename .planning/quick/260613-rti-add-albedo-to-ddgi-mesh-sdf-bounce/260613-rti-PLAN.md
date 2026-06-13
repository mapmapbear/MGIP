---
quick_id: 260613-rti
status: in_progress
date: 2026-06-13
---

# Plan

Add a minimal albedo path to the DDGI Mesh SDF pipeline so probe rays can pick up material color.

## Steps

- Extend the Mesh SDF baker/loader asset format with optional voxel albedo data.
- Upload mesh albedo as a 3D texture and compose a matching global albedo volume.
- Sample global albedo in DDGI SDF ray hit shading instead of using a scalar constant albedo.
- Build `sdf_baker` and `Demo`, then document verification.
