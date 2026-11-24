# Standalone FrameLab D3D12 FG consumer

Build this directory as an external project against an installed SDK with the `D3D12FG` component. The C header test checks the exported ABI without running on a GPU. The C++ program uses public FrameLab and Windows SDK headers to upload a synthetic moving rectangle, invoke Direct NGX, and verify readback and fence-based lease retirement.

Use the [public D3D12 FG guide](../../docs/sdk/D3D12_FG.md) for installation. Launch with:

```powershell
& .\build-client\Release\d3d12_fg.exe $runtimeDirectory $evidenceDirectory $appLocalProxy
```

Use `-` for the last argument when your runtime supports the native profile without a proxy. The application takes its genuine capability verdict from the runtime. The evidence directory receives the synthetic sixth-frame readback. The sample does not send frames to a display.
