# First experimental SDK release gates

The current release scope is incremental. The package and its ABI are tested separately from GPU hardware.

| Gate | Evidence or remaining work |
| --- | --- |
| Independent C and C++ consumers | Both compile with the installed include directory and link through `FrameLab::FrameLab`. The C test exercises an accepted packet through preparation, submission acknowledgement, retirement and final release. |
| Static and shared installs | Exercise both package variants on Windows and Linux in [SDK package CI](../../.github/workflows/sdk-package.yml). |
| Versioned ABI, lifecycle and safety | ABI v1 version and descriptor size checks. The [public contract](README.md) documents caller-side ordering, real completion and callback obligations. |
| Independent hardware-backed FG and SR | Existing Vulkan FG/Streamline SR profiles are sample paths. Extract each feature as an independently invoked SDK module, preserve readback evidence and keep native capability verdicts. |
| Vulkan and D3D12 consumers | Each backend must validate actual resources, state and submission ownership before recording. D3D12 and external Vulkan waits require independent synthetic GPU evidence. |
| Rust FFI | Verify layout against the C header and exercise installed-library lifecycle calls before a Rust integration. |
| Combined SR + FG | Require on-device readback, validation, resize/reset and fallback on a narrowly specified combined profile before declaring support. |
| Tests and publication | Keep original FG regressions and portable CI passing. Record tested hardware, commands, failed/skipped checks and third-party source attribution. Keep proprietary binaries and game files outside the package. |

Leonida and VIctory integration proposals are optional consumers. Neither is a release build dependency.
