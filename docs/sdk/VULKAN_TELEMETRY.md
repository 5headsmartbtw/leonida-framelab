# Vulkan synthetic scene telemetry

Run the installed Vulkan FG/SR sample with a bounded frame count. The benchmark turns validation and diagnostic readback off. The soak enables standard validation, readback and alternating resize at quarter intervals. Collect SyncVal separately so its results are visible and its instrumented timings are separate.

~~~powershell
./tests/run_vulkan_profiles.ps1 -Executable $exe -Mode benchmark -Frames 180 -LogRoot ./benchmark
./tests/run_vulkan_profiles.ps1 -Executable $exe -Mode soak -Frames 10000 -LogRoot ./soak
~~~

The CSV records source frame intervals, summed Vulkan GPU timestamps for the recorded command buffers, real and generated Present calls, evaluation/error/fallback deltas, and actual render/output sizes and refresh. GPU timing covers the recorded work and excludes physical scanout. RSS is the Windows working set. VRAM is the Vulkan device-local heap usage estimate from VK_EXT_memory_budget. Both memory fields are sampled every 30 source frames. A GPU time of -1 means the timing query was unavailable. The CSV verified counter includes SR readbacks alongside verified generated images.

## First measured run

RTX 2050 Laptop, Windows 11 build 26200, NVIDIA 617.14, Vulkan SDK 1.4.363.1, Streamline 2.14.1, with the separately acquired FG reference stack documented in #24. All eight 180-frame benchmark processes exited successfully. The table excludes the first 30 frames of each process. Low means the reciprocal of the mean of the slowest 1% of source intervals. Output was 1280×720 throughout.

| Profile | Present mode / Hz | Render | Source FPS | Generated/s | Presents/s | Mean / P99 interval (ms) | 1% low | GPU ms / source | RSS MiB | Vulkan VRAM MiB |
| --- | --- | --- | ---: | ---: | ---: | --- | ---: | ---: | --- | ---: |
| Native | Immediate / 75 | 1280×720 | 175.39 | 0 | 175.39 | 5.70 / 13.41 | 67.17 | 0.11 | 388–395 | 100 |
| Native | FIFO / 75 | 1280×720 | 74.89 | 0 | 74.89 | 13.35 / 15.88 | 60.04 | 0.39 | 351–364 | 96 |
| SR | Immediate / 75 | 640×360 | 151.54 | 0 | 151.54 | 6.60 / 53.99 | 14.76 | 2.74 | 376–377 | 189 |
| SR | FIFO / 75 | 640×360 | 74.89 | 0 | 74.89 | 13.35 / 19.00 | 48.20 | 3.72 | 356–369 | 185 |
| FG | Immediate / 75 | 1280×720 | 112.29 | 112.29 | 224.59 | 8.91 / 90.55 | 8.44 | 5.04 | 117–121 | 247 |
| FG | FIFO / 75 | 1280×720 | 37.18 | 37.18 | 74.37 | 26.89 / 34.82 | 25.37 | 7.22 | 380–399 | 243 |
| SR + FG | Immediate / 144 | 640×360 | 94.61 | 94.61 | 189.22 | 10.57 / 11.98 | 24.40 | 7.68 | 438–446 | 335 |
| SR + FG | FIFO / 144 | 640×360 | 43.29 | 43.29 | 86.58 | 23.10 / 199.44 | 4.09 | 8.09 | 155–452 | 327 |

FrameLab measured API presents, not displayed frames. The monitor reported a different refresh for the combined rows, so the four-profile comparative benchmark still needs a rerun pinned to one display. The first standard-validation soak was interrupted at SR startup after a display incident. No complete 10,000-frame runs, soak allocation trends or soak stability results are available.
