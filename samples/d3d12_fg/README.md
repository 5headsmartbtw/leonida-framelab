# Standalone FrameLab D3D12 FG consumer

Build this directory as an external project against an installed SDK with the `D3D12FG` component. The C header test checks the exported ABI without running on a GPU, including the `FrameLabD3D12FgConfig` offsets and the accepted and rejected config sizes. The C++ program uses public FrameLab and Windows SDK headers to upload a synthetic moving rectangle, invoke Direct NGX, and verify readback and fence-based lease retirement.

Use the [public D3D12 FG guide](../../docs/sdk/D3D12_FG.md) for installation. Launch with:

```powershell
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy --render 960x540
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy --color hdr10
```

Use `-` for the third argument when your runtime supports the native profile without a proxy. The application takes its genuine capability verdict from the runtime. The output is 1280×720. `--render` sets the depth and motion-vector extent (at most the output); without it, both extents are equal. `--color` selects the color and generated-image format: `rgba8` (default), `bgra8`, `rgb10a2` (SDR) or `hdr10` (RGB10A2 with PQ-encoded BT.2020 content). The scene is the same in every mode. For HDR10 its sRGB colors are shown at a 203 cd/m² reference white, converted to BT.2020 and PQ-encoded. `--debug-layer` enables the D3D12 debug layer, GPU-based validation and synchronized queue validation before the device is created (this needs the Windows Graphics Tools feature), writes every message to `d3d12-debug.log` in the evidence directory and prints a count per severity.

Before the run, the program creates probe backends on the device: configs of the v0.4.0 and the render-extent sizes are accepted without the later fields being read, the sizes around them are rejected, and invalid render extents and color configurations (FP16, HDR10 without RGB10A2, non-color or unknown formats, unknown flags) are rejected. If the device cannot run the selected color format, the program instead checks that create refuses it and exits with code 3. The first backend then records six frames. Frame 0 also checks that a wrong native state, a frame at another render extent, a depth resource of another size, frames in another color format or FP16, and color or generated resources of another format are rejected. The last submission is gated to check lease retention and that the busy backend cannot be destroyed. Afterwards the idle backend is destroyed and a second one is created at a different render extent; its first frame must carry a history reset. For every interpolated frame the program prints the rectangle's measured edges on the middle row next to the expected halfway position.

The evidence directory receives the synthetic sixth-frame readback as raw texels in the selected format. The sample does not send frames to a display.

[`tests/run_d3d12_fg_extent_checks.ps1`](../../tests/run_d3d12_fg_extent_checks.ps1) runs the program for each color mode at equal extents, 960×540 and 640×360 in separate processes and writes a JSON summary with the readback hashes:

```powershell
& .\tests\run_d3d12_fg_extent_checks.ps1 -Executable .\build-client\Release\d3d12_fg.exe -RuntimeDirectory $runtimeDirectory
& .\tests\run_d3d12_fg_extent_checks.ps1 -Executable .\build-client\Release\d3d12_fg.exe -RuntimeDirectory $runtimeDirectory -DebugLayer
```

With `-DebugLayer`, a case also fails on any corruption or error message from the debug layer.
