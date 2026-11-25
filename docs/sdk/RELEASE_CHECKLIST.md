# First experimental SDK release gates

The current release scope is incremental. The package and its ABI are tested separately from GPU hardware.

| Gate | Evidence or remaining work |
| --- | --- |
| Independent C and C++ consumers | Both compile with the installed include directory and link through `FrameLab::FrameLab`. The C test exercises an accepted packet through preparation, submission acknowledgement, retirement and final release. |
| Static and shared installs | Exercise both package variants on Windows and Linux in [SDK package CI](../../.github/workflows/sdk-package.yml). |
| Versioned ABI, lifecycle and safety | ABI v1 version and descriptor size checks. The [public contract](README.md) documents caller-side ordering, real completion and callback obligations. |
| Independent hardware-backed FG and SR | [D3D12 FG](D3D12_FG.md) runs on the reference profile through the installed SDK. Vulkan FG and [SR-only](../evidence/sr-only-rtx2050.md) pass in the sample. Public Vulkan FG and independent SR dispatch remain open. |
| Vulkan and D3D12 consumers | The [D3D12 reference](../../experiments/d3d12/FRAME_INPUTS.md) recorded a verified workload on a native queue. The [Vulkan handoff test](../../tests/vulkan-handoff/README.md) covered separate queues and family ownership. The [public D3D12 component](D3D12_FG.md) checks the configured native completion fence. Wiring external producer completion into Vulkan FG remains open. |
| Rust FFI | [Raw Rust bindings](../../bindings/rust/README.md) pass layout comparison against the C header and shared-library lifecycle tests on Windows and Linux. |
| Combined SR + FG | Require on-device readback, validation, resize/reset and fallback on a narrowly specified combined profile before declaring support. |
| Tests and publication | Keep original FG regressions and portable CI passing. Record tested hardware, commands, failed/skipped checks and third-party source attribution. Keep proprietary binaries and game files outside the package. |

Leonida and VIctory integration proposals are optional consumers. Neither is a release build dependency.

## GPU compatibility and memory

Preserve previously verified RTX 20- and 30-series-compatible functionality on the documented device, driver and runtime combinations. The RTX 5090 is a development and stress-test platform. The RTX 2050 Laptop GPU with 4 GB VRAM remains the low-memory reference. Query native hardware and runtime capabilities before enabling optional features, including features specific to Blackwell, and retain ordinary rendering when a feature is unsupported or resources cannot be allocated. Record official NVIDIA support and tested experimental compatibility profiles separately. Make compatibility claims only for configurations supported by evidence. Preserve the installed public ABI and its versioned descriptors and verify existing consumers.

Keep per-frame allocations and the number of in-flight resources bounded. Avoid unnecessary allocations and extra full-resolution copies. Measure peak and steady-state VRAM usage and the available memory budget on the tested adapter, with the same accounting source and sampling method for each revision. Exercise allocation failure or budget exhaustion and verify safe rejection or fallback, valid resource ownership and cleanup.

## RTX 2050 low-memory merge gate

Before merging GPU implementation PRs that affect rendering, SDK dispatch, memory, scheduling or synchronization, a maintainer runs and records the regression check on the RTX 2050 Laptop (4 GB). Contributors develop and validate on their available hardware. Higher-resolution RTX 5090 and cross-device runs add coverage alongside the reference check.

- Compare the last working revision and the exact proposed revision at **1280×720 output**. Use equivalent input resolution and quality mode, the same synthetic scene and camera trajectory, driver and runtime profile, and matching queue, display/VSync and run settings where applicable. Record both commits and exact commands.
- Record source FPS and source-frame intervals, GPU execution time, peak and steady-state VRAM, and available memory budget. Record native evaluation and verified readback, actual completion and resource retirement, and validation diagnostics. Run comparable performance captures separately from debug validation and readback. Keep source FPS separate from generated frames and Present counts.
- Repeat baseline runs under the same conditions, record the measured spread, and set and document numerical tolerances from that variability before judging proposed results. Apply the same sampling and warm-up rules to both revisions. Investigate changes outside the tolerances and hold the merge while a performance or memory regression is unexplained.
- Compare the modes and features the baseline already supported. For a new optional feature with no prior equivalent, compare the existing common paths and ordinary-rendering fallback and report the new mode's absolute costs and observed capability separately. Keep required native evaluation and readback in supported paths. Record unsupported cases and reasons. Keep official and experimental runtime results separate, and compare each runtime profile with its matching baseline.

## Synchronization and lifetime stress

Exercise applicable scenarios using controlled queue/fence gates, timeline values or submission fault injection. Use deterministic controls where possible. Cover delayed GPU completion and fence/timeline signaling, multiple frames in flight, producer/consumer imbalance, premature resource reuse attempts, resize while GPU work is pending, resource exhaustion, rejected submissions and cancellation, and device loss with safe teardown. For APIs that deliberately limit in-flight work, verify the busy or rejection contract and retention until completion.

Base reuse, retirement and lease release on verified native GPU completion. Cancel only after establishing that a recording cannot execute. Exercise and label simulated loss separately from observed native device loss, and verify the documented safe teardown path. Keep Vulkan synchronization validation and applicable D3D12 debug-layer checks enabled in correctness runs, including GPU-based validation where supported and appropriate. The separate native [NGX SyncVal release gate](https://github.com/5headsmartbtw/leonida-framelab/issues/24) remains open.
