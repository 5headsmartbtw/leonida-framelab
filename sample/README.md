# Validated Windows reference sample

Imported from local `experiment/phase3-frame-inputs`, commit `b017e469d740e167dc45db95c67b0737d3d4594b`. Runtime C++ is preserved; only the CMake target name/include/output placement is adapted for this repository layout.

**The GPU reference still contains the original E:/Dev/GTA6 runtime/log/proxy paths.** This snapshot reproduces the tested setup; it is not a portable installer. Path configuration is tracked as a public roadmap item. The root CMake build runs independent contract tests by default and does not launch this sample.

External requirements: nvpro_core at `bc19d6ac3ef62938d0ea0e099735878457ce1b6e`, Streamline SDK 2.14.1, official DLSS SDK v310.9.1, existing SM86 0.4.2 local dependency with Bundled runtime 310.9.16, Vulkan SDK 1.4.363.1, MSVC 2022. Nothing downloads or installs these automatically.

With those dependencies present, enable `FRAMELAB_BUILD_SAMPLE`, pass `BASE_DIRECTORY` (the directory containing nvpro_core), `STREAMLINE_ROOT_DIR`, `DLSS_NGX_SDK_ROOT` and `VULKAN_SDK`. The executable is named vk_streamline and placed under the build tree's bin directory. Its runtime paths must match the validated setup until runtime configuration is refactored. Shader headers are generated from the included Apache-2.0 synthetic GLSL scene; no generated shader binaries are committed.

`--verify-direct-fg --frames 6 --validation --no-vsync` checks direct NGX FG. `--verify-sr` selects the earlier Streamline SR path. Combined SR + direct FG has not been validated. The VSync checkbox and physical V key toggle FIFO/Immediate; resize preserves the selection. The original Streamline FG plugin capability gate is not overridden.
