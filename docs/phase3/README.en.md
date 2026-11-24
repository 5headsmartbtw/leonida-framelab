# Phase 3 — FrameInputs and Leonida integration research

[Русский](README.md)

The original Phase 3 branch `experiment/phase3-frame-inputs` preserved the validated sample baseline at `a175d35f01a7c172429f79af27b6c6d43704b6b9`. The source-only Leonida audit targeted `8575d36edc479218972497a7bded8651a42856c0`. Phase 3 changes were made to the independent sample. The original checkout and hashes are recorded in [DELIVERY_MANIFEST.json](DELIVERY_MANIFEST.json).

- [FrameInputs contract](CONTRACT.en.md) covers input meaning, image state, completion and lifetime for a single source frame.
- [Vulkan pipeline audit](LEONIDA_AUDIT.en.md) records the available renderer infrastructure and the semantic inputs that still need evidence.
- [Validation](VALIDATION.en.md) records the checks performed and their hardware, runtime and test limits.
- [Integration plan](INTEGRATION_PLAN.en.md) lists the gates before a Leonida backend change.

Phase 3 proved the standalone sample on its RTX 2050 profile. It exercised native Vulkan 2X FG, VSync, resize, missing-depth fallback, and ownership through completion. The preserved EXE and reference image are local validation artifacts. See the current [build guide](../BUILD.md) to reproduce without that historical directory layout.
