# Vulkan synthetic scene telemetry

Run the installed Vulkan FG/SR sample with a bounded frame count. The benchmark disables validation and diagnostic readback, places the window on one display and verifies its current refresh rate. The runner defaults to the primary display and accepts a Windows display name through `-MonitorDevice`. The soak enables standard validation and alternating resize at quarter intervals. The soak runner captures two consecutive FG source frames per 100-frame interval. The verifier compares the second generated image to both source images. SR captures the first evaluation at each extent and the final evaluation. Collect SyncVal separately so its results are visible and its instrumented timings are separate.

~~~powershell
./tests/run_vulkan_profiles.ps1 -Executable $exe -Mode benchmark -Frames 180 -LogRoot ./benchmark
./tests/run_vulkan_profiles.ps1 -Executable $exe -Mode soak -Frames 10000 -LogRoot ./soak
~~~

The CSV records source frame intervals, summed Vulkan GPU timestamps for the recorded command buffers, real and generated Present calls, evaluation/error/fallback deltas, and actual render/output sizes and refresh. GPU timing covers the recorded work and excludes physical scanout. RSS is the Windows working set. VRAM is the Vulkan device-local heap usage estimate from VK_EXT_memory_budget. Both memory fields are sampled every 30 source frames. A GPU time of -1 means the timing query was unavailable. The CSV verified counter includes SR readbacks alongside verified generated images.

## Fixed-display benchmark

RTX 2050 Laptop, Windows 11 build 26200, NVIDIA 617.14, Vulkan 1.4.363.1 and Streamline 2.14.1. Tested sample and SDK revision `400d7c7` with the reference FG runtime. All eight runs used `DISPLAY6` at 75 Hz and 1280×720 output. Each run recorded 180 source frames; the table excludes the first 30. All exited successfully with empty stderr. Validation and readback were off for timing. Present rates count API calls. The experiment did not measure scanout.

| Profile | VSync / Present | Render | Base FPS | Generated/s | Presents/s | Mean / P99 ms | 1% low FPS | GPU ms | RSS MiB | Vulkan VRAM MiB |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Native | Off / Immediate | 1280x720 | 179.90 | 0.00 | 179.90 | 5.56 / 6.14 | 161.17 | 0.11 | 403.9–404.0 | 99.9–99.9 |
| Native | On / FIFO | 1280x720 | 74.97 | 0.00 | 74.97 | 13.34 / 14.05 | 69.18 | 0.12 | 371.3–371.4 | 96.1–96.1 |
| SR | Off / Immediate | 640x360 | 177.42 | 0.00 | 177.42 | 5.64 / 6.96 | 130.11 | 2.68 | 384.6–384.7 | 189.1–189.1 |
| SR | On / FIFO | 640x360 | 74.93 | 0.00 | 74.93 | 13.35 / 14.27 | 69.66 | 3.18 | 381.5–381.6 | 185.3–185.3 |
| FG | Off / Immediate | 1280x720 | 144.08 | 144.08 | 288.17 | 6.94 / 8.09 | 121.21 | 5.03 | 417.0–417.0 | 246.8–246.8 |
| FG | On / FIFO | 1280x720 | 37.49 | 37.49 | 74.97 | 26.68 / 27.71 | 36.03 | 6.94 | 414.3–414.6 | 243.1–243.1 |
| SR + FG | Off / Immediate | 640x360 | 99.28 | 99.28 | 198.56 | 10.07 / 11.41 | 86.55 | 7.93 | 427.4–427.4 | 322.5–322.5 |
| SR + FG | On / FIFO | 640x360 | 37.48 | 37.48 | 74.97 | 26.68 / 27.65 | 36.16 | 8.63 | 422.7–422.8 | 318.7–318.7 |

## Historical run across two displays

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
