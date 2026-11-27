# Synthetic SyncVal calibration

This standalone test supports [FL-17](https://github.com/5headsmartbtw/leonida-framelab/issues/32). It checks how a given Khronos validation layer and driver report the command shapes behind the [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24) diagnostics. The test records them on FrameLab-owned synthetic resources. It does not load NGX, Streamline or DLSS. A passing run says nothing about the native diagnostics on its own. Read it with the [triage procedure](../../docs/sdk/SYNCVAL_TRIAGE.md).

```sh
cmake -S tests/vulkan-syncval-control -B build-syncval
cmake --build build-syncval --config Release
ctest --test-dir build-syncval -C Release -V
```

A Vulkan 1.2 device, the Vulkan loader and headers and `VK_LAYER_KHRONOS_validation` are required. Synchronization2 cases need a Vulkan 1.3 device with that feature and are otherwise reported as `SYNCVAL_CONTROL_SKIP`. Use the standard loader variables to choose a layer build, for example `VK_LAYER_PATH`. Use `--device N` to choose an adapter, `--case NAME` to run one case and `--list` to print the matrix. Exit code 77 means that no layer or suitable device was found. CTest reports this as a skip.

The test enables synchronization validation, `syncval_message_extra_properties` and a duplicate-message limit of 10,000 through `VK_EXT_layer_settings`. Layers without that extension fall back to `VK_EXT_validation_features`. Each callback is printed in the reference sample's `SAMPLE_VALIDATION severity=...` form, so `scripts/syncval_report.py` reads both logs. The environment can still override these settings. With `VK_VALIDATION_VALIDATE_SYNC=0` every positive control fails and the test exits with 1.

## Cases

Each case runs in a `framelab.syncval.control` debug region on `framelab.syncval.control.image` (16×16 `R8G8B8A8_UINT`) or `framelab.syncval.control.buffer` (4 KiB). Positive controls intentionally omit a dependency. They must produce exactly one `SYNC-HAZARD-WRITE-AFTER-WRITE` and nothing else. When the layer prints extra properties, these must name the expected command, prior command and debug region. Positive controls that are only recorded are never executed. Negative controls must produce no message at all. They are submitted, and every pixel or word is checked after the queue goes idle.

| Case | #24 shape | Dependency before the second write | Expected |
| --- | --- | --- | --- |
| `image-transition-clear/sync1-transfer-write` | Layout transition, then clear | `TRANSFER` / `TRANSFER_WRITE` | Clean, readback |
| `image-transition-clear/sync2-clear-write` | Same | `CLEAR` / `TRANSFER_WRITE` | Clean, readback |
| `image-transition-clear/sync2-all-commands` | Same, `GENERAL` | `ALL_COMMANDS` / `MEMORY_WRITE` | Clean, readback |
| `image-transition-clear/sync1-no-dst-access` | Same | `TRANSFER` / no access | WAW |
| `image-transition-clear/sync1-bottom-of-pipe` | Same | `BOTTOM_OF_PIPE` / no access | WAW |
| `image-transition-clear/sync1-compute-write` | Same, `GENERAL` | `COMPUTE_SHADER` / `SHADER_WRITE` | WAW |
| `image-transition-clear/sync2-copy-stage` | Same | `COPY` / `TRANSFER_WRITE` | WAW |
| `buffer-fill-fill/no-barrier` | Fill, then fill | None | WAW |
| `buffer-fill-fill/transfer-barrier` | Same | `TRANSFER` / `TRANSFER_WRITE` | Clean, readback |
| `buffer-fill-fill/sync2-copy-stage` | Same | `COPY` / `TRANSFER_WRITE` on both sides | WAW from layer 1.3.280; clean before |
| `buffer-fill-fill/sync2-clear-stage` | Same | `CLEAR` / `TRANSFER_WRITE` on both sides | Clean from layer 1.3.280, readback; WAW before |
| `buffer-fill-submit/queue-order-only` | Fill in one submission, fill in the next | Submission order only | Submit-time WAW |
| `buffer-fill-submit/host-fence-wait` | Same | Host waits for the first fence | Clean, readback |
| `buffer-fill-submit/leading-barrier` | Same | Second submission starts with a `TRANSFER` barrier | Clean, readback |

The two `sync2-*-stage` fill cases have expectations that depend on the layer revision. Khronos layer commit [`8d93fca`](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/8d93fca0989bc63650a81fac339bede28c1185c8), first released in SDK 1.3.280.0, moved `vkCmdFillBuffer` and `vkCmdUpdateBuffer` from the COPY stage to `VK_PIPELINE_STAGE_2_CLEAR_BIT`. The Vulkan specification describes both commands under *Clear Commands*. Older layers therefore accept a COPY-only synchronization2 barrier before a fill and reject a CLEAR-only one. Synchronization1 `TRANSFER` covers both stages. On layer 1.3.275, the COPY-stage fill control intentionally records but does not submit the spec-invalid dependency: the older layer reports no hazard (`expect=none`), but no GPU execution or readback is attempted (`readback=n/a`). Its `PASS` records the expected diagnostic classification only, not valid synchronization or correct output.

## Results on a software device

These runs used Mesa lavapipe, not NVIDIA hardware, NGX or the #24 workload. They calibrate the layer and the tooling only.

| Layer | Device and driver | Result |
| --- | --- | --- |
| Khronos validation layer `vulkan-sdk-1.4.363.1` ([`317cff0`](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/317cff060893783f96d4174f50491e6374eb3714)), built from source without WSI | llvmpipe (LLVM 20.1.2), Mesa 25.2.8, Vulkan 1.4.318, Ubuntu 24.04 | 14/14 passed: 7 WAW positives, 7 clean negatives with correct readback, 0 messages outside cases, exit 0. Extra properties were present. [Raw log](../fixtures/syncval/control-lavapipe.log) |
| Khronos validation layer `v1.4.369` ([`6ca326f`](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/6ca326f85cd2563308e91b9839127387c00a170a)), built the same way | Same | 14/14 passed, exit 0. `syncval_report.py --compare` found the same 7 diagnostic signatures as 1.4.363.1. Only the internal `batch_tag` value differed. |
| Ubuntu `vulkan-validationlayers` 1.3.275.0-1 | Same | 14/14 passed with that revision's fill classification. It has no `VK_EXT_layer_settings`, so the test used `VK_EXT_validation_features`. The messages use the older `Access info (...)` format without extra properties. [Raw log](../fixtures/syncval/control-lavapipe-1.3.275.log) |

The fixture was compiled with GCC 13.3 and Clang 18 (`-Wall -Wextra`) against the Ubuntu Vulkan headers 1.3.275 and loader 1.3.275. In both runs, every positive control reported the prior write and its barrier scope as the case intended. The negative controls were clean. These results are consistent with correct reporting on this software device. They do not show how the layer behaves with NVIDIA drivers.
