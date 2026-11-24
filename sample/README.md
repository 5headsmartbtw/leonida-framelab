# Validated Windows reference sample

Imported from local `experiment/phase3-frame-inputs`, commit `b017e469d740e167dc45db95c67b0737d3d4594b`. The public sample takes local runtime paths from its launch environment.

See [build and reproduce](../docs/BUILD.md) for the dependency versions, Windows build commands, runtime settings and GPU runner. The root CMake build runs independent CPU contract tests by default. When the sample is enabled, the Visual Studio build puts `vk_streamline.exe` and the local SDK DLLs under `bin/Release` in the build directory. CMake generates headers from the included Apache-2.0 GLSL scene.

`--verify-direct-fg --frames 6 --validation --no-vsync` checks direct NGX FG. `--verify-sr` selects the earlier Streamline SR path. Combined SR + direct FG has not been validated. The VSync checkbox and physical V key toggle FIFO/Immediate; resize preserves the selection. The original Streamline FG plugin capability gate is not overridden.
