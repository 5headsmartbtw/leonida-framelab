# Standalone FrameLab D3D12 FG consumer

Build this directory as an external project against an installed SDK with the `D3D12FG` component. The C header test checks the exported ABI without running on a GPU, including the `FrameLabD3D12FgConfig` offsets and the accepted and rejected config sizes. The C++ program uses public FrameLab and Windows SDK headers to upload a synthetic moving rectangle, invoke Direct NGX, and verify readback and fence-based lease retirement.

Use the [public D3D12 FG guide](../../docs/sdk/D3D12_FG.md) for installation. Launch with:

```powershell
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy --render 960x540
```

Use `-` for the third argument when your runtime supports the native profile without a proxy. The application takes its genuine capability verdict from the runtime. The output is 1280×720. `--render` sets the depth and motion-vector extent (at most the output); without it, both extents are equal.

Before the run, the program creates probe backends on the device: a config of the v0.4.0 size is accepted, the sizes around it are rejected, and invalid render extents are rejected. The first backend then records six frames. Frame 0 also checks that a wrong native state, a frame at another render extent and a depth resource of another size are rejected. The last submission is gated to check lease retention and that the busy backend cannot be destroyed. Afterwards the idle backend is destroyed and a second one is created at a different render extent; its first frame must carry a history reset. For every interpolated frame the program prints the rectangle's measured edges on the middle row next to the expected halfway position.

The evidence directory receives the synthetic sixth-frame readback. The sample does not send frames to a display.

[`tests/run_d3d12_fg_extent_checks.ps1`](../../tests/run_d3d12_fg_extent_checks.ps1) runs the program at equal extents, 960×540 and 640×360 in separate processes and writes a JSON summary with the readback hashes:

```powershell
& .\tests\run_d3d12_fg_extent_checks.ps1 -Executable .\build-client\Release\d3d12_fg.exe -RuntimeDirectory $runtimeDirectory
```
