# Standalone FrameLab D3D12 FG consumer

Build this directory as an external project against an installed SDK with the `D3D12FG` component. The C header test checks the exported ABI without running on a GPU. The C++ program uses public FrameLab and Windows SDK headers to upload a synthetic moving rectangle, invoke Direct NGX, and verify readback and fence-based lease retirement. With a presentation option it also presents the real and verified generated frames through an application-owned DXGI flip-model swapchain.

Use the [public D3D12 FG guide](../../docs/sdk/D3D12_FG.md) for installation. The original six-frame readback check:

```powershell
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy
```

Use `-` for the last argument when your runtime supports the native profile without a proxy. The application takes its genuine capability verdict from the runtime. The evidence directory receives the synthetic sixth-frame readback.

## DXGI presentation

Add `--vsync` (sync interval 1) or `--no-vsync` (sync interval 0, with `DXGI_PRESENT_ALLOW_TEARING` where the system supports it). The program opens a 1280×720 window and writes `d3d12-fg-summary.json` to the evidence directory.

| Option | Effect |
| --- | --- |
| `--frames N` | Source frames per segment, default 6. The object wraps every 25 positions at 720p; each wrap is a discontinuity with a history reset. |
| `--no-fg` | Ordinary rendering only. No NGX load or FrameLab backend. |
| `--reject-frame` | Gives source frame 2 a stale depth index. The frame is rejected before recording, presented as a real frame, and history restarts. Also `FRAMELAB_D3D12_REJECT_FRAME=1`. |
| `--cut-at N` | Moves the object at source frame N. The client invalidates history, checks that a frame without a reset is rejected, then records the frame with `FRAMELAB_RESET_CAMERA_CUT`. |
| `--resize` | Holds the queue behind an unsignalled fence, confirms that work is still outstanding, then drains it, resizes to 960×540, creates a new backend and runs another segment. Also `FRAMELAB_D3D12_RESIZE=1`. |
| `--debug` | Enables the D3D12 debug layer and fails on any error or warning message. Also `FRAMELAB_D3D12_DEBUG=1`. |
| `--timing` | Skips the sentinel clear and readback, and presents evaluated output without verification. Use it for interval measurements only. |
| `--no-pacing` | Presents a real frame immediately after its generated frame instead of holding it for the preceding real-to-generated interval, which splits each real-frame period evenly. |

[`tests/run_d3d12_fg_present_checks.ps1`](../../tests/run_d3d12_fg_present_checks.ps1) runs all cases in separate processes:

```powershell
.\tests\run_d3d12_fg_present_checks.ps1 -Executable .\build-client\Release\d3d12_fg.exe -RuntimeDirectory $runtimeDirectory -LogRoot .\framelab-logs
```

Each `DXGI_PRESENT` line names the source frame, the previous frame for a generated image, the backbuffer index, the fence value of the copy and the CPU interval since the last Present call. Present counters and intervals are submission evidence. The window's display mode is recorded, but the actual present mode (independent flip or composed) and physical scanout are not measured by the program; record them separately, for example with PresentMon, and mark them unmeasured otherwise.
