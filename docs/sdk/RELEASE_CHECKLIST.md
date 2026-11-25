# First experimental SDK release gates

The current release scope is incremental. The package and its ABI are tested separately from GPU hardware.

| Gate | Evidence or remaining work |
| --- | --- |
| Independent C and C++ consumers | Both compile with the installed include directory and link through `FrameLab::FrameLab`. The C test exercises an accepted packet through preparation, submission acknowledgement, retirement and final release. |
| Static and shared installs | Exercise both package variants on Windows and Linux in [SDK package CI](../../.github/workflows/sdk-package.yml). |
| Versioned ABI, lifecycle and safety | ABI v1 version and descriptor size checks. The [public contract](README.md) documents caller-side ordering, real completion and callback obligations. |
| Independent hardware-backed FG and SR | [D3D12 FG](D3D12_FG.md) runs on the reference profile through the installed SDK. The [installed Vulkan SR](VULKAN_SR.md) and Vulkan FG in [#20](https://github.com/5headsmartbtw/leonida-framelab/pull/20) have reference-GPU readback. The independent RTX 5090 test reproduced FG output and SyncVal diagnostics in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24). |
| Vulkan and D3D12 consumers | The [D3D12 reference](../../experiments/d3d12/FRAME_INPUTS.md) recorded a verified workload on a native queue. The [Vulkan handoff test](../../tests/vulkan-handoff/README.md) covered separate queues and family ownership. The [public D3D12 component](D3D12_FG.md) checks the configured native completion fence. Installed Vulkan FG and combined SR → FG are covered by [#20](https://github.com/5headsmartbtw/leonida-framelab/pull/20) and [#27](https://github.com/5headsmartbtw/leonida-framelab/pull/27). Keep standard validation and SyncVal results separate. |
| Rust FFI | [Raw Rust bindings](../../bindings/rust/README.md) pass layout comparison against the C header and shared-library lifecycle tests on Windows and Linux. |
| Combined SR + FG | Drafts [#23](https://github.com/5headsmartbtw/leonida-framelab/pull/23) and [#27](https://github.com/5headsmartbtw/leonida-framelab/pull/27) cover generated readback, fallback and queue handoff on the RTX 2050. Extended SyncVal remains a release blocker. |
| Failure and recovery | [#26](https://github.com/5headsmartbtw/leonida-framelab/pull/26) covers deterministic submission rejection, cancellation and teardown. Keep the simulated and native failure results separate. Verify same-process recovery for runtimes that permit it. |
| Stability and benchmarking | [#25](https://github.com/5headsmartbtw/leonida-framelab/pull/25) tracks stepwise soak tests and a fixed-display benchmark. Keep readback, validation, physical display settings and Present counts explicit. |
| Tests and publication | Keep original FG regressions and portable CI passing. Record tested hardware, commands, failed/skipped checks and third-party source attribution. Keep proprietary binaries and game files outside the package. |

Leonida and VIctory integration proposals are optional consumers. Neither is a release build dependency.
