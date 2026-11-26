# Vulkan NGX SyncVal triage (FL-17)

[FL-17](https://github.com/5headsmartbtw/leonida-framelab/issues/32) owns the root-cause work for the write-after-write diagnostics in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24). This page covers the NGX-free calibration fixture, the log tooling and what they established. It also lists the hardware runs that decide between the remaining hypotheses. **The responsible component is still unknown. The #24 release gate remains open.**

## Established evidence

The following comes from #24 and the FL-09 report. It was not rerun for this page.

- The RTX 2050 experimental profile and the official RTX 5090 runtime 310.9.1.0 both reported 8 errors in two-frame installed and legacy traces. All 8 were inside the two `NGX_VK_EVALUATE_DLSSG` calls. In the six-frame workload, readback was verified and each topology reported 11 errors. Standard validation reported 0 errors and 0 warnings in the tested cases. Both stacks used Vulkan SDK and validation layer 1.4.363.1.
- Every image diagnostic is a `vkCmdClearColorImage` WAW on an object the runtime names `nv.ngx.dlssg.resource`. The excerpt names the prior access as `vkCmdPipelineBarrier` in the `nv.ngx.dlssg.Evaluate` debug region. The six-frame workload adds `vkCmdFillBuffer` WAW diagnostics on buffers with the same name. Their excerpts give no prior command.
- FrameLab does not set either name or label. Neither string appears in the FrameLab sources at [`a27e66b`](https://github.com/5headsmartbtw/leonida-framelab/tree/a27e66b6a516695a74f21f1ab86ae709fdaa0347) or [`04a5120`](https://github.com/5headsmartbtw/leonida-framelab/tree/04a51207b32e412e9d5461d3c9ff3ee3a43f06e0). At `a27e66b` the sample's only Evaluate call site is in `src/backends/vulkan/direct_ngx_fg.hpp`. Before Evaluate, it records barriers only for FrameLab's color, depth, motion-vector and output images. The two-frame trace also omits FrameLab's diagnostic output clear.
- The published excerpts are sanitized one-line summaries. They omit the access, prior access and barrier scope that the layer printed, so they cannot show which part of which dependency is missing.

## What layer 1.4.363.1 reports

The layer builds each SyncVal message from the hazard state (`layers/sync/sync_reporting.cpp`, `FormatErrorMessage`). The [control fixture](../../tests/vulkan-syncval-control/README.md) produced each shape below on a software device:

| Message text | Recorded dependency | `syncval_report.py` | Control case |
| --- | --- | --- | --- |
| `No sufficient synchronization is present ...` | No scope covers the prior write, for example a destination access mask of 0 or no barrier | `none` | `sync1-no-dst-access`, `sync1-bottom-of-pipe`, `buffer-fill-fill/no-barrier` |
| `The current synchronization allows X, but ... it must allow A accesses at S.` | The destination scope exists but lacks access A at stage S | `access` | `sync1-compute-write` |
| `... it must allow these accesses at S.` | The access is in scope, but stage S is not | `stage` | `image-transition-clear/sync2-copy-stage` |
| `... (from VkCommandBuffer ... submitted on VkQueue ...)`, `message_type = SubmitTimeError` | The accesses are in different submissions | location `submit` | `buffer-fill-submit/queue-order-only` |

`syncval_message_extra_properties` appends `access`, `prior_access`, `write_barriers`, `read_barriers`, `prior_command`, `prior_debug_region`, `message_type` and sequence numbers. Set `VK_VALIDATION_SYNCVAL_MESSAGE_EXTRA_PROPERTIES=1` in the launch environment. The control run confirmed that layer 1.4.363.1 reads this variable and that it takes precedence over application settings. It changes only the message text. The pinned runners count callback lines that start with `SAMPLE_VALIDATION severity=`, so the multi-line messages do not change their counts.

## Findings from the calibration

These come from synthetic resources on Mesa lavapipe with layers 1.4.363.1 and 1.3.275 (see the [results](../../tests/vulkan-syncval-control/README.md#results-on-a-software-device)). They constrain the interpretation of #24 without testing NGX.

1. **One stage covers both commands.** Layer 1.4.363.1 reports `vkCmdClearColorImage` and `vkCmdFillBuffer` as `VK_PIPELINE_STAGE_2_CLEAR_BIT(VK_ACCESS_2_TRANSFER_WRITE_BIT)`. A dependency that orders either one must include that stage and access. Synchronization1 `TRANSFER` does, as do synchronization2 `CLEAR`, `ALL_TRANSFER` and `ALL_COMMANDS`.
2. **The excerpt points to synchronization1 barriers.** The layer names the entry point that recorded the barrier. In the controls, a synchronization2 barrier was reported as `vkCmdPipelineBarrier2`; the `KHR` alias has its own name. A synchronization1 barrier was reported as `vkCmdPipelineBarrier`. The #24 clear excerpts name `vkCmdPipelineBarrier`. If the sanitizer kept that name, each transition before a clear was recorded with synchronization1. Synchronization1 `TRANSFER` already includes the CLEAR stage, so a synchronization2 stage-granularity mistake cannot explain those diagnostics. That leaves a destination stage mask without `TRANSFER` or a destination access mask without `TRANSFER_WRITE`. The raw `write_barriers` value tells these apart: `0` means no destination access, and a compute or other scope means the wrong destination stage.
3. **Fill classification changed between layer revisions.** Layer commit [`8d93fca`](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/8d93fca0989bc63650a81fac339bede28c1185c8), first released in SDK 1.3.280.0, moved `vkCmdFillBuffer` and `vkCmdUpdateBuffer` from the COPY stage to `VK_PIPELINE_STAGE_2_CLEAR_BIT`. This matches the specification: `VK_PIPELINE_STAGE_2_CLEAR_BIT` covers the *Clear Commands* chapter, which includes filling and updating buffers. On layer 1.3.275 a COPY-only synchronization2 barrier between fills was accepted. A spec-conforming CLEAR-only barrier was reported as a WAW. Layer 1.4.363.1 did the reverse. Both #24 stacks used 1.4.363.1, so the old misclassification cannot have produced their fill diagnostics. A COPY-only synchronization2 barrier before the fills would produce them on 1.4.363.1 and not on 1.3.275.
4. **Submission order is not a memory dependency.** Two fills in consecutive submissions on one queue produced a submit-time WAW. A host fence wait between the submissions removed it, as did a leading barrier in the second submission.

## Log tooling

`scripts/syncval_report.py` reads raw stdout from the reference sample, the trace scripts or the control fixture. It accepts the 1.4-era and older message formats, sanitized summary lines and UTF-16 logs written by PowerShell redirection. Run it from a checkout of this branch against logs produced at the pinned revision:

```sh
python scripts/syncval_report.py logs/fg-trace-extra/installed/stdout.log --json installed.json
python scripts/syncval_report.py --compare logs/fg-trace-extra/installed/stdout.log logs/fg-trace-extra/legacy/stdout.log
```

It attributes each diagnostic to an `NGX_FG_NATIVE_BEGIN`/`END` window or a control case. Diagnostics are grouped by a signature that leaves out handles and sequence numbers. The tool flags summary/count mismatches, an unclosed evaluation window, duplicate-limit truncation, sanitized input, the older message format and missing extra properties. `--fail-on-diagnostics` and `--fail-on-change` exit with 1 for automation. The tool keeps the raw text and line number of every diagnostic. It describes the recorded dependency only and does not assign responsibility.

## Hypotheses

| ID | Hypothesis | Deciding evidence | Status |
| --- | --- | --- | --- |
| H1 | A synchronization1 barrier inside Evaluate transitions a private image without `TRANSFER` stage or `TRANSFER_WRITE` access in its destination scope and then clears it | Raw clear diagnostics show `prior_command = vkCmdPipelineBarrier`, `prior_debug_region = nv.ngx.dlssg.Evaluate` and `write_barriers` without `CLEAR_BIT(TRANSFER_WRITE)`. Same on the RTX 2050 profile and the official RTX 5090 runtime. The control passes on the same layer and driver. | Open: needs a raw trace (run 1) |
| H2 | False positive in the validation layer or its interaction with the driver | `write_barriers` already contains `VK_PIPELINE_STAGE_2_CLEAR_BIT(VK_ACCESS_2_TRANSFER_WRITE_BIT)`, a control negative case fails on that machine, or the result changes between layer revisions in a way the controls do not explain | Open: runs 1–3 |
| H3 | The private buffer fills conflict across evaluations in different submissions | Fill diagnostics show `message_type = SubmitTimeError` and a prior `vkCmdFillBuffer` from an earlier submission. A diagnostic-only wait for frame N's fence before frame N+1 removes the fill diagnostics but not the clears. The 5 fills in 6 evaluations fit this pattern but do not establish it. | Open: runs 1 and 4 |
| H4 | Device or feature configuration changes the recording path inside Evaluate | Vary one setting at a time: API version, `synchronization2`, queue family, installed versus legacy. The signature diff stays at zero or changes. | Open |
| H5 | FrameLab's barriers on its own inputs and output cause the hazard | A diagnostic names a FrameLab-owned resource, or the prior access lies outside Evaluate. The published diagnostics name private runtime objects in the runtime's region, which makes H5 less likely without excluding it. | Open, less likely |
| H6 | A synchronization2 barrier with `COPY` but not `CLEAR` precedes the fills | Fill diagnostics report a `COPY_BIT` scope, or report `write_barriers = 0` and disappear with layers before 1.3.280 while the clears remain | Open: runs 1 and 3 |

If the evidence points outside FrameLab, prepare a minimal reproducer, list affected and unaffected versions, and write an upstream-ready report. Leave #24 open for maintainer review. Global barriers, waits or suppression are diagnostic experiments only. The FL-17 gate rejects them as a fix unless the exact dependency is explained.

## Hardware runs

Use the pinned checkout, runtime and build commands from #24, for example [`a27e66b`](https://github.com/5headsmartbtw/leonida-framelab/tree/a27e66b6a516695a74f21f1ab86ae709fdaa0347) on an official runtime. Change one variable per run. Keep raw stdout, stderr and exit codes, and record the GPU, driver, runtime, layer, revision and exact commands. Keep the RTX 2050 experimental profile and official-runtime results separate.

1. **Raw scopes on layer 1.4.363.1.** In PowerShell:

   ```powershell
   $env:VK_VALIDATION_SYNCVAL_MESSAGE_EXTRA_PROPERTIES = '1'
   $env:VK_LAYER_DUPLICATE_MESSAGE_LIMIT = '10000'
   ./tests/trace_ngx_fg.ps1 -InstalledExecutable $fg -LegacyExecutable $legacy -LogRoot ./logs/fg-trace-extra
   ./tests/run_gpu_checks.ps1 -Executable $fg -VulkanSdkProfile same-stream -Scenario off -LogRoot ./logs/fg-syncval-extra -SynchronizationValidation
   Remove-Item Env:VK_VALIDATION_SYNCVAL_MESSAGE_EXTRA_PROPERTIES, Env:VK_LAYER_DUPLICATE_MESSAGE_LIMIT
   ```

   Then run `syncval_report.py` on each `stdout.log`. Record `write_barriers`, `prior_command`, `prior_debug_region` and `message_type` for every clear and fill. The runners still fail, as #24 requires.
2. **Calibration on the same machine.** Build and run [`tests/vulkan-syncval-control`](../../tests/vulkan-syncval-control/README.md) with the same driver and layer. Select the NVIDIA adapter with `--device N`.
3. **Layer revision only.** Repeat the six-frame SyncVal run with layers from SDK 1.3.275.0 or earlier, keeping everything else the same. Compare with `--compare`. Expect fill diagnostics to disappear under H6 and the clears to remain under H1.
4. **Cross-frame dependency only.** Run a diagnostic-only variant that waits for frame N's fence before recording frame N+1. Compare signatures. This is an experiment and must not ship as a fix.
