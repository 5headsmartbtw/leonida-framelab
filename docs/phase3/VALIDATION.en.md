# Validation evidence

[Русский и исходные команды](VALIDATION.md) · [Raw text evidence](evidence) · [Current build and GPU procedure](../BUILD.md)

The Phase 3 hardware was a 4 GB RTX 2050 Laptop (SM 8.6) with Windows 11 build 26200, NVIDIA driver 617.14 and MSVC 2022. The profile used Streamline 2.14.1, DLSS SDK v310.9.1, SM86 0.4.2 with Bundled runtime 310.9.16, and the Windows cubin layer. Independent hardware replication remains open.

## CPU contract checks

Both CTest targets passed. The API-neutral target has 43 checks and the Vulkan-header target has 16 checks. Together they cover missing input data, non-finite and stale metadata, reset/generation consistency, lifetime and owner changes, invalid device/queue/command buffer, native image state/extent and unsupported profiles. The Vulkan adapter unit tests do not require a GPU.

## Phase 3 GPU run

| Scenario | Real Present | NGX Evaluate | Verified and presented generated frames | Input rejections | Validation errors/warnings |
| --- | ---: | ---: | ---: | ---: | --- |
| VSync Off / Immediate | 6 | 6 | 5 | 0 | 0/0 |
| VSync On / FIFO | 6 | 6 | 5 | 0 | 0/0 |
| Missing depth at source frame 2 | 6 | 5 | 3 | 1 expected | 0/0 |

Frame 2 was rejected before NGX and frame 3 reset history. The retained packet count reached zero after completion in each run. The first reset frame counts as an evaluation without counting as an interpolation.

The deterministic RGBA16F generated image matched the original baseline. SHA-256 `caa4a0256f6920a8fc596fc414680d9544b72abff5c7f6e3c148a6d6924d96a2`.

The live record also covers Immediate ↔ FIFO, resizing from 1280×720 to 1920×1009 and disabling/re-enabling FG. It logged 10,605 source frames, 7,380 Evaluate calls, 10,605 real Present calls and 7,375 generated Present calls. FG errors, rejected inputs, remaining leases, and validation errors/warnings were zero. Live readback per frame was disabled. The bounded tests above verify image content.

These results cover the synthetic sample and this hardware profile. Real display cadence and latency, HDR, combined SR + FG, arbitrary queue transfers, cross-device compatibility, device-loss recovery and long memory pressure remain unverified.

## Configurable runtime rerun

On the same RTX 2050, a fresh CMake build on C: with spaces in its output path used separately configured SDKs. Both CPU targets passed. The new runner passed all three GPU cases with and without the optional local baseline file. The VSync Off result matched the preserved hash above. See the [sanitized result JSON](../evidence/runtime-config-rtx2050.json).

A separate live run recreated the FG feature after resize from 1280×720 to 1008×601. It completed 300 real Present calls, 300 evaluations and 297 generated Present calls with zero validation errors or warnings and zero remaining leases. Image readback was disabled in that live run.

The separate Streamline SR check completed 120 successful evaluations, but the Vulkan validation run failed with 26 errors. Reported errors covered device extension/feature setup, dynamic rendering attachments and reuse of a presentation semaphore. Fix the SR validation baseline before testing a combined SR + FG path.
