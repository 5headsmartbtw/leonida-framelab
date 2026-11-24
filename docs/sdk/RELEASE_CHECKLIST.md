# First experimental SDK release gates

The current release scope is incremental. The package and its ABI are tested separately from GPU hardware.

| Gate | Evidence or remaining work |
| --- | --- |
| Independent C and C++ consumers | Both compile with the installed include directory and link through `FrameLab::FrameLab`. The C test exercises an accepted packet through preparation, submission acknowledgement, retirement and final release. |
| Static and shared installs | Exercise both package variants on Windows and Linux in [SDK package CI](../../.github/workflows/sdk-package.yml). |
| Versioned ABI, lifecycle and safety | ABI v1 version and descriptor size checks. The [public contract](README.md) documents caller-side ordering, real completion and callback obligations. |
| Independent hardware-backed FG and SR | [D3D12 FG](D3D12_FG.md) runs on the reference profile through the installed SDK. The [installed Vulkan FG](VULKAN_FG.md) client passes readback and standard validation on the reference profile. Expanded synchronization validation and independent [SR-only](../evidence/sr-only-rtx2050.md) dispatch remain open. |
| Vulkan and D3D12 consumers | The [D3D12 reference](../../experiments/d3d12/FRAME_INPUTS.md) recorded a verified workload on a native queue. The [Vulkan handoff test](../../tests/vulkan-handoff/README.md) covered separate queues and family ownership. The [public D3D12 component](D3D12_FG.md) checks the configured native completion fence. The [public Vulkan component](VULKAN_FG.md) checks its actual GPU timeline. The installed consumer passed a real cross-family binary handoff. The two-queue same-family NGX test was skipped. |
| Rust FFI | [Raw Rust bindings](../../bindings/rust/README.md) pass layout comparison against the C header and shared-library lifecycle tests on Windows and Linux. |
| Combined SR + FG | Require on-device readback, validation, resize/reset and fallback on a narrowly specified combined profile before declaring support. |
| Tests and publication | Keep original FG regressions and portable CI passing. Record tested hardware, commands, failed/skipped checks and third-party source attribution. Keep proprietary binaries and game files outside the package. |

Leonida and VIctory integration proposals are optional consumers. Neither is a release build dependency.
