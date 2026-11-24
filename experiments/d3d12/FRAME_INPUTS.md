# D3D12 native FrameInputs regression

This standalone Windows reference evaluates NGX from validated `FrameInputs` and typed D3D12 bindings. The input profile is RGBA8 color, R32_FLOAT depth, RG16_FLOAT motion, equal native extents and a zero-jitter, Y-up camera. The output is a distinct RGBA8 UAV. All work is recorded in the producer's direct list on one queue. Bindings retain COM references and verify actual resource descriptions and device ownership. The producer supplies tracked resource states. The adapter does not infer resource state or perform cross-queue transitions.

```powershell
cmake -S experiments/d3d12 -B build-d3d12 -G 'Visual Studio 17 2022' -A x64 `
  '-DSTREAMLINE_ROOT_DIR=D:/SDKs/Streamline' '-DDLSS_NGX_SDK_ROOT=D:/SDKs/DLSS'
cmake --build build-d3d12 --config Release
& .\build-d3d12\Release\ngx_d3d12_fg.exe $runtimeDir $outputDir $proxyPath 6 present
```

The probe accepts `-` instead of a local proxy path for a native runtime profile. It returns an unsupported result when the real NGX capability query does not report FG available. The RTX 2050 run used the previously validated external SM86 profile and Streamline 2.14.1. The run rejected a stale-depth copy before NGX, completed six Evaluate/readback submissions at signaled fence values 2, 4, 7, 10, 13 and 16, and verified five nontrivial interpolations. Six real and five generated DXGI Present calls were accepted. These counts describe API calls. The D3D12 debug layer, display timing, asynchronous fences and cross-queue handoff were not tested.

This profile uses the native adapter internally. It is a regression for native dispatch before exposing D3D12 feature creation and dispatch through the public C ABI.
