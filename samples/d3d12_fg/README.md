# Standalone FrameLab D3D12 FG consumer

Build this directory as an external project against an installed SDK with the `D3D12FG` component. The C header test checks the exported ABI without running on a GPU, including the `FrameLabD3D12FgConfig` offsets and the accepted and rejected config sizes. The C++ program uses public FrameLab and Windows SDK headers to upload a synthetic moving rectangle, invoke Direct NGX, and verify readback and fence-based lease retirement. With a presentation option the client also presents frames through an application-owned DXGI flip-model swapchain.

Use the [public D3D12 FG guide](../../docs/sdk/D3D12_FG.md) for installation. The original six-frame readback check:

```powershell
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy --render 960x540
```

Use `-` for the third argument when your runtime supports the native profile without a proxy. The application takes its genuine capability verdict from the runtime. The output is 1280×720. `--render` sets the depth and motion-vector extent (at most the output); without it, both extents are equal.

Before the run, the program creates probe backends on the device: a config of the v0.4.0 size is accepted, the sizes around it are rejected, and invalid render extents are rejected. The first backend then records six frames. Frame 0 also checks that a wrong native state, a frame at another render extent and a depth resource of another size are rejected. The last submission is gated to check lease retention and that the busy backend cannot be destroyed. Afterwards the idle backend is destroyed and a second one is created at a different render extent; its first frame must carry a history reset. For every interpolated frame the program prints the rectangle's measured edges on the middle row next to the expected halfway position.

The evidence directory receives the synthetic sixth-frame readback. The headless readback path opens no window.

[`tests/run_d3d12_fg_extent_checks.ps1`](../../tests/run_d3d12_fg_extent_checks.ps1) runs the program at equal extents, 960×540 and 640×360 in separate processes and writes a JSON summary with the readback hashes:

```powershell
& .\tests\run_d3d12_fg_extent_checks.ps1 -Executable .\build-client\Release\d3d12_fg.exe -RuntimeDirectory $runtimeDirectory
```

For a runtime profile that requires an app-local proxy, pass `-AppLocalProxy $appLocalProxy` to the runner.

## DXGI presentation

Add `--vsync` (sync interval 1) or `--no-vsync` (sync interval 0, with `DXGI_PRESENT_ALLOW_TEARING` where the system supports it). The program opens a 1280×720 window and writes `d3d12-fg-summary.json` to the evidence directory.

| Option | Effect |
| --- | --- |
| `--frames N` | Source frames per segment, default 6. The object wraps every 25 positions at 720p; each wrap is a discontinuity with a history reset. |
| `--no-fg` | Ordinary rendering only. No NGX load or FrameLab backend. |
| `--reject-frame` | Gives source frame 2 a stale depth index. The frame is rejected before recording, presented as a real frame, and history restarts. Also `FRAMELAB_D3D12_REJECT_FRAME=1`. |
| `--cut-at N` | Moves the object at source frame N. The client invalidates history, checks that a frame without a reset is rejected, then records the frame with `FRAMELAB_RESET_CAMERA_CUT`. |
| `--resize` | Gates the final FG packet and real backbuffer copy before resizing. After observing completion, the client drains Present, resets both sizes as needed and starts a new backend at 960×540 output. An explicit render extent is clamped to the new output. Also `FRAMELAB_D3D12_RESIZE=1`. |
| `--render WxH` | Selects the extent of depth and motion vectors. The color and swapchain sizes follow the output extent. |
| `--debug` | Enables the D3D12 debug layer and fails on any error or warning message. Also `FRAMELAB_D3D12_DEBUG=1`. Use `--gpu-validation` to enable GPU-based validation as well. |
| `--timing` | Skips the sentinel clear and readback. Records source intervals, GPU timestamps for source submissions and backbuffer copies, and a separate count of generated Presents without image verification. |
| `--no-pacing` | Skips the CPU delay after each generated Present. The default delay approximates the previous real-to-generated Present interval. |

The JSON summary records sampled peak VRAM, median post-warm-up VRAM, and the minimum local DXGI budget. Each segment compares live usage at warm-up and before backend teardown. The summary also reports usage after cleanup. Set `FRAMELAB_D3D12_FAIL_FG_ALLOCATION=1` in a separate correctness run to exercise ordinary rendering and cleanup after an optional FG allocation fails.

[`tests/run_d3d12_fg_present_checks.ps1`](../../tests/run_d3d12_fg_present_checks.ps1) runs all cases in separate processes:

```powershell
.\tests\run_d3d12_fg_present_checks.ps1 -Executable .\build-client\Release\d3d12_fg.exe -RuntimeDirectory $runtimeDirectory -LogRoot .\framelab-logs
```

Each `DXGI_PRESENT` line names the source frame, the previous frame for a generated image, the backbuffer index, the fence value of the copy and the CPU interval since the last Present call. Present counters and intervals are submission evidence. The window's display mode is recorded, but the actual present mode (independent flip or composed) and physical scanout are not measured by the program; record them separately and mark them unmeasured otherwise.

With `-PresentMon <PresentMon 2.x executable>`, the runner records each presentation case with PresentMon's ETW trace. This needs elevation or membership of the Performance Log Users group; without elevation PresentMon lists the client as `<unknown>`, so the runner selects its rows by process ID. Each case directory then holds `presentmon.csv` with only the client's rows (PresentMon 1.x metrics). The JSON report adds, per case: presents, displayed and dropped frames, the present modes, `AllowsTearing`, the sync interval, and p50/p95/min/max of `msBetweenPresents`, `msBetweenDisplayChange` and `msUntilDisplayed`. These come from DXGI and DWM events; physical scanout remains unmeasured.

```powershell
.\tests\run_d3d12_fg_present_checks.ps1 -Executable .\build-client\Release\d3d12_fg.exe -RuntimeDirectory $runtimeDirectory -LogRoot .\framelab-logs -PresentMon $presentMon
```
